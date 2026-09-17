#pragma once

#include <atomic>

namespace lockfree {
    
template <class LT>
class stack {
private:
    std::atomic<LT*> head;

public:
    constexpr stack() : head(nullptr) {}

    ~stack() = default;
    stack(const stack&) = delete;
    stack& operator=(const stack&) = delete;

    inline void push(LT* item)
    {
        LT* old = head.load(std::memory_order_relaxed);
        do {
            item->next = old;
        } while (!head.compare_exchange_weak(old, item,
                                             std::memory_order_release,
                                             std::memory_order_relaxed));
    }

    // Atomically claims all nodes currently in the stack.
    // Safe under concurrent pushers.
    inline LT* popall()
    {
        return head.exchange(nullptr, std::memory_order_acquire);
    }

    // Single-consumer pop.
    // Relies on CAS loop to safely peel off the top node without races 
    // against concurrent push operations.
    inline LT* pop()
    {
        LT* old = head.load(std::memory_order_acquire);
        while (old) {
            LT* next = old->next;
            if (head.compare_exchange_weak(old, next,
                                           std::memory_order_acquire,
                                           std::memory_order_acquire)) {
                old->next = nullptr;
                return old;
            }
        }
        return nullptr;
    }

    inline bool empty() const
    {
        return head.load(std::memory_order_relaxed) == nullptr;
    }
};

}
