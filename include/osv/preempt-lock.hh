/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef __PREEMPT_LOCK_H__
#define __PREEMPT_LOCK_H__

#include <osv/sched.hh>

class preempt_lock_t {
public:
    void lock() { sched::preempt_disable(); }
    void unlock() { sched::preempt_enable(); }

    static inline void prepare() {
#if CONF_lazy_stack_invariant
        assert(sched::preemptable() && arch::irq_enabled());
#endif
#if CONF_lazy_stack
        arch::ensure_next_stack_page();
#endif
    }
};

namespace {
    preempt_lock_t preempt_lock;
}

#endif // !__PREEMPT_LOCK_H__
