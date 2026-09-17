import pandas as pd
from io import StringIO
import sys
import matplotlib.pyplot as plt
from plydata import *
from plydata.tidy import *
from plotnine import *

def variants(df, *args):
    mask = None
    for x in args:
        if type(x) is tuple:
            m = (df.variant.str.contains(x[0]))
            m &= (df.backoff.str.contains(x[1]))
        else:
            m = (df.variant.str.contains(x))
        if mask is None:
            mask = m
        else:
            mask = mask | m
    return mask


def load_results(fn):
    lines = []
    with open(fn) as fd:
        for line in fd.readlines():
            if  line.startswith('!!'):
                lines.append(line[2:])

    fd = StringIO("\n".join(lines))

    df = pd.read_csv(fd, delimiter="\t")
    df.columns = [x.strip() for x in df.columns]

    df >>= define(ops="op_writes+op_reads",
                  threads="readers+writers")
    df >>= define(
                  parallelism='cpu_time/wall_time',
                  cpu_per_op="cpu_time/ops",
                  wall_per_op="wall_time/ops",
                  active_cores=if_else('threads > cores', 'cores', 'threads')
                  )
    df >>= extract('variant', regex=r'\[(?:with )?Lock\s*=\s*(.*?)[\];]',
                   into='variant')
    df['variant'] = df.variant.replace("WithoutPreemption<(.*)>", "w/o \\1", regex=True)
    df >>= extract('variant', regex=r'(.*?)(?:<(.*)>)?$',
                   into=['variant', 'backoff'])
    df['variant'] = df.variant.replace("w/o (.*)", "\\1 w/o preempt.", regex=True)

    df['backoff'] = df.backoff.str.replace("AutoTune<>", "A")
    df['backoff'] = df.backoff.str.replace("Exponential<>", "E")
    df['backoff'] = df.backoff.str.strip()

    df['backoff'] = df.backoff.fillna("BackoffNone")
    if 'posix' in fn:
        df['variant'] = df.variant.map(lambda s: "Linux/" + s)

        df['variant'] = df.variant.str.replace("StdMutex", "std::mutex")
        df['variant'] = df.variant.str.replace("StdRwMutex", "std::shared_mutex")


    return df

dfs = [load_results(fn) for fn in sys.argv[1:]]

df_posix = load_results("results/results-posix")
dfs.append(df_posix[variants(df_posix, "std::")])
df = pd.concat(dfs)

print(df)

all_variants = df['variant'].unique().tolist()
cmap = plt.get_cmap('tab10')
def to_html(color):
    color = ("%02x" % int(x * 255) for x in color)
    color = "#" + "".join(color)
    print(color)
    return color
palette = [to_html(cmap(i)) for i in range(0, len(all_variants))]
variant_color_map = dict(zip(all_variants, palette))


def plot(df,y="per_op"):
    if len(df) == 0:
        return
    df_long = (df
               >> query("readers == 0")
           >> pivot_longer(['cpu_per_op', 'wall_per_op'],
                           values_to='per_op',
                           names_pattern="(.*)_per_op",
                           names_to='timescale')
    )
    
    g = (ggplot(df_long)
         + aes(x="threads", y="per_op", color="variant", shape='backoff')
         + geom_point() +geom_line()
         + facet_wrap('~ preempt_chance + timescale', scales='free_y', ncol=2,
                      labeller="label_both")
         # + facet_grid("preempt_chance ~ timescale", scales='free_y')
         + labs(subtitle="VM mit 8 Kernen", y="Time per OP [ns]")
         # + scale_color_manual(values=variant_color_map)
         +theme(
             figure_size=(10,10)
         )
         )
    return g

plots = []

g = plot(df) + scale_y_log10()
g += ggtitle("Alle Daten, Logscale")
plots.append(g)


V = variants(df, "OSV::", "std::", "StdMutex", "StdRwMutex", "PrLock")

g = plot(df[V])
if g:
    g += ggtitle("Schlafende Locks")
plots.append(g)

V2 = variants(df, "TATAS", "TicketLock", "MCSLock")
g = plot(df[V2])
if g:
    g += ggtitle("Spinlocks")
plots.append(g)


V = V | variants(df, 
             ("TATAS", "BackoffA"),
             ("TATAS", "BackoffE")
             )


g = plot(df[(df.threads <= 8) & V])
if g:
    g += ggtitle("Nur bis 8 Threads")
plots.append(g)


g = plot(df[variants(df, "StdMutex",
                     "OSV::mutex", "std::",
                     ("PrLock", "BackoffE"),
                     ("TATAS w/o", "BackoffE"),
                     ("TicketLock w/o"),
                     ("MCSLock w/o"))
            ])
if g:
    g += ggtitle("Lock Performance")
plots.append(g)

rw_locks = variants(df,
                     "StdRwMutex",
                     "OSV::rwlock",
                     "std::shared_mutex"
                     )

g = plot(df[rw_locks])
if g:
    g += ggtitle("Reader/Writer Lock")


def plot_rwlock(df):
    df = df.copy()
    df['latency_reads']  = df.cpu_time_readers / df.op_reads;
    df['latency_writes'] = df.cpu_time_writers / df.op_writes;

    df >>= query("(readers > 0)")
    if len(df) == 0:
        return

    df_long = (df
        >> pivot_longer(['op_reads', 'op_writes'],
                           values_to='op_count',
                           names_pattern="op_(.*)",
                           names_to='op_type')
    )

    g = (ggplot(df_long)
         + aes(x="writer_chance", y="op_count/(wall_time/1e9)", color="variant")
         + geom_point() +geom_line()
         + labs(subtitle="VM mit 8 Kernen")
         + facet_grid("threads ~ op_type", scales='free_y', labeller='label_both')
         + labs(y="OPs per Second")
         + scale_x_log10()
         +theme(
             figure_size=(10,10)
         )
         )
    return g

g = plot_rwlock(df[rw_locks])
if g:
    g += ggtitle("RW-Lock Performance")

plots += [g]




g =(ggplot(df[V & (df.readers == 0)])
    + aes(x="factor(threads)", y="parallelism/active_cores", color="variant",
          shape="backoff",
          group='variant+backoff'
          )
     + geom_point() + geom_line()
     + facet_grid("preempt_chance ~ ", scales='free')
     + ggtitle("CPU Nutzung")
     + labs(y='Anteil Aktiver Cores (von 8)')
     + theme(
         figure_size=(10,10)
     )
    )
plots +=[g]


lock_size = df >> group_by('variant') >> summarize(lock_size='lock_size.max()')

print(lock_size)

g = (
    ggplot(lock_size, aes(x="variant", y="lock_size"))
    + geom_col(fill="#4C72B0", width=0.7)
    + geom_text(aes(label="lock_size"), ha="left", nudge_y=1.2, size=9)
    + coord_flip()
    + scale_y_continuous(expand=(0, 0, 0.1, 0))
    + labs(x=None, y="Lock Size (Bytes)")
    + theme_classic()
    + theme(
        axis_text=element_text(color="black", size=9),
        axis_title=element_text(color="black", size=10),
        panel_grid_major_x=element_line(color="#e0e0e0", linetype="dotted"),
    )
    )
plots +=[g]



save_as_pdf_pages([x for x in plots if x], 'plot.pdf')

# pd.set_option('display.float_format', '{:,.2f}'.format)
