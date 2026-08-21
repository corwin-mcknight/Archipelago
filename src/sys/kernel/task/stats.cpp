// src/sys/kernel/task/stats.cpp
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
    sched_guard guard(g_sched_lock);
    global_stats s = g_stats;
    s.runq_depth   = run_queue_depth_locked();
    s.sleepers     = sleeper_count();
    s.zombies      = reaper_zombie_count();
    s.reaped       = reaper_reaped_count();
    // Charge every core's running thread its in-progress slice so idle/busy shares are current;
    // the anchors are scheduler-lock state, so another core's switch_to cannot race this.
    uint64_t now   = kernel::arch::timestamp();
    for (size_t i = 0; i < CONFIG_MAX_CORES; i++) {
        auto& c = cpu_at(i);
        if (c.current) {
            c.current->stats().cpu_cycles += now - c.last_switch_ts;
            c.last_switch_ts      = now;
            s.cores[i].online     = true;
            s.cores[i].switches   = c.switches;
            s.cores[i].current_id = c.current->id();
        }
        if (c.idle) {
            s.cores[i].idle_cycles = c.idle->stats().cpu_cycles;
            s.idle_cycles += c.idle->stats().cpu_cycles;
        }
    }
    return s;
}

size_t trace_copy_newest(trace_record* out, size_t max) {
    sched_guard guard(g_sched_lock);
    return g_trace.copy_newest(out, max);
}

void trace_clear() {
    sched_guard guard(g_sched_lock);
    g_trace.clear();
}

void set_lifecycle_log(bool enabled) { g_lifecycle_log = enabled; }
bool lifecycle_log_enabled() { return g_lifecycle_log; }
void set_lifecycle_log_verbose(bool enabled) { g_lifecycle_log_verbose = enabled; }
bool lifecycle_log_verbose_enabled() { return g_lifecycle_log && g_lifecycle_log_verbose; }

}  // namespace kernel::sched
