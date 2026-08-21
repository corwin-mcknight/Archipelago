#pragma once

#include <kernel/mm/page.h>
#include <stddef.h>
#include <stdint.h>

#include <ktl/maybe>
#include <ktl/ref>

namespace kernel::sched {

class Thread;

// Thread reaping. A dead thread cannot free itself -- it is still running on its own stack -- so
// exit_current() queues it and the reaper thread does the teardown later. The reaper also owns the
// recycled kernel-stack pool, since its whole purpose is handing reaped threads' stacks back to
// new spawns (alloc_contiguous has no contiguous free of its own).

// Spawn the reaper thread. Called by sched::init() once the scheduler is online.
void reaper_start();

// Queue a dead thread for reaping; the caller holds the scheduler lock (exit_current is
// mid-teardown and never returns). The ref is taken by value so the dying thread stays pinned
// through its final context switch. reaper_kick() wakes the reaper and takes no locks on entry.
void reaper_enqueue(ktl::ref<Thread> zombie);
void reaper_kick();

// Live counts for stats_snapshot(); read with the scheduler lock held.
size_t reaper_zombie_count();
uint64_t reaper_reaped_count();

// Recycled kernel-stack pool. acquire() returns a cached stack or nothing (the caller then
// allocates a fresh one); release() returns a stack to the pool. Both lock internally.
ktl::maybe<kernel::mm::vm_paddr_t> stack_pool_acquire();
void stack_pool_release(kernel::mm::vm_paddr_t phys);

}  // namespace kernel::sched
