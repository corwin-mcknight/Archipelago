// src/sys/kernel/task/reaper.cpp
#include <kernel/arch.h>
#include <kernel/assert.h>
#include <kernel/log.h>
#include <kernel/sched/internal.h>
#include <kernel/sched/reaper.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>
#include <kernel/sched/user_task.h>
#include <kernel/sched/wait_queue.h>

#include <ktl/deque>
#include <ktl/ref>

namespace kernel::sched {

namespace {

// Dead threads awaiting the reaper, and recycled stacks. Stacks come from alloc_contiguous, which
// has no contiguous free -- the pool recycles them among threads instead; it is bounded by the peak
// live thread count. All under g_sched_lock.
[[clang::no_destroy]] constinit ktl::deque<ktl::ref<Thread>> g_zombies;
[[clang::no_destroy]] constinit ktl::deque<kernel::mm::vm_paddr_t> g_stack_cache;
[[clang::no_destroy]] wait_queue g_reaper_wq;
uint64_t g_reaped = 0;

void reap(ktl::ref<Thread> zombie) {
    // The zombie was queued before its core switched away from it; wait for that switch to finish
    // before touching its stack or its task's address space.
    while (zombie->on_cpu()) {}
    if (lifecycle_log_enabled()) { g_log.debug("sched: reap id={0}", zombie->id()); }
    auto task = zombie->owner();
    task->remove_thread(zombie->id());
    // Return the thread's IPC buffer alongside its kernel stack: both are resources the task holds
    // on the thread's behalf, and a task that spawns and reaps workers would otherwise run out of
    // buffer slots (and address space) long before it ran out of anything else.
    release_thread_ipc(*task, zombie->ipc());
    if (zombie->kstack_phys() != 0) { stack_pool_release(zombie->kstack_phys()); }
    if (task.get() != kernel_task().get() && task->thread_count() == 0) { teardown_user_task(ktl::move(task)); }
    note_thread_reaped();
    sched_guard guard(g_sched_lock);
    g_reaped += 1;
}

[[noreturn]] void reaper_main(void*) {
    while (true) {
        ktl::maybe<ktl::ref<Thread>> zombie;
        {
            sched_guard guard(g_sched_lock);
            zombie = g_zombies.pop_front();
        }
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
    // g_sched_lock is held by the caller (exit_current). The push cannot allocate:
    // ensure_tick_capacity reserved a zombie slot per live thread at spawn, and a thread dies at
    // most once. The check survives NDEBUG because a silent failure here would be a permanently
    // leaked thread, not a caught bug.
    bool ok = g_zombies.push_back(ktl::move(zombie));
    ensure(ok, "reaper_enqueue: zombie list push failed despite reservation");
}

void reaper_kick() { g_reaper_wq.wake_one(); }

bool zombies_reserve(size_t capacity) {
    // The stack cache grows with the zombie list: it is bounded by the same peak thread count.
    return grow_under_sched_lock(g_zombies, capacity) && grow_under_sched_lock(g_stack_cache, capacity);
}

size_t reaper_zombie_count() { return g_zombies.size(); }
uint64_t reaper_reaped_count() { return g_reaped; }

ktl::maybe<kernel::mm::vm_paddr_t> stack_pool_acquire() {
    sched_guard guard(g_sched_lock);
    return g_stack_cache.pop_back();
}

void stack_pool_release(kernel::mm::vm_paddr_t phys) {
    bool ok;
    {
        sched_guard guard(g_sched_lock);
        ok = g_stack_cache.push_back(phys);
    }
    if (!ok) { g_log.warn("sched: stack cache full; leaking a thread stack"); }
}

}  // namespace kernel::sched
