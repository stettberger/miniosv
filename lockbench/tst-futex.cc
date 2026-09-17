#include <vector>
#ifndef POSIX
#include "lockbench/osv-common.hh"
#else
#include "lockbench/posix-common.hh"
#endif


#define ITERATIONS 100000

int test_futex(unsigned num_threads) {
    LockbenchAdapter adapter;

    std::atomic<unsigned> turn = 0;

    num_threads = 2;

     for (unsigned tid = 0; tid < num_threads; ++tid) {
        adapter.spawn_thread([tid, num_threads, &adapter, &turn]() {
            for (int i = 0; i < ITERATIONS; ++i) {
                unsigned cur_turn;
                while ((cur_turn = turn.load(std::memory_order_acquire)) != tid) {
                    adapter.futex_wait(turn, cur_turn);
                }

                uint32_t next = (tid + 1) % num_threads;

                turn.store(next, std::memory_order_release);
                adapter.futex_wake(turn, 1);
            }
        }, true);
    }

    auto start = std::chrono::steady_clock::now();

    adapter.start_threads();
    adapter.join_threads();

    auto end = std::chrono::steady_clock::now();

    double total_ns = std::chrono::duration<double, std::nano>(end - start).count();
    double total_handoffs = static_cast<double>(ITERATIONS) * num_threads;
    double handoff_ns = total_ns / total_handoffs;
    double round_trip_ns = handoff_ns * num_threads;

    std::printf("Threads:         %u\n", num_threads);
    std::printf("Iterations:      %d full rounds (%lu handoffs)\n",
                ITERATIONS, static_cast<unsigned long>(total_handoffs));
    std::printf("Total time:      %.2f ms\n", total_ns / 1e6);
    std::printf("Round-trip:      %.0f ns (%.2f µs)\n", round_trip_ns, round_trip_ns / 1000.0);
    std::printf("One-way handoff: %.0f ns (%.2f µs)\n", handoff_ns, handoff_ns / 1000.0);

    return 0;
}

#ifdef POSIX
int main() {
    test_futex(2);
    return 0;
}
#endif
