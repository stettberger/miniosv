#include <osv/inline-futex.hh>


static constexpr unsigned NUM_FUTEXES = 2024;
static inline_futex global_futexes[NUM_FUTEXES];

inline_futex::ref inline_futex::get(void *addr) {
    uintptr_t hash = (uintptr_t) addr;
    hash = hash ^ (hash >> (sizeof(uintptr_t)/2)*8);

    auto &q = global_futexes[hash % NUM_FUTEXES];

    // Create a pre-keyed reference
    return ref(q, (uintptr_t) addr);
}
