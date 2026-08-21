#pragma once

#include <kernel/sched/scheduler.h>
#include <kernel/sched/thread.h>
#include <kernel/synchronization/spinlock.h>
#include <kernel/time.h>

#include <ktl/deque>
#include <ktl/ref>

// Scheduler-private interfaces shared by the core/sched/ translation units (scheduler, sleep,
// spawn, reaper, stats). Everything here -- the run queues, sleeper, timed-wait and zombie lists,
// counters and the trace ring -- is guarded by g_sched_lock; per-core state is touched only by its
// own core with interrupts off. Nothing outside core/sched/ may include this header.

namespace kernel::sched {

// Taken with interrupts off and preemption disabled; never held across a context switch. Nests
// inside nothing and outside a wait queue's lock (kill scan, timed-wait expiry).
extern kernel::synchronization::spinlock g_sched_lock;
using sched_guard = kernel::synchronization::critical_irq_lock_guard<kernel::synchronization::spinlock>;

struct cpu_sched {
    ktl::ref<Thread> current;
    ktl::ref<Thread> idle;
    // Outgoing thread of the in-flight switch; dropped by sched_finish_switch() on the incoming
    // thread's stack so a dead thread's final ref never dies on its own stack.
    ktl::ref<Thread> previous;
    // Anchors per-thread cycle accounting between switch_to and stats_snapshot.
    uint64_t last_switch_ts = 0;
    uint64_t switches       = 0;
};
// The calling core's state; interrupts must be disabled so the caller cannot migrate.
cpu_sched& cur_cpu();
cpu_sched& cpu_at(size_t index);

// Grow a container that lives under g_sched_lock to `capacity` without allocating while holding
// it: the lock is taken from interrupt context, so an allocation inside it could deadlock against
// a core holding the heap lock with interrupts on. A fresh container is reserved outside, the
// contents move over under the lock (within the reservation, so no allocation), and the old
// storage dies after the unlock.
template <typename C> bool grow_under_sched_lock(C& live, size_t capacity) {
    C fresh;
    if (!fresh.reserve(capacity)) { return false; }
    C old;
    {
        sched_guard guard(g_sched_lock);
        if (live.capacity() >= capacity) { return true; }
        if constexpr (requires { live.pop_front(); }) {
            for (auto item = live.pop_front(); item.has_value(); item = live.pop_front()) {
                (void)fresh.push_back(ktl::move(*item));
            }
        } else {
            for (size_t i = 0; i < live.size(); i++) { (void)fresh.push_back(ktl::move(live[i])); }
        }
        old  = ktl::move(live);
        live = ktl::move(fresh);
    }
    return true;
}

// Queue a READY thread; g_sched_lock held. Capacity was reserved by ensure_tick_capacity.
bool push_runnable_locked(ktl::ref<Thread> thread);
size_t run_queue_depth_locked();
// make_ready for callers already holding g_sched_lock.
void make_ready_locked(ktl::ref<Thread> thread);

// The tick path (wake sleepers, requeue the preempted thread) pushes to the run queue and scans
// the sleeper list from interrupt context, where the heap forbids allocation. Capacity for the
// worst case -- every live thread runnable or sleeping at once -- is therefore reserved here, in
// thread context, before a new thread first enters scheduling. spawn calls this once per thread;
// the reaper gives the slot back. Failure is a clean spawn error, never a tick-time allocation.
ktl::result<void> ensure_tick_capacity();
void note_thread_reaped();

// Reserve the sleeper list (owned by sleep.cpp) and the zombie list (owned by reaper.cpp) to
// `capacity` entries; both take interrupts-off internally.
bool sleepers_reserve(size_t capacity);
bool zombies_reserve(size_t capacity);

// Counters and the flight recorder live in stats.cpp; both are written under g_sched_lock.
extern global_stats g_stats;
void trace_push(trace_kind kind, switch_reason reason, uint64_t from, uint64_t to);

// Sleep machinery lives in sleep.cpp. wake_due_sleepers runs from the tick handler; all three are
// called with g_sched_lock held.
void wake_due_sleepers();
size_t sleeper_count();
// Pull `thread` out of the sleeper list before its deadline and make it ready; false if it is not
// sleeping. Task kill's path for waking a killed sleeper early.
bool wake_sleeper(Thread* thread);

// Timed waits also live in sleep.cpp: waiters parked on an object's wait queue with a deadline.
// The registry entry points at the wait node on the waiter's stack; register before parking,
// unregister after waking (tolerant of the expiry scan having removed the entry first). The
// expiry scan runs from the tick handler alongside wake_due_sleepers, under g_sched_lock.
struct wait_node;
class wait_queue;
void timed_wait_register(wait_queue* queue, wait_node* node, ktime_t wake_at);
void timed_wait_unregister(wait_node* node);
void wake_due_timed_waits();

}  // namespace kernel::sched
