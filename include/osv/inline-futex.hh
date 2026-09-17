#pragma once

#include <atomic>
#include <cassert>
#include <osv/sched.hh>
#include <osv/spinlocks.hh>
#include <osv/preempt-lock.hh>
#include <osv/wait_record.hh>
#include <osv/intrusive-queue.hh>


class inline_futex {
    struct futex_node : public wait_record {
        futex_node(sched::thread *me, uintptr_t key, uint32_t mask)
            : wait_record(me), key(key), mask(mask) {}

        uintptr_t key;
        uint32_t mask;
    };

    using waitqueue_t = intrusive_queue<wait_record>;
    using wq_lock = InPlaceMCSLock<waitqueue_t, waitqueue_t::inplace_lock_member>;

    waitqueue_t waitqueue;

public:

    bool empty() {
        return waitqueue.empty();
    }

    template<class value>
    void wait(std::atomic<value> &addr, value expected, uintptr_t key = 0, uint32_t mask=~0) {
        sched::thread* me = sched::thread::current();
        bool must_wait = false;
        futex_node waiter(me, key, mask);

        preempt_lock.prepare();
        preempt_lock.lock();

        wq_lock::Node node;
        wq_lock::wlock(waitqueue, node);
        {
            if (addr.load(std::memory_order_acquire) == expected) {
                node.val.push(&waiter);
                must_wait = true;
            }
        }
        wq_lock::wunlock(waitqueue, node);
        preempt_lock.unlock();

        if (must_wait) {
            waiter.wait();
        }
    }

    void wait_always(uintptr_t key = 0, uint32_t mask=~0) {
        std::atomic<int> foo = 0;
        wait(foo, 1, key, mask);
    }

    // Traverses FIFO, selectively unlinks up to `count` matching nodes,
    // and wakes them outside the MCS and preempt locks.
    unsigned wake(uintptr_t key = 0, uint32_t mask_pattern = ~0U,  unsigned count = 1) {
        if (count == 0 || mask_pattern == 0) {
            return 0;
        }

        wait_record* wake_head = nullptr;
        wait_record** wake_tail = &wake_head;
        unsigned woken_count = 0;

        preempt_lock.lock();
        wq_lock::Node node;
        wq_lock::wlock(waitqueue, node);
        {
            waitqueue_t &waitqueue = node.val;

            wait_record* pred = nullptr;
            wait_record* curr = waitqueue.next(nullptr);

            while (curr && woken_count < count) {
                // If curr has no next, advancing next(curr) pulls from push_list on demand
                if (!curr->next) {
                    waitqueue.next(curr);
                }

                futex_node *curr_n = (futex_node *)curr;

                if (((curr_n->key == key) || (key == 0))
                    && (curr_n->mask & mask_pattern) != 0) {
                    wait_record* to_wake = curr;
                    curr = curr->next;

                    // Unlink to_wake from waitqueue
                    if (!pred) {
                        // Unlinking the head of pop_list (and waitqueue)
                        node.val.pop();
                    } else {
                        pred->next = curr;
                    }

                    // Append to isolated wake list
                    to_wake->next = nullptr;
                    *wake_tail = to_wake;
                    wake_tail = &to_wake->next;
                    woken_count++;
                } else {
                    pred = curr;
                    curr = curr->next;
                }
            }
        }
        wq_lock::wunlock(waitqueue, node);

        preempt_lock.unlock();

        // Wake threads outside the critical section
        wait_record* it = wake_head;
        while (it) {
            wait_record* next_node = it->next;
            assert(!it->woken());
            it->wake();
            it = next_node;
        }

        return woken_count;
    }

    unsigned wake_all(uintptr_t key = ~0, uint32_t mask_pattern = ~0U) {
        return wake(key, mask_pattern, std::numeric_limits<unsigned>::max());
    }

    class ref {
        inline_futex &queue;
        uintptr_t key;

    public:
        ref(inline_futex &q, uintptr_t key=0) : queue(q), key(key) {}

        template<typename value>
        void wait(std::atomic<value> &addr, value expected, uint32_t mask=~0) {
            return queue.wait(addr, expected, this->key, mask);
        }

        void wait_always(uint32_t mask=~0) {
            return queue.wait_always(this->key, mask);
        }

        unsigned wake(uint32_t mask = ~0U, unsigned count=1) {
            return queue.wake(this->key, mask, count);
        }

        unsigned wake_all(uint32_t mask = ~0U) {
            return queue.wake_all(this->key, mask);
        }

        bool empty() {
            return queue.empty();
        }
    };

    static ref get(void* addr);
};

