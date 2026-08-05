#pragma once

#include <kernel/panic.h>
#include <kernel/synchronization/spinlock.h>

#include <ktl/ref>

namespace kernel::sched {

class Thread;

// A parked waiter, living on the parked thread's own stack inside block_if: the frame stays alive
// exactly as long as the thread is parked, so the queue owns no storage and parking can never
// allocate or fail -- the same self-sufficiency rule the PMM's frame stacks follow. mask carries
// the signal bits an object waiter wants; plain wake_one()/wake_all() waiters use mask 0. The ref
// pins the thread while it is parked; the waker moves it out when it unlinks the node.
struct wait_node {
    ktl::ref<Thread> thread;
    uint32_t mask   = 0;
    wait_node* next = nullptr;
    wait_node* prev = nullptr;
};

// The kernel's blocking primitive: a lock plus parked threads. Blocking/waking methods are
// defined in core/sched/wait_queue.cpp and link only in kernel builds; the type itself is
// host-safe so Object can embed it.
class wait_queue {
   public:
    wait_queue()                             = default;
    wait_queue(const wait_queue&)            = delete;
    wait_queue& operator=(const wait_queue&) = delete;

    // A wait_queue outliving its parked waiters would leave dangling nodes on parked threads'
    // stacks, so tearing one down non-empty must fail loudly. This uses panic(), not the
    // assert()/kernel_assert() macro: kernel_assert reaches for g_log/kernel::crash::dispatch,
    // which the host test runner does not link (only core/object.cpp's Object subsystem is host-
    // built, not the logging/crash machinery). panic() is already the host-safe escape hatch
    // ktl reaches for (see ktl::maybe, ktl::result, ktl::ref) -- the host runner longjmps out of
    // the test instead of halting. On the host nothing ever calls block_if/wake_*, so the list is
    // always empty there and this never fires; on real kernel builds it does its job.
    ~wait_queue() {
        if (m_waiters != nullptr) { panic("wait_queue: destroyed with waiters still parked"); }
    }

    // Park the current thread. should_block is evaluated under the queue lock, closing the
    // check-then-block race against wakers that publish state before waking: pass nullptr to
    // block unconditionally. Callers must enter with interrupts enabled -- the entry/woke
    // lifecycle log lines assume it.
    void block_if(uint32_t mask, bool (*should_block)(void*), void* ctx);
    void block(uint32_t mask = 0) { block_if(mask, nullptr, nullptr); }

    void wake_one();
    void wake_all();
    // Wake every waiter whose nonzero mask intersects signals. Returns the number woken.
    size_t wake_matching(uint32_t signals);
    bool has_waiters();

   private:
    void unlink(wait_node* node);

    // m_lock protects the waiter list only. The parking protocol itself -- unlock, then switch
    // with interrupts off -- is safe on a single scheduling core because nothing can run and wake
    // the thread in that window. The decided SMP handshake (todo.md, scheduler section) is a
    // per-thread on-cpu flag cleared from sched_finish_switch: a waker on another core spins
    // until the flag clears before running a thread it found parked here.
    kernel::synchronization::spinlock m_lock;
    wait_node* m_waiters = nullptr;  // intrusive doubly-linked list of on-stack nodes
};

}  // namespace kernel::sched
