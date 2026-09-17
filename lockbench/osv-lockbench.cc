/*
 * OSv Reader-Writer Lock Benchmarkc
  */

#include <osv/sched.hh>
#include <osv/rwlock.h>
#include <osv/clock.hh>
#include <atomic>
#include <vector>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <unistd.h>

#include <osv/stdmutex.hh>
#include <osv/stdrwmutex.hh>
#include <osv/stdrwmutex2.hh>
#include <osv/spinlocks.hh>
#include <osv/prlock.hh>


#include "lockbench/osv-common.hh"
#include "lockbench/lockbench.hh"

sched::thread ** threads;

namespace OSV {
    class rwlock : public ::rwlock, public lock_interface {
    public:
        void rlock(Node &) { ::rwlock::rlock(); }
        void wlock(Node &) { ::rwlock::wlock(); }

        void runlock(Node &) { ::rwlock::runlock(); }
        void wunlock(Node &) { ::rwlock::wunlock(); }
    };

    class mutex : public ::mutex, public lock_interface {
    public:
        void wlock(Node &) { ::mutex::lock(); }
        void wunlock(Node &) { ::mutex::unlock(); }
    };
};

template <class Lock>
struct WithoutPreemption : public Lock {
    using Node = Lock::Node;

    void rlock(Node & n) { preempt_lock.lock(); Lock::rlock(n); }
    void wlock(Node & n) { preempt_lock.lock(); Lock::wlock(n); }

    void runlock(Node & n) { Lock::runlock(n); preempt_lock.unlock();  }
    void wunlock(Node & n) { Lock::wunlock(n); preempt_lock.unlock(); }
};

struct BackoffE : public BackoffExponential<> {};
struct BackoffA : public BackoffAutoTune<> {};


int test_futex(unsigned);

extern "C"
int osv_app_main()
{
    Table table;
    printf("OSv McsRwLock Benchmark\n");
    printf("===================================\n");

    test_futex(2);


    std::vector<int> wait_percentages = {0, 1, 2, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    std::vector<int> all_writers = {
        1, 32, 16, 8, 24, 4, 12, 20, 28, 2, 6, 10, 14, 18, 22, 26, 30,
        3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31
    };

    std::vector<int> all_writers_short = {1, 2, 3, 4, 5, 6, 8, 12, 16, 20, 24, 28, 32};
    std::vector<int> wait_percentages_short = {0, 1, 10, 50, 100};

    bool Short = true;
    unsigned seconds = Short ? 1 : 5;


    for (int writers : *(Short  ? &all_writers_short      : &all_writers)) {
        for (int wait : *(Short ? &wait_percentages_short : &wait_percentages)) {

            // We want to measure the different mutexes
            run_lockbench<OSV::mutex>             (table, 0, writers, seconds, wait);
            run_lockbench<StdMutex>(table, 0, writers, seconds, wait);
            run_lockbench<StdMutexTiny>(table, 0, writers, seconds, wait);

            // We are also intereted in the lock performance of rw-locks
            run_lockbench<StdRwMutex>(table, 0, writers, seconds, wait);
            run_lockbench<StdRwMutex2>  (table, 0, writers, seconds, wait);
            run_lockbench<OSV::rwlock> (table, 0, writers, seconds, wait);

            run_lockbench<PrLock<BackoffNone>>(table, 0, writers, seconds, wait);
            run_lockbench<PrLock<BackoffE>>(table, 0, writers, seconds, wait);
            run_lockbench<PrLock<BackoffA>>(table, 0, writers, seconds, wait);

            run_lockbench<TATAS<BackoffNone>> (table, 0, writers, seconds, wait);
            run_lockbench<TATAS<BackoffE>> (table, 0, writers, seconds, wait);
            run_lockbench<TATAS<BackoffA>> (table, 0, writers, seconds, wait);
            run_lockbench<TicketLock>             (table, 0, writers, seconds, wait);
            run_lockbench<MCSLock>                (table, 0, writers, seconds, wait);

            if (wait == 0) { // This only makes sense without blocking
                run_lockbench<WithoutPreemption<TATAS<BackoffNone>>> (table, 0, writers, seconds, wait);
                run_lockbench<WithoutPreemption<TATAS<BackoffE>>>    (table, 0, writers, seconds, wait);
                run_lockbench<WithoutPreemption<TATAS<BackoffA>>>    (table, 0, writers, seconds, wait);
                run_lockbench<WithoutPreemption<TicketLock>>         (table, 0, writers, seconds, wait);
                run_lockbench<WithoutPreemption<MCSLock>>            (table, 0, writers, seconds, wait);
            }

        }
    }

    // Benchmark 2: Readers that sometimes do a write
    std::vector<double> writer_chance = {0, 0.001, 0.01, 0.1, 1, 10, 20, 50, 99};
    std::vector<double> reader_threads = {1,2,4,8,16,32};

    for (double wc : writer_chance) {
        for (int readers : reader_threads) {
            run_lockbench<StdRwMutex>  (table, readers, 0, seconds, 0, wc);
            run_lockbench<StdRwMutex2>  (table, readers, 0, seconds, 0, wc);
            run_lockbench<OSV::rwlock>  (table, readers, 0, seconds, 0, wc);
        }
    }
    


    return 0;
}
