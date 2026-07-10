// src/sys/kernel/includes/kernel/sched/wait_queue.h
#pragma once

#include <kernel/synchronization/spinlock.h>

#include <ktl/ref>
#include <ktl/vector>

namespace kernel::sched {

class Thread;

// A parked waiter. mask carries the signal bits an object waiter wants; plain wake_one()/
// wake_all() waiters use mask 0. The ref pins the thread while it is parked.
struct wait_node {
    ktl::ref<Thread> thread;
    uint32_t mask = 0;
};

// The kernel's blocking primitive: a lock plus parked threads. Blocking/waking methods are
// defined in core/sched/wait_queue.cpp and link only in kernel builds; the type itself is
// host-safe so Object can embed it.
class wait_queue {
   public:
    wait_queue()                             = default;
    wait_queue(const wait_queue&)            = delete;
    wait_queue& operator=(const wait_queue&) = delete;

    // Park the current thread. should_block is evaluated under the queue lock, closing the
    // check-then-block race against wakers that publish state before waking: pass nullptr to
    // block unconditionally.
    void block_if(uint32_t mask, bool (*should_block)(void*), void* ctx);
    void block(uint32_t mask = 0) { block_if(mask, nullptr, nullptr); }

    void wake_one();
    void wake_all();
    // Wake every waiter whose nonzero mask intersects signals. Returns the number woken.
    size_t wake_matching(uint32_t signals);

   private:
    kernel::synchronization::spinlock m_lock;
    ktl::vector<wait_node> m_nodes;
};

}  // namespace kernel::sched
