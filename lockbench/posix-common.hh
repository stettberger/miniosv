#pragma once
#include <atomic>
#include <vector>
#include <pthread.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>



class LockbenchAdapter {
    std::vector<std::thread> threads;

public:
    unsigned cpu_count;

LockbenchAdapter() : cpu_count(sysconf(_SC_NPROCESSORS_ONLN)) {}

    template<class Action>
        void spawn_thread(Action action, bool pinned) {
        unsigned thread_id = threads.size();
        threads.emplace_back([this, thread_id, action,pinned] () {
                if (pinned) {
                    cpu_set_t cpuset;
                    CPU_ZERO(&cpuset);
                    CPU_SET(thread_id % this->cpu_count, &cpuset);
                    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
                }

                action();
            });
    }

    void sched_yield() {
        ::sched_yield();
    }

    void start_threads() {}

    void join_threads() {
        for (auto &th : threads) {
            th.join();
        }
    }

    // Futex interface
    static int futex_wait(const std::atomic<uint32_t> &uaddr, uint32_t expected) {
        int op = FUTEX_WAIT_PRIVATE;
        return syscall(SYS_futex, const_cast<std::atomic<uint32_t> *>(&uaddr), op, expected, nullptr, nullptr, 0);
    }

    static int futex_wake(const std::atomic<uint32_t> &uaddr, int count = 1) {
        int op = FUTEX_WAKE_PRIVATE;
        return syscall(SYS_futex, const_cast<std::atomic<uint32_t> *>(&uaddr), op, count, nullptr, nullptr, 0);
    }
};
