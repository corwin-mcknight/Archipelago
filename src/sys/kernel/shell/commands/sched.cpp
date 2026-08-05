#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/arch.h>
#include <kernel/config.h>
#include <kernel/platform.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/trace.h>
#include <kernel/shell/output.h>
#include <kernel/shell/render.h>

#include <ktl/fmt>
#include <ktl/ref>
#include <ktl/string_view>
#include <ktl/vector>

namespace {

using namespace kernel::sched;
using kernel::shell::human_str;
using kernel::shell::print_stats_header;
using kernel::shell::print_stats_row;
using kernel::shell::sort_by_cpu;
using kernel::shell::state_name;

const char* kind_name(trace_kind k) {
    switch (k) {
        case trace_kind::SWITCH: return "SWITCH";
        case trace_kind::WAKE: return "WAKE";
        case trace_kind::SPAWN: return "SPAWN";
        case trace_kind::EXIT: return "EXIT";
        default: return "?";
    }
}

const char* reason_name(switch_reason r) {
    switch (r) {
        case switch_reason::NONE: return "-";
        case switch_reason::PREEMPT: return "preempt";
        case switch_reason::YIELD: return "yield";
        case switch_reason::BLOCK: return "block";
        case switch_reason::SLEEP: return "sleep";
        case switch_reason::EXIT: return "exit";
        default: return "-";
    }
}

// argv entries are string_views over the input line, not null-terminated -- parse by hand.
ktl::maybe<size_t> parse_size(ktl::string_view sv) {
    if (sv.size() == 0) { return ktl::nothing; }
    size_t v = 0;
    for (size_t i = 0; i < sv.size(); ++i) {
        if (sv[i] < '0' || sv[i] > '9') { return ktl::nothing; }
        v = v * 10 + static_cast<size_t>(sv[i] - '0');
    }
    return v;
}

void print_human(kernel::shell::ShellOutput& output, uint64_t cycles, uint64_t hz) {
    char buf[24];
    output.print("{0}", human_str(buf, sizeof(buf), cycles, hz));
}

// Threads are stored in their owning task (spawn_into puts them there); gather across every task so
// user threads show up alongside kernel ones.
bool snapshot_all_threads(ktl::vector<ktl::ref<Thread>>& out) {
    ktl::vector<ktl::ref<Task>> tasks;
    if (!snapshot_tasks(tasks)) { return false; }
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (!tasks[i]->snapshot_threads(out)) { return false; }
    }
    return true;
}

// Resolve an id through a threads snapshot; reaped threads print as bare ids.
const char* name_of(const ktl::vector<ktl::ref<Thread>>& threads, uint64_t id) {
    for (size_t i = 0; i < threads.size(); ++i) {
        if (threads[i]->id() == id) { return threads[i]->name() ? threads[i]->name() : "?"; }
    }
    return nullptr;
}

void cmd_threads(kernel::shell::ShellOutput& output, bool top) {
    uint64_t hz = kernel::platform::timestamp_hz();
    ktl::vector<ktl::ref<Thread>> threads;
    if (!snapshot_all_threads(threads)) {
        output.print("snapshot failed (oom)\n");
        return;
    }
    if (top) { sort_by_cpu(threads); }
    auto cur       = current();
    auto s         = stats_snapshot();
    uint64_t total = kernel::arch::timestamp() - s.boot_ts;
    print_stats_header(output);
    for (size_t i = 0; i < threads.size(); ++i) {
        auto& t     = threads[i];
        bool is_cur = cur && cur->id() == t->id();
        print_stats_row(output, is_cur ? ">" : " ", t->id(), t->name() ? t->name() : "?", state_name(t->state()),
                        t->stats(), total, hz);
    }
}

void cmd_stats(kernel::shell::ShellOutput& output) {
    auto s         = stats_snapshot();
    uint64_t hz    = kernel::platform::timestamp_hz();
    uint64_t total = kernel::arch::timestamp() - s.boot_ts;
    output.print("uptime: ");
    print_human(output, total, hz);
    if (total > 0) { output.print("  idle: {0}%", s.idle_cycles * 100 / total); }
    output.print("\nswitches: {0} (preempt {1}, yield {2}, block {3}, sleep {4}, exit {5})\n", s.switches, s.preempts,
                 s.yields, s.block_switches, s.sleep_switches, s.exit_switches);
    output.print("wakes: {0}  spawned: {1}  reaped: {2}\n", s.wakes, s.spawned, s.reaped);
    output.print("runq: {0}  sleepers: {1}  zombies: {2}\n", s.runq_depth, s.sleepers, s.zombies);
}

void cmd_trace_dump(kernel::shell::ShellOutput& output, size_t n) {
    static trace_record recs[CONFIG_SCHED_TRACE_EVENTS];
    if (n > CONFIG_SCHED_TRACE_EVENTS) { n = CONFIG_SCHED_TRACE_EVENTS; }
    size_t got  = trace_copy_newest(recs, n);
    uint64_t hz = kernel::platform::timestamp_hz();
    auto s      = stats_snapshot();
    ktl::vector<ktl::ref<Thread>> threads;
    snapshot_all_threads(threads);
    output.print("trace: {0} records (newest first, capacity {1})\n", got, CONFIG_SCHED_TRACE_EVENTS);
    for (size_t i = 0; i < got; ++i) {
        auto& r = recs[i];
        output.print("[t+");
        print_human(output, r.timestamp >= s.boot_ts ? r.timestamp - s.boot_ts : 0, hz);
        output.print("] {0}", kind_name(r.kind));
        if (r.kind == trace_kind::SWITCH) { output.print(" ({0})", reason_name(r.reason)); }
        const char* from = name_of(threads, r.from_id);
        const char* to   = name_of(threads, r.to_id);
        if (r.from_id != 0) { output.print(" from={0}", r.from_id); }
        if (from != nullptr) { output.print("'{0}'", from); }
        if (r.to_id != 0) { output.print(" to={0}", r.to_id); }
        if (to != nullptr) { output.print("'{0}'", to); }
        output.print("\n");
    }
}

void sched_handler(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: sched threads|top|stats|trace dump [n]|trace clear|log on|off|verbose\n");
        return;
    }
    if (argv[1] == "threads") {
        cmd_threads(output, /*top=*/false);
    } else if (argv[1] == "top") {
        cmd_threads(output, /*top=*/true);
    } else if (argv[1] == "stats") {
        cmd_stats(output);
    } else if (argv[1] == "trace") {
        if (argc >= 3 && argv[2] == "clear") {
            trace_clear();
            output.print("trace cleared\n");
        } else if (argc >= 3 && argv[2] == "dump") {
            size_t n = 32;
            if (argc >= 4) {
                auto parsed = parse_size(argv[3]);
                if (!parsed.has_value()) {
                    output.print("bad count: {0}\n", argv[3]);
                    return;
                }
                n = *parsed;
            }
            cmd_trace_dump(output, n);
        } else {
            output.print("usage: sched trace dump [n]|clear\n");
        }
    } else if (argv[1] == "log") {
        if (argc >= 3 && argv[2] == "on") {
            set_lifecycle_log(true);
            set_lifecycle_log_verbose(false);
            output.print("lifecycle log on\n");
        } else if (argc >= 3 && argv[2] == "verbose") {
            set_lifecycle_log(true);
            set_lifecycle_log_verbose(true);
            output.print("lifecycle log verbose\n");
        } else if (argc >= 3 && argv[2] == "off") {
            set_lifecycle_log(false);
            set_lifecycle_log_verbose(false);
            output.print("lifecycle log off\n");
        } else {
            output.print("usage: sched log on|off|verbose\n");
        }
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(sched, "sched", "Scheduler inspection: threads, stats, trace, lifecycle log", sched_handler);

#endif  // CONFIG_KERNEL_SHELL
