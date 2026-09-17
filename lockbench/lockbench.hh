#pragma once

#include <random>
#include "lockbench/common.h"

static inline uint64_t cpu_ns() {
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

static inline uint64_t cpu_thread_ns() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}


static inline uint64_t cpu_time() {
#ifdef POSIX
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
#else
    return clock::get()->time();
#endif
}

/* =========================================================================
 * Part A: Correctness Harness
 * ========================================================================= */

struct ValidationState {
    alignas(64) std::atomic<int64_t> current_readers{0};
    alignas(64) std::atomic<int64_t> current_writers{0};
    alignas(64) std::atomic<int64_t> max_concurrent_readers{0};
    alignas(64) std::atomic<bool>    safety_violation{false};

    uint64_t shared_counter{0};
    uint64_t shared_shadow{0 ^ 0xDEADBEEFCAFEBABEULL};
};

template <class Lock>
void run_lockbench(Table &table,
                   size_t num_readers, size_t num_writers, size_t seconds,
                   double preempt_chance = 0, double writer_chance = 0) {

    LockbenchAdapter adapter; // OS adaptor
    std::string variant(__PRETTY_FUNCTION__);
    variant = variant.substr(variant.find("["));
    table
        .col("variant", variant)
        .col("cores", adapter.cpu_count)
        .col("readers", num_readers)
        .col("writers", num_writers)
        .col("runtime", seconds)
        .col("writer_chance", writer_chance)
        .col("preempt_chance", preempt_chance);

    std::cout << "Started" << variant
              << " with r=" << num_readers
              << ", w=" << num_writers
              << ", p=" << preempt_chance
              << std::endl;

    int preempt_chance_int = (int)preempt_chance * 10;

    Lock lock;
    ValidationState state;
    std::atomic<bool> start_flag{false};

    unsigned thread_count = 0;

    bool pinned = true;

    std::atomic<uint64_t> op_reads = 0;
    std::atomic<uint64_t> op_writes = 0;


    std::atomic<uint64_t> cpu_time_readers = 0;
    std::atomic<uint64_t> cpu_time_writers = 0;



    for (size_t i = 0; i < num_writers; ++i) {
        auto thread_id = thread_count ++;
        (void) thread_id;
        
        adapter.spawn_thread([&] {
            typename Lock::Node my_node;


            while (!start_flag.load(std::memory_order_acquire)) {
                CPU_PAUSE();
            }

            std::mt19937 rng;

            uint64_t ops = 0;

            auto start_cpu = cpu_thread_ns();

            while(start_flag.load(std::memory_order_acquire)) {
                lock.wlock(my_node);

                int64_t w = state.current_writers.fetch_add(1, std::memory_order_relaxed) + 1;
                int64_t r = state.current_readers.load(std::memory_order_relaxed);
                if (w != 1 || r != 0) {
                    state.safety_violation.store(true, std::memory_order_relaxed);
                }

                state.shared_counter++;
                state.shared_shadow = state.shared_counter ^ 0xDEADBEEFCAFEBABEULL;

                if (preempt_chance > 0 && (rng() % 1000) < preempt_chance_int) {
                    adapter.sched_yield();
                }

                state.current_writers.fetch_sub(1, std::memory_order_relaxed);
                lock.wunlock(my_node);

                ops += 1;
            }

            auto end_cpu = cpu_thread_ns();
            cpu_time_writers += (end_cpu - start_cpu);

            op_writes += ops;
        }, pinned);
    }

    for (size_t i = 0; i < num_readers; ++i) {
        auto thread_id = thread_count ++;
        (void) thread_id;

        adapter.spawn_thread([&] {
            typename Lock::Node my_node;

            while (!start_flag.load(std::memory_order_acquire)) {
                CPU_PAUSE();
            }

            std::mt19937 rng;
            uint64_t _op_reads = 0;
            uint64_t _op_writes = 0;

            auto start_cpu = cpu_thread_ns();

            int writer_chance_int = (int)(writer_chance * 100);

            while(start_flag.load(std::memory_order_acquire)) {
                if (writer_chance_int > 0 && (rng() % 10000) < writer_chance_int) {
                    lock.wlock(my_node);

                    int64_t w = state.current_writers.fetch_add(1, std::memory_order_relaxed) + 1;
                    int64_t r = state.current_readers.load(std::memory_order_relaxed);
                    if (w != 1 || r != 0) {
                        state.safety_violation.store(true, std::memory_order_relaxed);
                    }

                    state.shared_counter++;
                    state.shared_shadow = state.shared_counter ^ 0xDEADBEEFCAFEBABEULL;

                    state.current_writers.fetch_sub(1, std::memory_order_relaxed);
                    lock.wunlock(my_node);

                    _op_writes++;
                    continue;
                }

                lock.rlock(my_node);

                int64_t r = state.current_readers.fetch_add(1, std::memory_order_relaxed) + 1;
                int64_t w = state.current_writers.load(std::memory_order_relaxed);
                if (w != 0) {
                    state.safety_violation.store(true, std::memory_order_relaxed);
                }

                int64_t max_r = state.max_concurrent_readers.load(std::memory_order_relaxed);
                while (r > max_r && !state.max_concurrent_readers.compare_exchange_weak(
                           max_r, r, std::memory_order_relaxed)) {}

                uint64_t val = state.shared_counter;
                uint64_t shadow = state.shared_shadow;

                if ((val ^ 0xDEADBEEFCAFEBABEULL) != shadow) {
                    state.safety_violation.store(true, std::memory_order_relaxed);
                }

                _op_reads += 1;

                state.current_readers.fetch_sub(1, std::memory_order_relaxed);
                lock.runlock(my_node);

            }
            auto end_cpu = cpu_thread_ns();
            cpu_time_readers += (end_cpu - start_cpu);

            op_reads  += _op_reads;
            op_writes += _op_writes;

        }, pinned);
    }

    // Start all threads
    adapter.start_threads();

    uint64_t cpu_before = cpu_ns();
    uint64_t time_before = cpu_time();

    start_flag.store(true, std::memory_order_release);

    usleep(1000*1000*seconds);
    start_flag.store(false, std::memory_order_release);

    adapter.join_threads();
    
    uint64_t cpu_after = cpu_ns();
    uint64_t time_after = cpu_time();


    bool passed = !state.safety_violation.load();
    bool had_concurrency = (num_readers <= 1) || (state.max_concurrent_readers.load() > 1);

    std::cout << "  - Mutual Exclusion:        " << (passed ? "PASSED" : "FAILED") << "\n";
    std::cout << "  - Ops R/W: " << op_reads << "/" << op_writes << "\n";
    std::cout << "  - Max Concurrent Readers:  " << state.max_concurrent_readers.load()
              << (had_concurrency ? " (Verified Shared Access)" : " (No Concurrency Observed)") << "\n";
    std::cout << "  - Final Counter Value:     " << state.shared_counter
              << " (Expected: " << (op_writes) << ")\n";

    auto ops = op_reads + op_writes; // ((num_writers+num_readers)* ops_per_thread);
    auto cpu_time = cpu_after-cpu_before;
    auto wall_time = time_after-time_before;


    std::cout << "  - Lock Size" << sizeof(lock) << std::endl;
    std::cout << "  - CPU Time: " << cpu_time/1e9
              << ", per Op: " << (cpu_time/(ops)) << "ns"
              << ", R=" << cpu_time_readers.load()
              << ", W=" << cpu_time_writers.load()
              << "\n";
    std::cout << "  - Wall Time: " << wall_time/1e9
              << ", per Op: " << (wall_time/(ops)) << "ns"
              << "\n";
    std::cout << "  - Parallelism: " << cpu_time/(double)wall_time
              << "\n";

    table
        .col("lock_size", sizeof(lock))
        .col("op_reads", op_reads.load())
        .col("op_writes", op_writes.load())
        .col("cpu_time", cpu_time)
        .col("cpu_time_readers", cpu_time_readers.load())
        .col("cpu_time_writers", cpu_time_writers.load())
        .col("wall_time", wall_time);

    table.dump("!!");
    std::cout << "\n\n";

    assert(passed && "Correctness validation failed!");
    assert(state.shared_counter == op_writes && "Data integrity failed!");
}
