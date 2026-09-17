#pragma once

#include <atomic>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__)
#define CPU_PAUSE() asm volatile("yield" ::: "memory")
#else
#define CPU_PAUSE() std::this_thread::yield()
#endif

static inline void burn_tsc_ticks(uint64_t n) {
    _mm_lfence();
    uint64_t start = __rdtsc();
    _mm_lfence();

    while ((__rdtsc() - start) < n) {
        CPU_PAUSE();
    }}

class lock_interface {
public:
    struct Node {};

    void wlock(Node &) { assert(false); }
    void wunlock(Node &) { assert(false); }

    void rlock(Node &) { assert(false); }
    void runlock(Node &) { assert(false); }
};

struct BackoffNone {
    void attempt_new() { }
    void attempt_fail() { }
    void sleep() { }
};

template<int init=200, int factor=3, int divide=2>
struct BackoffExponential {
    int backoff = init;
    void attempt_new() { }
    void attempt_fail() {
        backoff = (backoff * factor / divide);
        backoff &= ((1 << 14)-1);
    }
    void sleep() {
        burn_tsc_ticks(backoff);
    }
};

template<int init = 200>
struct BackoffAutoTune {
    int backoff = init;
    int start;
    
    void attempt_new() {
        start = __rdtsc();
    }
    void attempt_fail() {
        int delta = (__rdtsc() - start)*2;
        delta &= ((1 << 14)-1);
        if (delta  > backoff) {
            backoff = delta;
        }
    }
    void sleep() {
        burn_tsc_ticks(backoff);
    }
};


template<class Backoff = BackoffNone >
class TATAS : public lock_interface {
    std::atomic<bool> state_{false};
public:
    void wlock(Node &) {
        Backoff backoff;
        
        while (true) {
            // Test: Read without invalidating the cache line in other cores
            while (state_.load(std::memory_order_relaxed)) {
                backoff.sleep();
            }
            backoff.attempt_new();

            // Test-and-Set: Attempt to acquire ownership exclusively
            if (!state_.exchange(true, std::memory_order_acquire)) {
                // printf("backoff: %d\n", backoff);
                return;
            }
            backoff.attempt_fail();
        }
    }

    void wunlock(Node &) {
        state_.store(false, std::memory_order_release);
    }

    void rlock(Node &n) { assert(false); }
    void runlock(Node &n) { assert(false); }
};

class TicketLock : public lock_interface {
    // Both counters are unsigned to guarantee well-defined wrap-around behavior
    std::atomic<uint32_t> next_ticket_{0};
    std::atomic<uint32_t> now_serving_{0};

public:
    void wlock(Node &) {
        // Fetch-and-add to draw a ticket (FIFO order)
        const uint32_t my_ticket = next_ticket_.fetch_add(1, std::memory_order_relaxed);

        // Spin until our ticket is called
        while (now_serving_.load(std::memory_order_acquire) != my_ticket) {
            CPU_PAUSE();
        }
    }

    void wunlock(Node &) {
        // Increment serving counter to hand off to the next waiting ticket
        const uint32_t current = now_serving_.load(std::memory_order_relaxed);
        now_serving_.store(current + 1, std::memory_order_release);
    }
};

class MCSLock : public lock_interface {
public:
    struct Node : public lock_interface::Node {
        std::atomic<Node*> next{nullptr};
        std::atomic<bool> locked{false};
    };

private:
    std::atomic<Node*> tail_{nullptr};

public:
    void wlock(Node &node) {
        node.next.store(nullptr, std::memory_order_relaxed);

        // Atomically enqueue this node at the tail of the waiting queue
        Node *prev = tail_.exchange(&node, std::memory_order_acq_rel);

        if (prev != nullptr) {
            // Link predecessor to self
            node.locked.store(true, std::memory_order_relaxed);
            prev->next.store(&node, std::memory_order_release);

            // Spin locally on our own node until predecessor passes ownership
            while (node.locked.load(std::memory_order_acquire)) {
                CPU_PAUSE();
            }
        }
    }

    void wunlock(Node &node) {
        // If we are still the tail, try to reset tail_ to nullptr
        Node *expected = &node;
        if (tail_.compare_exchange_strong(expected, nullptr,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {
            return;
        }

        // A successor is in the process of enqueueing: wait until next pointer is set
        Node *succ = nullptr;
        while ((succ = node.next.load(std::memory_order_acquire)) == nullptr) {
            CPU_PAUSE();
        }

        // Hand over the lock to the successor
        succ->locked.store(false, std::memory_order_release);
    }
};

template <typename Host, auto  Member>
class InPlaceMCSLock {
    // Extract MemberType from the member pointer type
    template <typename T>
    struct member_traits;

    template <typename M, typename C>
    struct member_traits<M C::*> {
        using member_type = M;
        using class_type  = C;
    };

    using Traits     = member_traits<decltype(Member)>;
    using MemberType = typename Traits::member_type;

    static_assert(std::is_same_v<typename Traits::class_type, Host>, 
                  "Member pointer must belong to Host.");
    static_assert(sizeof(MemberType) == 8, 
                  "Target member must be 8 bytes wide.");
    static_assert(std::is_trivially_copyable_v<Host>, 
                  "Host must be trivially copyable.");

    static constexpr uintptr_t LOCKED_BIT = 0x1;
    static constexpr uintptr_t PTR_MASK   = ~static_cast<uintptr_t>(0x7);

public:
    struct alignas(8) Node {
        std::atomic<Node*> next{nullptr};
        std::atomic<bool>  locked{false};

        Host val;
        Host reference;

        Host current;

        inline void capture(const Host &host) {
            __builtin_memcpy(&val,       &host, sizeof(Host));
            __builtin_memcpy(&reference,       &val, sizeof(Host));
        }

        inline void inherit(const Node &node) {
            __builtin_memcpy(&val,        &node.val,             sizeof(Host));
            __builtin_memcpy(&reference,  &node.reference,       sizeof(Host));
        }

        inline void write_update(Host &host, size_t offset, size_t len) {
            current = host;
            
            const uint8_t* src = reinterpret_cast<const uint8_t*>(&val);
            uint8_t* ref = reinterpret_cast<uint8_t*>(&reference);
            uint8_t* dst = reinterpret_cast<uint8_t*>(&host);
            for (size_t i = offset; i < offset+len; i++) {
                auto old_dst = dst[i]; (void)old_dst;
                if (src[i] != ref[i]) {
                    dst[i] = src[i];
                    ref[i] = src[i];
                }
            }
        }
    };

    static inline size_t member_offset() {
        return reinterpret_cast<size_t>(&(static_cast<Host*>(nullptr)->*Member));
    }


    // Acquires the lock. Returns the current 8-byte payload.
    // - If uncontended, reads payload directly from host.*Member.
    // - If contended, waits and receives payload handed over by predecessor.
    static void wlock(Host& host, Node& node) {
        assert((reinterpret_cast<uintptr_t>(&node) & 0x7) == 0 && "Node must be >= 8-byte aligned.");

        node.next.store(nullptr, std::memory_order_relaxed);
        node.locked.store(false, std::memory_order_relaxed);

        auto& tail_field = reinterpret_cast<std::atomic<uintptr_t>&>(host.*Member);
        uintptr_t node_tagged = reinterpret_cast<uintptr_t>(&node) | LOCKED_BIT;

        // Atomically publish node as tail and set the locked bit
        uintptr_t prev = tail_field.exchange(node_tagged, std::memory_order_acq_rel);

        if ((prev & LOCKED_BIT) == 0) {
            // Uncontended fast path.

            // 1. Copy original host state into local node
            Host copy = host;
            copy.*Member = reinterpret_cast<MemberType>(prev);
            node.capture(copy);

            return;
        }

        // Contended slow path: link predecessor to self and wait
        Node* pred = reinterpret_cast<Node*>(prev & PTR_MASK);
        assert(pred != nullptr);

        node.locked.store(true, std::memory_order_relaxed);
        pred->next.store(&node, std::memory_order_release);

        while (node.locked.load(std::memory_order_acquire)) {
            CPU_PAUSE();
        }
    }

    // Releases the lock and commits/forwards the updated payload.
    // - If no successor, writes new_val back into host.*Member (LOCKED bit cleared).
    // - If successor exists, hands new_val directly over to succ->val.
    static void wunlock(Host& host, Node& node) {
        Node* succ = node.next.load(std::memory_order_acquire);

        if (!succ) { // We might be the last locker.
            // 1. Extract raw field value from node.val
            uintptr_t raw_val = reinterpret_cast<uintptr_t>(node.val.*Member);
            assert((raw_val & LOCKED_BIT) == 0 && "Payload must not use LSB (bit 0).");

            // 2. Speculatively restore the rest of the host object before releasing the lock
            const size_t offset      = member_offset();
            const size_t tail_offset = offset + sizeof(MemberType);
            const size_t tail_size   = sizeof(Host) - tail_offset;
            const char* src = reinterpret_cast<const char*>(&node.val);
            char* dst = reinterpret_cast<char*>(&host);
            (void)src; (void)dst;


            if (offset > 0) {
                // node.write_update(host, 0, offset);
                __builtin_memcpy(dst, src, offset);
            }
            if (tail_size > 0) {
                // node.write_update(host, tail_offset, tail_size);
                __builtin_memcpy(dst + tail_offset, src + tail_offset, tail_size);
            }

            // 3. Attempt to unlock by replacing our node with raw_val
            auto& tail_field = reinterpret_cast<std::atomic<uintptr_t>&>(host.*Member);
            uintptr_t expected = reinterpret_cast<uintptr_t>(&node) | LOCKED_BIT;
            if (tail_field.compare_exchange_strong(expected, raw_val,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed)) {
                return;
            }

            // Successor enqueuing: wait until its next pointer is published
            while ((succ = node.next.load(std::memory_order_acquire)) == nullptr) {
                CPU_PAUSE();
            }
        }

        // Handoff Instance to successor
        succ->inherit(node);
        succ->locked.store(false, std::memory_order_release);
    }

    static void acquire(Host& host, Node& node) { wlock(host, node); }
    static void release(Host& host, Node& node) { wunlock(host, node); }

};
