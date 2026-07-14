// src/sys/kernel/core/sched/reaper.cpp
#include <kernel/arch.h>
#include <kernel/assert.h>
#include <kernel/log.h>
#include <kernel/sched/reaper.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>
#include <kernel/sched/user_task.h>
#include <kernel/sched/wait_queue.h>

#include <ktl/deque>
#include <ktl/ref>
#include <ktl/stack>

namespace kernel::sched {

namespace {

// Dead threads awaiting the reaper, and recycled stacks. Stacks come from alloc_contiguous, which
// has no contiguous free -- the pool recycles them among threads instead; it is bounded by the peak
// live thread count.
ktl::deque<ktl::ref<Thread>> g_zombies;
ktl::stack<kernel::mm::vm_paddr_t> g_stack_cache;
wait_queue g_reaper_wq;
uint64_t g_reaped = 0;

void reap(ktl::ref<Thread> zombie) {
    if (lifecycle_log_enabled()) { g_log.debug("sched: reap id={0}", zombie->id()); }
    auto task = ktl::static_ref_cast<Task>(zombie->owner());
    // Both spawn paths construct a Thread with an owner, so this only catches ownerless threads
    // from the bare constructor (tests build those directly): fall back to kernel_task, where
    // remove_thread is a harmless no-op.
    if (!task) { task = kernel_task(); }
    task->remove_thread(zombie->id());
    if (zombie->kstack_phys() != 0) { stack_pool_release(zombie->kstack_phys()); }
    if (task.get() != kernel_task().get() && task->thread_count() == 0) { teardown_user_task(ktl::move(task)); }
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    g_reaped += 1;
    kernel::arch::restore_interrupts(flags);
}

[[noreturn]] void reaper_main(void*) {
    while (true) {
        ktl::maybe<ktl::ref<Thread>> zombie;
        uint64_t flags = kernel::arch::save_and_disable_interrupts();
        zombie         = g_zombies.pop_front();
        kernel::arch::restore_interrupts(flags);
        if (!zombie.has_value()) {
            g_reaper_wq.block_if(0, [](void*) { return g_zombies.size() == 0; }, nullptr);
            continue;
        }
        reap(ktl::move(*zombie));
    }
}

}  // namespace

void reaper_start() { spawn("reaper", reaper_main, nullptr).expect("sched: reaper spawn failed"); }

void reaper_enqueue(ktl::ref<Thread> zombie) {
    // Interrupts are already disabled by the caller (exit_current, mid-teardown).
    bool ok = g_zombies.push_back(ktl::move(zombie));
    assert(ok, "reaper_enqueue: zombie list allocation failed");
    g_reaper_wq.wake_one();
}

size_t reaper_zombie_count() {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    size_t n       = g_zombies.size();
    kernel::arch::restore_interrupts(flags);
    return n;
}

uint64_t reaper_reaped_count() {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    uint64_t n     = g_reaped;
    kernel::arch::restore_interrupts(flags);
    return n;
}

ktl::maybe<kernel::mm::vm_paddr_t> stack_pool_acquire() {
    uint64_t flags                          = kernel::arch::save_and_disable_interrupts();
    ktl::maybe<kernel::mm::vm_paddr_t> phys = g_stack_cache.pop();
    kernel::arch::restore_interrupts(flags);
    return phys;
}

void stack_pool_release(kernel::mm::vm_paddr_t phys) {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    bool ok        = g_stack_cache.push(phys);
    kernel::arch::restore_interrupts(flags);
    if (!ok) { g_log.warn("sched: stack cache full; leaking a thread stack"); }
}

}  // namespace kernel::sched
