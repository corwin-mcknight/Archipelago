// src/sys/kernel/core/sched/stats.cpp
#include <kernel/arch.h>
#include <kernel/config.h>
#include <kernel/sched/internal.h>
#include <kernel/sched/reaper.h>
#include <kernel/sched/scheduler.h>

namespace kernel::sched {

namespace {

trace_ring<CONFIG_SCHED_TRACE_EVENTS> g_trace;
bool g_lifecycle_log         = true;
// Per-scheduling-event messages (sleep/block/woke) flood the log; opt in via `sched log verbose`.
bool g_lifecycle_log_verbose = false;

}  // namespace

global_stats g_stats;
uint64_t g_last_switch_ts = 0;

void trace_push(trace_kind kind, switch_reason reason, uint64_t from, uint64_t to) {
    trace_record r;
    r.timestamp = kernel::arch::timestamp();
    r.kind      = kind;
    r.reason    = reason;
    r.from_id   = from;
    r.to_id     = to;
    g_trace.push(r);
}

global_stats stats_snapshot() {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    global_stats s = g_stats;
    auto& c        = cur_cpu();
    s.runq_depth   = c.run_queue.size();
    s.sleepers     = sleeper_count();
    s.zombies      = reaper_zombie_count();
    s.reaped       = reaper_reaped_count();
    // Charge the running thread's in-progress slice so idle/busy shares are current.
    if (c.current) {
        uint64_t now = kernel::arch::timestamp();
        c.current->stats().cpu_cycles += now - g_last_switch_ts;
        g_last_switch_ts = now;
    }
    if (c.idle) { s.idle_cycles = c.idle->stats().cpu_cycles; }
    kernel::arch::restore_interrupts(flags);
    return s;
}

size_t trace_copy_newest(trace_record* out, size_t max) {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    size_t n       = g_trace.copy_newest(out, max);
    kernel::arch::restore_interrupts(flags);
    return n;
}

void trace_clear() {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    g_trace.clear();
    kernel::arch::restore_interrupts(flags);
}

void set_lifecycle_log(bool enabled) { g_lifecycle_log = enabled; }
bool lifecycle_log_enabled() { return g_lifecycle_log; }
void set_lifecycle_log_verbose(bool enabled) { g_lifecycle_log_verbose = enabled; }
bool lifecycle_log_verbose_enabled() { return g_lifecycle_log && g_lifecycle_log_verbose; }

}  // namespace kernel::sched
