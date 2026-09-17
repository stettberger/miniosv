#pragma once

#include <vector>
#include <osv/sched.hh>
#include <osv/inline-futex.hh>

extern sched::thread ** threads;

class LockbenchAdapter {
    std::vector<sched::thread*> threads;

public:
    unsigned cpu_count;

    LockbenchAdapter() : cpu_count(sched::cpus.size()) {}

    template<class Action>
    void spawn_thread(Action action, bool pinned) {
        unsigned thread_id = threads.size();

        sched::thread::attr attr;
        if (pinned) {
            attr.pin(sched::cpus[(thread_id + 1) % cpu_count]);
        }

        threads.push_back(sched::thread::make(std::move(action), attr));
        ::threads = threads.data();
    }

    void sched_yield() {
        sched::thread::yield();
    }

    void start_threads() {
        for (auto *th : threads) {
            th->start();
        }
    }

    void join_threads() {
        for (auto *th : threads) {
            th->join();
            delete th;
        }
        threads.clear();
    }

    // Futex interface
    static int futex_wait(std::atomic<uint32_t> &uaddr, uint32_t expected) {
        auto q = inline_futex::get((void*)&uaddr);
        q.wait(uaddr, expected);
        return 0;
    }

    static int futex_wake(std::atomic<uint32_t> &uaddr, int count = 1) {
        auto q = inline_futex::get((void*)&uaddr);
        return q.wake(count);
    }
};
