// src/sys/kernel/core/sched/wait_queue.cpp
#include <kernel/arch.h>
#include <kernel/assert.h>
#include <kernel/log.h>
#include <kernel/obj/object.h>
#include <kernel/obj/semaphore.h>
#include <kernel/sched/internal.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/thread.h>
#include <kernel/sched/wait_queue.h>

#include <ktl/static_vector>

namespace kernel::sched {

void wait_queue::block_if(uint32_t mask, bool (*should_block)(void*), void* ctx) {
    // The node lives in this frame, which survives exactly as long as the thread stays parked, so
    // parking allocates nothing and cannot fail -- there is no OOM path out of a blocking wait.
    // The waker unlinks the node and moves the ref out before make_ready, after which the node is
    // dead memory nobody references; this frame only unwinds once the thread runs again.
    wait_node node;
    block_if(node, mask, should_block, ctx);
}

void wait_queue::block_if(wait_node& node, uint32_t mask, bool (*should_block)(void*), void* ctx) {
    assert(!current_is_idle(), "block_if: idle thread cannot block");
    if (lifecycle_log_verbose_enabled()) { g_log.debug("sched: block id={0}", current()->id()); }
    node.mask      = mask;
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    kernel::synchronization::preempt_disable();
    m_lock.lock();
    // A killed thread must not park on a signal wait: its wake may never come, and the kill scan
    // that would have claimed it has already run. Mask-0 waits (mutexes) still park -- their wake
    // is the holder's unlock, which always comes, and refusing them would spin instead. The check
    // lives under the queue lock so a thread that slipped past the kill scan unparked cannot then
    // park unnoticed.
    ktl::ref<Thread> self = current();
    if ((mask != 0 && self->killed()) || (should_block != nullptr && !should_block(ctx))) {
        m_lock.unlock();
        kernel::synchronization::preempt_enable();
        kernel::arch::restore_interrupts(flags);
        return;
    }
    self->stats().blocks += 1;
    self->set_state(thread_state::BLOCKED);
    Thread* parked = self.get();
    parked->set_parked(this, &node);
    node.thread = ktl::move(self);
    node.next   = m_waiters;
    node.prev   = nullptr;
    if (m_waiters != nullptr) { m_waiters->prev = &node; }
    m_waiters = &node;
    m_lock.unlock();
    kernel::synchronization::preempt_enable();
    // Interrupts stay off between unlock and the switch: on the single scheduling core nothing
    // can run and wake us in that window.
    schedule_out(switch_reason::BLOCK);
    parked->set_parked(nullptr, nullptr);
    kernel::arch::restore_interrupts(flags);
    if (lifecycle_log_verbose_enabled()) { g_log.debug("sched: woke id={0}", current()->id()); }
}

// Unlink under the queue lock; the caller takes the thread ref before the node becomes dead
// memory on the (about-to-wake) thread's stack.
void wait_queue::unlink(wait_node* node) {
    if (node->prev != nullptr) { node->prev->next = node->next; }
    if (node->next != nullptr) { node->next->prev = node->prev; }
    if (m_waiters == node) { m_waiters = node->next; }
    node->next = nullptr;
    node->prev = nullptr;
}

namespace {
// Wakes drain through a fixed stack batch instead of a heap-allocated list: waking is what
// interrupt-side paths call (the timer today, interrupt objects later), and the heap forbids
// interrupt-context allocation. A batch that fills simply triggers another collection pass;
// waiters that re-block between passes are re-checked by block_if's predicate, so an extra wake
// is spurious, never wrong.
constexpr size_t WAKE_BATCH = 8;
}  // namespace

ktl::ref<Thread> wait_queue::claim(wait_node* node) {
    kernel::synchronization::critical_irq_lock_guard guard(m_lock);
    // An empty thread ref means the waiter either has not parked yet or was already taken by a
    // signal waker; either way this claim loses the race and touches nothing.
    if (!node->thread) { return ktl::ref<Thread>(); }
    ktl::ref<Thread> woken = ktl::move(node->thread);
    unlink(node);
    return woken;
}

void wait_queue::wake_one() {
    ktl::ref<Thread> woken;
    {
        kernel::synchronization::critical_irq_lock_guard guard(m_lock);
        for (wait_node* node = m_waiters; node != nullptr; node = node->next) {
            if (node->mask != 0) { continue; }  // signal waiters are woken by wake_matching
            woken = ktl::move(node->thread);
            unlink(node);
            break;
        }
    }
    if (woken) { make_ready(ktl::move(woken)); }
}

void wait_queue::wake_all() {
    while (true) {
        ktl::static_vector<ktl::ref<Thread>, WAKE_BATCH> batch;
        bool scanned_all = false;
        {
            kernel::synchronization::critical_irq_lock_guard guard(m_lock);
            wait_node* node = m_waiters;
            while (node != nullptr && batch.size() < batch.capacity()) {
                wait_node* next = node->next;
                if (node->mask == 0) {
                    (void)batch.push_back(ktl::move(node->thread));
                    unlink(node);
                }
                node = next;
            }
            scanned_all = node == nullptr;
        }
        for (auto woken = batch.pop_back(); woken.has_value(); woken = batch.pop_back()) {
            make_ready(ktl::move(*woken));
        }
        if (scanned_all) { return; }
    }
}

size_t wait_queue::wake_matching(uint32_t signals) {
    size_t woken_count = 0;
    while (true) {
        ktl::static_vector<ktl::ref<Thread>, WAKE_BATCH> batch;
        bool scanned_all = false;
        {
            kernel::synchronization::critical_irq_lock_guard guard(m_lock);
            wait_node* node = m_waiters;
            while (node != nullptr && batch.size() < batch.capacity()) {
                wait_node* next = node->next;
                if (node->mask != 0 && (node->mask & signals) != 0) {
                    (void)batch.push_back(ktl::move(node->thread));
                    unlink(node);
                }
                node = next;
            }
            scanned_all = node == nullptr;
        }
        woken_count += batch.size();
        for (auto woken = batch.pop_back(); woken.has_value(); woken = batch.pop_back()) {
            make_ready(ktl::move(*woken));
        }
        if (scanned_all) { return woken_count; }
    }
}

bool wait_queue::has_waiters() {
    kernel::synchronization::critical_irq_lock_guard guard(m_lock);
    return m_waiters != nullptr;
}

}  // namespace kernel::sched

namespace kernel::obj {

void object_signal_wake(Object* obj) { obj->waiters().wake_matching(obj->signals()); }

uint32_t Object::wait_signals(uint32_t mask) {
    assert(mask != 0, "wait_signals: empty mask would never wake");
    struct Ctx {
        Object* obj;
        uint32_t mask;
    };
    Ctx ctx{this, mask};
    uint32_t cur = signals();
    // A killed thread falls out with whatever signals are current -- block_if refuses to park it,
    // so looping on would busy-spin. The caller's syscall return is moot; the thread exits at the
    // syscall boundary.
    while ((cur & mask) == 0 && !kernel::sched::current()->killed()) {
        // The predicate re-checks under the queue lock; a signal_set that ran between our check
        // and the park either flips the predicate or finds the node via wake_matching.
        m_waiters.block_if(
            mask,
            [](void* p) {
                auto* c = static_cast<Ctx*>(p);
                return (c->obj->signals() & c->mask) == 0;
            },
            &ctx);
        cur = signals();
    }
    return cur;
}

uint32_t Object::wait_signals_deadline(uint32_t mask, ktime_t deadline) {
    assert(mask != 0, "wait_signals_deadline: empty mask would never wake");
    struct Ctx {
        Object* obj;
        uint32_t mask;
        ktime_t deadline;
    };
    Ctx ctx{this, mask, deadline};
    uint32_t cur = signals();
    while ((cur & mask) == 0 && kernel::time::now() < deadline && !kernel::sched::current()->killed()) {
        // The node lives in this frame and the registry entry points at it; register before
        // parking so the expiry scan can find the node, and unregister after every wake before
        // the next iteration reuses the frame. The predicate re-checks the deadline under the
        // queue lock: if the expiry scan already ran past a not-yet-parked node (claim finds no
        // thread and leaves the entry), the predicate refuses the park instead of sleeping
        // forever on a deadline nobody watches anymore.
        kernel::sched::wait_node node;
        kernel::sched::timed_wait_register(&m_waiters, &node, deadline);
        m_waiters.block_if(
            node, mask,
            [](void* p) {
                auto* c = static_cast<Ctx*>(p);
                return (c->obj->signals() & c->mask) == 0 && kernel::time::now() < c->deadline;
            },
            &ctx);
        kernel::sched::timed_wait_unregister(&node);
        cur = signals();
    }
    return cur;
}

void Semaphore::acquire() {
    // A killed thread gives up instead of re-parking: block_if lets mask-0 waits park even when
    // killed because a mutex holder's unlock always comes, but a semaphore's release() has no
    // obligated sender, so re-parking here would leave the thread unwakeable and its task
    // unkillable. The caller gets no unit in that case; the thread is on its way to the exit
    // boundary.
    while (!try_acquire()) {
        if (kernel::sched::current()->killed()) { return; }
        // mask 0: woken by release()'s wake_one, never by signal waiters' wake_matching.
        waiters().block_if(0, [](void* p) { return static_cast<Semaphore*>(p)->count() == 0; }, this);
    }
}

void Semaphore::release() {
    m_count.fetch_add(1, ktl::memory_order::release);
    waiters().wake_one();
}

}  // namespace kernel::obj
