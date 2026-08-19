#pragma once

#include <kernel/sched/scheduler.h>
#include <kernel/sched/thread.h>
#include <kernel/time.h>

#include <ktl/deque>
#include <ktl/ref>

// Scheduler-private interfaces shared by the core/sched/ translation units (scheduler, sleep,
// spawn, reaper, stats). Everything here is guarded by interrupts-off on the single scheduling
// core. Nothing outside core/sched/ may include this header.

namespace kernel::sched {

struct cpu_sched {
    ktl::deque<ktl::ref<Thread>> run_queue;
    ktl::ref<Thread> current;
    ktl::ref<Thread> idle;
    // Outgoing thread of the in-flight switch; dropped by sched_finish_switch() on the incoming
    // thread's stack so a dead thread's final ref never dies on its own stack.
    ktl::ref<Thread> previous;
};
cpu_sched& cur_cpu();

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

// Counters and the flight recorder live in stats.cpp. g_last_switch_ts anchors per-thread cycle
// accounting shared between switch_to and stats_snapshot.
extern global_stats g_stats;
extern uint64_t g_last_switch_ts;
void trace_push(trace_kind kind, switch_reason reason, uint64_t from, uint64_t to);

// Sleep machinery lives in sleep.cpp. wake_due_sleepers runs from the tick handler; both are
// called with interrupts disabled.
void wake_due_sleepers();
size_t sleeper_count();
// Pull `thread` out of the sleeper list before its deadline and make it ready; false if it is not
// sleeping. Task kill's path for waking a killed sleeper early; interrupts must be disabled.
bool wake_sleeper(Thread* thread);

// Timed waits also live in sleep.cpp: waiters parked on an object's wait queue with a deadline.
// The registry entry points at the wait node on the waiter's stack; register before parking,
// unregister after waking (tolerant of the expiry scan having removed the entry first). The
// expiry scan runs from the tick handler alongside wake_due_sleepers.
struct wait_node;
class wait_queue;
void timed_wait_register(wait_queue* queue, wait_node* node, ktime_t wake_at);
void timed_wait_unregister(wait_node* node);
void wake_due_timed_waits();

}  // namespace kernel::sched
