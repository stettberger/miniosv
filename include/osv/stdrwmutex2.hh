#pragma once

#include <osv/stdmutex.hh>
#include <lockfree/stack.hh>

struct std_rw_mutex2_data {
    using State = char;
    std::atomic<State> state{0};

    typedef uint16_t writer_phase_t;
    unsigned char                 writer_phase_overflow {0};
    std::atomic<writer_phase_t>   writer_phase  {0};

    using ReaderState = uint32_t;
    std::atomic<ReaderState> readers_state{0};

    inline_futex _std_mutex_queue;
    inline_futex::ref get_queue2() { return inline_futex::ref(_std_mutex_queue, (uintptr_t)this); }
    // inline_futex::ref get_queue2() { return get_queue(); }

    inline_futex::ref get_queue() { return inline_futex::get(this); }

    // inline_futex::ref get_queue2() { return inline_futex::ref(_std_rw_mutex_queue, (uintptr_t)this); }
};

struct std_rw_mutex2_data_tiny {
    using State = char;
    std::atomic<State> state{0};

    typedef uint16_t writer_phase_t;
    unsigned char                 writer_phase_overflow {0};
    std::atomic<writer_phase_t>   writer_phase  {0};

    using ReaderState = uint32_t;
    std::atomic<ReaderState> readers_state{0};

    inline_futex::ref get_queue() { return inline_futex::get(this); }
    inline_futex::ref get_queue2() { return inline_futex::get(this); }
};



template <class data_fields>
class std_rw_mutex2_alg : public std_mutex_alg<data_fields> {
    using std_mutex   = std_mutex_alg<data_fields>;
    using ReaderState = typename data_fields::ReaderState;

    using data_fields::get_queue;
    using data_fields::get_queue2;

    static constexpr uint32_t MASK_PARKED_WRITER = 1 << 1;
    static constexpr uint32_t MASK_READER        = 1 << 2;

    using data_fields::state;
    using data_fields::readers_state;
    using data_fields::writer_phase;
    using data_fields::writer_phase_overflow;

    static constexpr uint32_t WRITER_BIAS    = 1u << 30;
    static constexpr uint32_t WRITER_WAITING = 1u << 31;
    static constexpr uint32_t READER_MASK    = WRITER_BIAS - 1;

    static constexpr uint32_t PHASE_WRITING  = 1u;

public:
    struct Node : public std_mutex::Node {};

    constexpr std_rw_mutex2_alg() noexcept = default;
    ~std_rw_mutex2_alg() = default;

    std_rw_mutex2_alg(const std_rw_mutex2_alg&) = delete;
    std_rw_mutex2_alg& operator=(const std_rw_mutex2_alg&) = delete;

    void rlock(Node &) {
        while (true) {
            ReaderState prev = readers_state.fetch_add(1, std::memory_order_acquire);

            if (__builtin_expect((prev & WRITER_BIAS) == 0, 1)) {
                return;
            }

            prev = readers_state.fetch_sub(1, std::memory_order_release);

            if ((prev & WRITER_WAITING) && ((prev & READER_MASK) == 1)) {
                get_queue2().wake(MASK_PARKED_WRITER);
            }

            auto phase = writer_phase.load(std::memory_order_acquire);
            while ((phase & PHASE_WRITING) != 0) {
                get_queue2().wait(writer_phase, phase, MASK_READER);
                phase = writer_phase.load(std::memory_order_acquire);
            }
        }
    }

    void runlock(Node &) {
        ReaderState prev = readers_state.fetch_sub(1, std::memory_order_release);

        if (__builtin_expect((prev & WRITER_WAITING) != 0, 0)) {
            if ((prev & READER_MASK) == 1) {
                get_queue2().wake(MASK_PARKED_WRITER);
            }
        }
    }

    void wlock(Node &node) {
        bool handoff = std_mutex::acquire(node);
        // state.fetch_and(~std_mutex::STATE_LOCK_POLL, std::memory_order_acquire);
        if (handoff) {
            return;
        }

        // Assert phase flag before blocking new readers.
        // memory_order_release ensures visibility of this flag to readers 
        // who subsequently observe WRITER_BIAS.
        writer_phase.fetch_or(PHASE_WRITING, std::memory_order_release);

        ReaderState expected = readers_state.load(std::memory_order_relaxed);
        while (true) {
            ReaderState desired = expected | WRITER_BIAS;
            if ((expected & READER_MASK) != 0) {
                desired |= WRITER_WAITING;
            }
            if (readers_state.compare_exchange_weak(expected, desired,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                break;
            }
        }

        expected = readers_state.load(std::memory_order_acquire);
        while ((expected & READER_MASK) != 0) {
            if ((expected & WRITER_WAITING) == 0) {
                if (readers_state.compare_exchange_weak(expected, expected | WRITER_WAITING,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    expected |= WRITER_WAITING;
                }
                continue;
            }
            get_queue2().wait(readers_state, expected, MASK_PARKED_WRITER);
            expected = readers_state.load(std::memory_order_acquire);
        }
    }

    void wunlock(Node &node) {
        // if (state.load(std::memory_order_relaxed) & std_mutex::STATE_LOCK_POLL) {
        //     std_mutex::handoff(node);
        //     return;
        // }
        // if (!get_queue().empty()) {
        //     std_mutex::handoff(node);
        //     return;
        // }

        readers_state.fetch_and(~(WRITER_BIAS | WRITER_WAITING), std::memory_order_release);

        // Clear PHASE_WRITING bit (bit 0) and increment generation counter simultaneously.
        // Because writers are mutually excluded, bit 0 is guaranteed to be 1 here.
        auto prev_phase = writer_phase.fetch_add(1, std::memory_order_release);
        if (prev_phase == ~0)
            writer_phase_overflow ++;

        get_queue2().wake_all(MASK_READER);
        std_mutex::release(node);
    }
    
    bool try_rlock(Node &) {
        ReaderState expected = readers_state.load(std::memory_order_relaxed);
        while ((expected & WRITER_BIAS) == 0) {
            if (readers_state.compare_exchange_weak(expected, expected + 1,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    bool try_wlock(Node &node) {
        if (!std_mutex::try_acquire(node)) {
            return false;
        }

        ReaderState expected = 0;
        if (readers_state.compare_exchange_strong(expected, WRITER_BIAS,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            writer_phase.fetch_or(PHASE_WRITING, std::memory_order_release);
            return true;
        }

        std_mutex::release(node);
        return false;
    }

    uint32_t version() {
        return (writer_phase_overflow  << (sizeof(writer_phase)*8))
            | writer_phase.load(std::memory_order_relaxed);
    }
};

class StdRwMutex2 : public std_rw_mutex2_alg<std_rw_mutex2_data> {};
class StdRwMutex2Tiny : public std_rw_mutex2_alg<std_rw_mutex2_data_tiny> {};
