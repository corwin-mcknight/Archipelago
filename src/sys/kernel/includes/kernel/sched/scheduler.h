// src/sys/kernel/includes/kernel/sched/scheduler.h
#pragma once

#include <kernel/sched/thread.h>
#include <kernel/sched/trace.h>

#include <ktl/ref>
#include <ktl/result>

namespace kernel::sched {

// Bring the scheduler online on the boot core: the calling context becomes the idle thread and
// the reaper is spawned. Requires obj_init() and a ticking timer.
void init(uint32_t boot_core_index);
bool started();

ktl::ref<Thread> current();
// True if the current thread is the idle thread. The idle thread must never block: it is the
// scheduler's fallback when the run queue is empty, so parking it would wedge the core.
bool current_is_idle();

// Create a kernel thread under task zero and make it runnable. name must be a string literal.
ktl::result<ktl::ref<Thread>> spawn(const char* name, thread_entry_fn entry, void* arg);

void yield();
// Block the current thread until at least `ticks` kernel ticks have elapsed. The idle thread
// must never sleep.
void sleep_ticks(uint64_t ticks);
// Timer-tick hook: slice accounting and preemption. Interrupt context only.
void on_tick();
[[noreturn]] void exit_current();
[[noreturn]] void idle_loop();

// wait_queue/scheduler internals. schedule_out() requires interrupts disabled and the current
// thread already parked (BLOCKED/DEAD, or re-queued by the caller); reason tags the trace.
void schedule_out(switch_reason reason);
void make_ready(ktl::ref<Thread> thread);

// Global scheduler counters plus live queue depths, snapshotted with interrupts disabled.
struct global_stats {
    uint64_t switches       = 0;
    uint64_t preempts       = 0;
    uint64_t yields         = 0;
    uint64_t block_switches = 0;
    uint64_t sleep_switches = 0;
    uint64_t exit_switches  = 0;
    uint64_t wakes          = 0;
    uint64_t spawned        = 0;
    uint64_t reaped         = 0;
    uint64_t boot_ts        = 0;  // timestamp at sched::init
    uint64_t idle_cycles    = 0;  // convenience copy of the idle thread's cpu_cycles
    size_t runq_depth       = 0;
    size_t sleepers         = 0;
    size_t zombies          = 0;
};
global_stats stats_snapshot();

// Flight-recorder access. Copies are taken with interrupts disabled; newest first.
size_t trace_copy_newest(trace_record* out, size_t max);
void trace_clear();

// Lifecycle log stream (spawn/block/sleep/wake/exit/reap through g_log.debug). Default off.
// Emit sites are interrupt-enabled contexts only -- never the switch path or tick handler.
void set_lifecycle_log(bool enabled);
bool lifecycle_log_enabled();

}  // namespace kernel::sched

// Called by the arch entry trampolines.
extern "C" void sched_finish_switch();
extern "C" [[noreturn]] void sched_thread_exit();
