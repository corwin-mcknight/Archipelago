#pragma once

#include <kernel/sched/thread.h>
#include <kernel/sched/trace.h>

#include <ktl/ref>
#include <ktl/result>

namespace kernel::sched {

class Task;

// Bring the scheduler online on the boot core: the calling context becomes the idle thread and
// the reaper is spawned. Requires obj_init() and a ticking timer.
void init(uint32_t boot_core_index);
bool started();
// Bring the calling secondary core into scheduling once init() has run: its context becomes that
// core's idle thread. The caller then starts its tick, enables interrupts, and enters idle_loop().
void join_secondary(uint32_t core_index);
bool on_boot_core();

ktl::ref<Thread> current();
// True if the current thread is the idle thread. The idle thread must never block: it is the
// scheduler's fallback when the run queue is empty, so parking it would wedge the core.
bool current_is_idle();

// Create a kernel thread under task zero and make it runnable. name must be a string literal.
ktl::result<ktl::ref<Thread>> spawn(const char* name, thread_entry_fn entry, void* arg);
// The create half of spawn, for callers that must act between a thread existing and it
// becoming runnable -- task creation queues the bootstrap message (which carries a handle to the
// thread) in that window, so the payload can never observe an unprovisioned table. A created
// thread must go to exactly one of thread_enqueue or thread_discard. Threads in a task with an
// address space get an IPC buffer; kernel-task threads have none.
ktl::result<ktl::ref<Thread>> thread_create_in(ktl::ref<Task> task, const char* name, thread_entry_fn entry, void* arg);
ktl::result<void> thread_enqueue(ktl::ref<Thread> thread);
void thread_discard(ktl::ref<Thread> thread);

void yield();
// Block the current thread until at least `ticks` kernel ticks have elapsed. The idle thread
// must never sleep.
void sleep_ticks(uint64_t ticks);
// Timer-tick hook: slice accounting and preemption. Interrupt context only.
void on_tick();
// Consume a timer preemption request after the outermost protected/trap context exits.
void service_pending_preemption();
[[noreturn]] void exit_current();
[[noreturn]] void idle_loop();

// wait_queue/scheduler internals. schedule_out() requires interrupts disabled and the current
// thread already parked (BLOCKED/DEAD, or re-queued by the caller); reason tags the trace.
void schedule_out(switch_reason reason);
void make_ready(ktl::ref<Thread> thread);

struct core_stats {
    bool online          = false;
    uint64_t switches    = 0;
    uint64_t idle_cycles = 0;
    uint64_t current_id  = 0;  // thread running there at the snapshot
};

// Global scheduler counters plus live queue depths, snapshotted under the scheduler lock.
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
    core_stats cores[CONFIG_MAX_CORES];
};
global_stats stats_snapshot();

// Flight-recorder access. Copies are taken with interrupts disabled; newest first.
size_t trace_copy_newest(trace_record* out, size_t max);
void trace_clear();

// Lifecycle log stream through g_log.debug. Creation/destruction events (spawn/exit/reap,
// task create/teardown) are on by default; per-scheduling events (sleep/block/woke) flood
// the log and additionally need verbose. Emit sites are interrupt-enabled contexts only --
// never the switch path or tick handler.
void set_lifecycle_log(bool enabled);
bool lifecycle_log_enabled();
void set_lifecycle_log_verbose(bool enabled);
bool lifecycle_log_verbose_enabled();

}  // namespace kernel::sched

// Called by the arch entry trampolines.
extern "C" void sched_finish_switch();
extern "C" [[noreturn]] void sched_thread_exit();
