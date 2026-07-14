// src/sys/kernel/includes/kernel/sched/internal.h
#pragma once

#include <kernel/sched/scheduler.h>
#include <kernel/sched/thread.h>

#include <ktl/deque>
#include <ktl/ref>

// Scheduler-private interfaces shared by the core/sched/ translation units (scheduler, sleep,
// spawn, stats). Everything here is guarded by interrupts-off on the single scheduling core.
// Nothing outside core/sched/ may include this header.

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

// Counters and the flight recorder live in stats.cpp. g_last_switch_ts anchors per-thread cycle
// accounting shared between switch_to and stats_snapshot.
extern global_stats g_stats;
extern uint64_t g_last_switch_ts;
void trace_push(trace_kind kind, switch_reason reason, uint64_t from, uint64_t to);

// Sleep machinery lives in sleep.cpp. wake_due_sleepers runs from the tick handler; both are
// called with interrupts disabled.
void wake_due_sleepers();
size_t sleeper_count();

}  // namespace kernel::sched
