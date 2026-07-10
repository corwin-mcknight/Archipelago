#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/arch.h>
#include <kernel/config.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/trace.h>
#include <kernel/shell/output.h>

#include <ktl/ref>
#include <ktl/string_view>
#include <ktl/vector>

namespace {

using namespace kernel::sched;

const char* state_name(thread_state s) {
    switch (s) {
        case thread_state::READY: return "READY";
        case thread_state::RUNNING: return "RUNNING";
        case thread_state::BLOCKED: return "BLOCKED";
        case thread_state::DEAD: return "DEAD";
        default: return "?";
    }
}

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
    auto h = cycles_to_human(cycles, hz);
    if (h.unit[0] == 'c' || h.unit[0] == 'u') {
        output.print("{0}{1}", h.whole, h.unit);
    } else {
        output.print("{0}.{1}{2}{3}", h.whole, h.hundredths / 10, h.hundredths % 10, h.unit);
    }
}

// Resolve an id through a threads snapshot; reaped threads print as bare ids.
const char* name_of(const ktl::vector<ktl::ref<Thread>>& threads, uint64_t id) {
    for (size_t i = 0; i < threads.size(); ++i) {
        if (threads[i]->id() == id) { return threads[i]->name() ? threads[i]->name() : "?"; }
    }
    return nullptr;
}

void cmd_threads(kernel::shell::ShellOutput& output, bool top) {
    uint64_t hz = kernel::arch::timestamp_hz();
    ktl::vector<ktl::ref<Thread>> threads;
    if (!kernel_task()->snapshot_threads(threads)) {
        output.print("snapshot failed (oom)\n");
        return;
    }
    if (top) {
        // Insertion sort by cpu_cycles descending; thread counts are single digits.
        for (size_t i = 1; i < threads.size(); ++i) {
            for (size_t j = i; j > 0 && threads[j]->stats().cpu_cycles > threads[j - 1]->stats().cpu_cycles; --j) {
                ktl::ref<Thread> tmp = threads[j];
                threads[j]           = threads[j - 1];
                threads[j - 1]       = tmp;
            }
        }
    }
    auto cur       = current();
    auto s         = stats_snapshot();
    uint64_t total = kernel::arch::timestamp() - s.boot_ts;
    output.print("  ID NAME       STATE    CPU-TIME  {0}SCHED PRE  YLD  BLK  SLP  WAKE LAT-AVG LAT-MAX\n",
                 top ? "%CPU " : "");
    for (size_t i = 0; i < threads.size(); ++i) {
        auto& t     = threads[i];
        auto& st    = t->stats();
        bool is_cur = cur && cur->id() == t->id();
        output.print("{0} {1} {2} {3} ", is_cur ? ">" : " ", t->id(), t->name() ? t->name() : "?",
                     state_name(t->state()));
        print_human(output, st.cpu_cycles, hz);
        if (top && total > 0) { output.print(" {0}%", st.cpu_cycles * 100 / total); }
        output.print(" {0} {1} {2} {3} {4} {5} ", st.scheduled, st.preemptions, st.yields, st.blocks, st.sleeps,
                     st.wakes);
        print_human(output, st.scheduled ? st.lat_total_cycles / st.scheduled : 0, hz);
        output.print(" ");
        print_human(output, st.lat_max_cycles, hz);
        output.print("\n");
    }
}

void cmd_stats(kernel::shell::ShellOutput& output) {
    auto s         = stats_snapshot();
    uint64_t hz    = kernel::arch::timestamp_hz();
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
    uint64_t hz = kernel::arch::timestamp_hz();
    auto s      = stats_snapshot();
    ktl::vector<ktl::ref<Thread>> threads;
    kernel_task()->snapshot_threads(threads);
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
        output.print("usage: sched threads|top|stats|trace dump [n]|trace clear|log on|off\n");
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
            output.print("lifecycle log on\n");
        } else if (argc >= 3 && argv[2] == "off") {
            set_lifecycle_log(false);
            output.print("lifecycle log off\n");
        } else {
            output.print("usage: sched log on|off\n");
        }
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(sched, "sched", "Scheduler inspection: threads, stats, trace, lifecycle log", sched_handler);

#endif  // CONFIG_KERNEL_SHELL
