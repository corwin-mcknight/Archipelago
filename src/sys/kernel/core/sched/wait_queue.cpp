// src/sys/kernel/core/sched/wait_queue.cpp
#include <kernel/arch.h>
#include <kernel/assert.h>
#include <kernel/obj/object.h>
#include <kernel/obj/semaphore.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/thread.h>
#include <kernel/sched/wait_queue.h>

namespace kernel::sched {

void wait_queue::block_if(uint32_t mask, bool (*should_block)(void*), void* ctx) {
    assert(!current_is_idle(), "block_if: idle thread cannot block");
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    m_lock.lock();
    if (should_block != nullptr && !should_block(ctx)) {
        m_lock.unlock();
        kernel::arch::restore_interrupts(flags);
        return;
    }
    ktl::ref<Thread> self = current();
    self->set_state(thread_state::BLOCKED);
    bool pushed = m_nodes.push_back(wait_node{ktl::move(self), mask});
    assert(pushed, "wait_queue: waiter allocation failed");
    m_lock.unlock();
    // Interrupts stay off between unlock and the switch: on the single scheduling core nothing
    // can run and wake us in that window.
    schedule_out();
    kernel::arch::restore_interrupts(flags);
}

namespace {
void drain(ktl::vector<ktl::ref<Thread>>& threads) {
    for (size_t i = 0; i < threads.size(); ++i) { make_ready(ktl::move(threads[i])); }
}
}  // namespace

void wait_queue::wake_one() {
    ktl::ref<Thread> woken;
    {
        kernel::synchronization::lock_guard guard(m_lock);
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            if (m_nodes[i].mask != 0) { continue; }  // signal waiters are woken by wake_matching
            woken        = ktl::move(m_nodes[i].thread);
            m_nodes[i]   = ktl::move(m_nodes[m_nodes.size() - 1]);
            auto discard = m_nodes.pop_back();
            break;
        }
    }
    if (woken) { make_ready(ktl::move(woken)); }
}

void wait_queue::wake_all() {
    ktl::vector<ktl::ref<Thread>> woken;
    {
        kernel::synchronization::lock_guard guard(m_lock);
        for (size_t i = 0; i < m_nodes.size();) {
            if (m_nodes[i].mask != 0) {
                ++i;
                continue;
            }
            bool ok = woken.push_back(ktl::move(m_nodes[i].thread));
            assert(ok, "wait_queue: wake list allocation failed");
            m_nodes[i]   = ktl::move(m_nodes[m_nodes.size() - 1]);
            auto discard = m_nodes.pop_back();
        }
    }
    drain(woken);
}

size_t wait_queue::wake_matching(uint32_t signals) {
    ktl::vector<ktl::ref<Thread>> woken;
    {
        kernel::synchronization::lock_guard guard(m_lock);
        for (size_t i = 0; i < m_nodes.size();) {
            if (m_nodes[i].mask == 0 || (m_nodes[i].mask & signals) == 0) {
                ++i;
                continue;
            }
            bool ok = woken.push_back(ktl::move(m_nodes[i].thread));
            assert(ok, "wait_queue: wake list allocation failed");
            m_nodes[i]   = ktl::move(m_nodes[m_nodes.size() - 1]);
            auto discard = m_nodes.pop_back();
        }
    }
    size_t n = woken.size();
    drain(woken);
    return n;
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
    while ((cur & mask) == 0) {
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

void Semaphore::acquire() {
    while (!try_acquire()) {
        // mask 0: woken by release()'s wake_one, never by signal waiters' wake_matching.
        waiters().block_if(0, [](void* p) { return static_cast<Semaphore*>(p)->count() == 0; }, this);
    }
}

void Semaphore::release() {
    __atomic_fetch_add(&m_count, 1, __ATOMIC_RELEASE);
    waiters().wake_one();
}

}  // namespace kernel::obj
