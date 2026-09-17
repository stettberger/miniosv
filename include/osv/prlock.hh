#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <climits>
#include <osv/preempt-lock.hh>
#include <osv/inline-futex.hh>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__)
#define CPU_PAUSE() asm volatile("yield" ::: "memory")
#else
#define CPU_PAUSE() std::this_thread::yield()
#endif




template <class Backoff=BackoffExponential<>>
class PrLock : public lock_interface {
    std::atomic<uint32_t>       count;
    std::atomic<uint32_t>  preempted{0};
    inline_futex writers;

public:
    struct Node : public lock_interface::Node {
        uint32_t lock_mask;
    };
    
    constexpr PrLock() noexcept = default;
    ~PrLock() = default;

    PrLock(const PrLock&) = delete;
    PrLock& operator=(const PrLock&) = delete;

    /* =========================================================================
     * Writer (Exclusive)
     * ========================================================================= */
    void wlock(Node& node) noexcept {
        sched::thread *me = sched::thread::current();
        auto thread_id =  me->id();

        uint32_t cur_preempted = 0;
        uint32_t cur_count = 0;

        Backoff backoff;

        preempt_lock.prepare();

        // Fast Path, 0-> 1
        preempt_lock.lock();
        if (count.compare_exchange_weak(
                cur_count, 1,
                std::memory_order_acquire)) {
            goto lock_success;
        }
        preempt_lock.unlock();


        node.lock_mask = (1 << ((thread_id % 30) + 2));
        assert((node.lock_mask & 3) == 0);
        assert(node.lock_mask != 0);
        assert(__builtin_popcount(node.lock_mask) == 1);


        while (1) {
            cur_preempted = preempted.load(std::memory_order_relaxed);
            cur_count = count.load(std::memory_order_relaxed);
            if (cur_preempted > 0 || (cur_count == ~1U)) {
                uint32_t mode_2 = ~1U;
                preempt_lock.lock();
                if (count.exchange(mode_2, std::memory_order_acquire) == 0)
                    goto lock_success;
                preempt_lock.unlock();

                writers.wait(count, mode_2);
            } else {
                auto subscribers = __builtin_popcount(cur_count & ~3);
                if (subscribers == 0 || (cur_count & node.lock_mask)) {

                    uint32_t update = 2 | node.lock_mask;
                    backoff.attempt_new();
                    preempt_lock.lock();
                    if (count.exchange(update, std::memory_order_acquire) == 0)
                        goto lock_success;
                    preempt_lock.unlock();
                    backoff.attempt_fail();
                } else {
                    writers.wait(count, cur_count);
                }
            }

            backoff.sleep();
        }

    lock_success:
        me->push_lock(&this->preempted);
        preempt_lock.unlock();

        return;
    }

    void wunlock(Node& node) noexcept {
        sched::thread *me = sched::thread::current();
        preempt_lock.prepare();
        preempt_lock.lock();

        bool preempted = !me->pop_lock(&this->preempted);
        (void) preempted;
        uint32_t cur_count  = count.exchange(0, std::memory_order_release);
        preempt_lock.unlock();

        if (cur_count == 1) {
            return;
        }

        if (preempted || (cur_count == ~1U) || (cur_count == (2U | node.lock_mask))) {
            writers.wake(count);
        } else {
            // printf("cur_count=%x node.lock_mask=%x\n", cur_count, node.lock_mask);
        }
    }
};


