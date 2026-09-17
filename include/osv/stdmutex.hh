#pragma once

#include <atomic>
#include <cassert>
#include <osv/inline-futex.hh>

struct std_mutex_data {
    typedef char State;
    std::atomic<State> state{0}; // 1 byte

    inline_futex _std_mutex_queue; // 16 byte with pointers
    inline_futex::ref get_queue() { return inline_futex::ref(_std_mutex_queue, (uintptr_t)this); }
};

struct std_mutex_data_tiny {
    typedef char State;
    std::atomic<State> state{0}; // 1 byte

    inline_futex::ref get_queue() { return inline_futex::get(this); }
};


template<class data_fields>
class std_mutex_alg : public data_fields, public lock_interface {
public:
    using State = data_fields::State;

    static constexpr State STATE_MASK      = 3;
    static constexpr State STATE_LOCK_FAST = 1;
    static constexpr State STATE_LOCK_SLOW = 2;
    static constexpr State STATE_LOCK_POLL = 4;
    static constexpr State STATE_HANDOFF = 0x80;
    using data_fields::state;

    // For Mutex
    using data_fields::get_queue;
    static constexpr uint32_t MASK_WRITER = 1 << 0;

    constexpr std_mutex_alg() noexcept = default;
    ~std_mutex_alg() = default;

    std_mutex_alg(const std_mutex_alg&) = delete;
    std_mutex_alg& operator=(const std_mutex_alg&) = delete;

    bool wlock(Node &) noexcept {
        // Fast Path: 0 -> 1
        State cur_state = this->state.load(std::memory_order_relaxed);
        if ((cur_state & STATE_MASK) == 0 &&
            state.compare_exchange_strong(cur_state, STATE_LOCK_FAST | STATE_LOCK_POLL,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
            return (cur_state & STATE_HANDOFF);
        }


        // Slow Path: Tauschen gegen 2 und bei Bedarf schlafen
        while (true) {
            cur_state = state.exchange(STATE_LOCK_SLOW,
                                       std::memory_order_acquire);
            if ((cur_state & STATE_MASK) == 0) {
                return (cur_state & STATE_HANDOFF);
            }

            get_queue().wait(state, STATE_LOCK_SLOW, MASK_WRITER);
        }
    }


    void wunlock(Node &) noexcept {
        auto old_state = (state.exchange(0, std::memory_order_release));
        if ((old_state & STATE_MASK) == STATE_LOCK_SLOW) {
            get_queue().wake(MASK_WRITER, 1);
        }
    }

    void handoff(Node &) noexcept {
        auto old_state = (state.exchange(STATE_HANDOFF, std::memory_order_release));
        if ((old_state & STATE_MASK) == STATE_LOCK_SLOW) {
            get_queue().wake(MASK_WRITER, 1);
        }
    }

    bool acquire(Node &n) noexcept { return wlock(n); }
    void release(Node &n) noexcept { wunlock(n); }



    void lock() {
        Node dummy;
        wlock(dummy);
    }

    void unlock() {
        Node dummy;
        wunlock(dummy);
    }

    bool is_locked() {
        return state.load(std::memory_order_acquire) != 0;
    }
};


class StdMutex : public std_mutex_alg<std_mutex_data> {};

class StdMutexTiny : public std_mutex_alg<std_mutex_data_tiny> {};


