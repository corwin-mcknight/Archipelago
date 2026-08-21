#pragma once

#include <kernel/sched/thread.h>
#include <kernel/sched/trace.h>
#include <kernel/shell/output.h>

#include <ktl/fmt>
#include <ktl/ref>
#include <ktl/vector>

// Display helpers shared by the scheduler-inspection shell commands (sched, top).

namespace kernel::shell {

inline const char* state_name(kernel::sched::thread_state s) {
    using kernel::sched::thread_state;
    switch (s) {
        case thread_state::READY: return "READY";
        case thread_state::RUNNING: return "RUNNING";
        case thread_state::BLOCKED: return "BLOCKED";
        case thread_state::DEAD: return "DEAD";
        default: return "?";
    }
}

// Renders a cycle count into buf ("1.71s", "454us", "123cyc") and returns buf for inline use.
inline const char* human_str(char* buf, size_t len, uint64_t cycles, uint64_t hz) {
    auto h = kernel::sched::cycles_to_human(cycles, hz);
    if (h.unit[0] == 'c' || h.unit[0] == 'u') {
        ktl::format::format_to_buffer_raw(buf, len, "{0}{1}", h.whole, h.unit);
    } else {
        ktl::format::format_to_buffer_raw(buf, len, "{0}.{1}{2}{3}", h.whole, h.hundredths / 10, h.hundredths % 10,
                                          h.unit);
    }
    return buf;
}

// Sorts threads by consumed CPU, descending. Insertion sort; thread counts are single digits.
inline void sort_by_cpu(ktl::vector<ktl::ref<kernel::sched::Thread>>& threads) {
    for (size_t i = 1; i < threads.size(); ++i) {
        for (size_t j = i; j > 0 && threads[j]->stats().cpu_cycles > threads[j - 1]->stats().cpu_cycles; --j) {
            ktl::ref<kernel::sched::Thread> tmp = threads[j];
            threads[j]                          = threads[j - 1];
            threads[j - 1]                      = tmp;
        }
    }
}

// Header and every row share one format so the columns cannot drift apart.
inline constexpr const char* STATS_ROW_FMT =
    "{0}{1:3} {2:-12} {3:-8} {4:4} {5:9} {6:5} {7:5} {8:5} {9:5} {10:5} {11:5} {12:5} {13:5} {14:9} {15:9}\n";

inline void print_stats_header(ShellOutput& out) {
    out.print(STATS_ROW_FMT, " ", "ID", "NAME", "STATE", "CORE", "CPU-TIME", "%CPU", "SCHED", "PRE", "YLD", "BLK",
              "SLP", "WAKE", "MIG", "LAT-AVG", "LAT-MAX");
}

inline void print_stats_row(ShellOutput& out, const char* marker, uint64_t id, const char* name, const char* state,
                            const kernel::sched::thread_stats& st, uint64_t total, uint64_t hz) {
    char cpu_buf[24], pct_buf[8], core_buf[8], lat_avg_buf[24], lat_max_buf[24];
    if (st.last_core == kernel::sched::thread_stats::NO_CORE) {
        ktl::format::format_to_buffer_raw(core_buf, sizeof(core_buf), "-");
    } else {
        ktl::format::format_to_buffer_raw(core_buf, sizeof(core_buf), "{0}", st.last_core);
    }
    if (total > 0) {
        ktl::format::format_to_buffer_raw(pct_buf, sizeof(pct_buf), "{0}%", st.cpu_cycles * 100 / total);
    } else {
        ktl::format::format_to_buffer_raw(pct_buf, sizeof(pct_buf), "-");
    }
    out.print(STATS_ROW_FMT, marker, id, name, state, core_buf, human_str(cpu_buf, sizeof(cpu_buf), st.cpu_cycles, hz),
              pct_buf, st.scheduled, st.preemptions, st.yields, st.blocks, st.sleeps, st.wakes, st.migrations,
              human_str(lat_avg_buf, sizeof(lat_avg_buf), st.scheduled ? st.lat_total_cycles / st.scheduled : 0, hz),
              human_str(lat_max_buf, sizeof(lat_max_buf), st.lat_max_cycles, hz));
}

}  // namespace kernel::shell
