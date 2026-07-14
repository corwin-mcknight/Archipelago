#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/arch.h>
#include <kernel/config.h>
#include <kernel/drivers/uart.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/trace.h>
#include <kernel/shell/output.h>
#include <kernel/shell/render.h>

#include <ktl/fmt>
#include <ktl/ref>
#include <ktl/string_view>
#include <ktl/vector>

extern kernel::driver::uart uart;

namespace {

using namespace kernel::sched;
using kernel::shell::human_str;
using kernel::shell::ShellOutput;
using kernel::shell::state_name;

constexpr const char* C_DIM   = "2";
constexpr const char* C_TITLE = "1;36";
constexpr const char* C_TASK  = "1;33";

// Header and every row share one format so columns cannot drift apart.
constexpr const char* ROW_FMT =
    "{0}{1:3} {2:-12} {3:-8} {4:9} {5:5} {6:5} {7:5} {8:5} {9:5} {10:5} {11:5} {12:9} {13:9}\n";

void print_header(ShellOutput& out) {
    out.print(ROW_FMT, " ", "ID", "NAME", "STATE", "CPU-TIME", "%CPU", "SCHED", "PRE", "YLD", "BLK", "SLP", "WAKE",
              "LAT-AVG", "LAT-MAX");
}

// Sums a task's threads into one stat line; lat_max is the max across threads, lat_total the sum
// (so the row's LAT-AVG is total/scheduled, the same weighting sched top uses per thread).
thread_stats aggregate(const ktl::vector<ktl::ref<Thread>>& threads) {
    thread_stats agg;
    for (size_t i = 0; i < threads.size(); ++i) {
        const auto& st = threads[i]->stats();
        agg.cpu_cycles += st.cpu_cycles;
        agg.scheduled += st.scheduled;
        agg.preemptions += st.preemptions;
        agg.yields += st.yields;
        agg.blocks += st.blocks;
        agg.sleeps += st.sleeps;
        agg.wakes += st.wakes;
        agg.lat_total_cycles += st.lat_total_cycles;
        if (st.lat_max_cycles > agg.lat_max_cycles) { agg.lat_max_cycles = st.lat_max_cycles; }
    }
    return agg;
}

void print_row(ShellOutput& out, const char* marker, uint64_t id, const char* name, const char* state,
               const thread_stats& st, uint64_t total, uint64_t hz) {
    char cpu_buf[24], pct_buf[8], lat_avg_buf[24], lat_max_buf[24];
    if (total > 0) {
        ktl::format::format_to_buffer_raw(pct_buf, sizeof(pct_buf), "{0}%", st.cpu_cycles * 100 / total);
    } else {
        ktl::format::format_to_buffer_raw(pct_buf, sizeof(pct_buf), "-");
    }
    out.print(ROW_FMT, marker, id, name, state, human_str(cpu_buf, sizeof(cpu_buf), st.cpu_cycles, hz), pct_buf,
              st.scheduled, st.preemptions, st.yields, st.blocks, st.sleeps, st.wakes,
              human_str(lat_avg_buf, sizeof(lat_avg_buf), st.scheduled ? st.lat_total_cycles / st.scheduled : 0, hz),
              human_str(lat_max_buf, sizeof(lat_max_buf), st.lat_max_cycles, hz));
}

void render_tree(ShellOutput& out) {
    uint64_t hz    = kernel::arch::timestamp_hz();
    auto s         = stats_snapshot();
    uint64_t total = kernel::arch::timestamp() - s.boot_ts;
    auto cur       = current();

    ktl::vector<ktl::ref<Task>> tasks;
    if (!snapshot_tasks(tasks)) {
        out.print("snapshot failed (oom)\n");
        return;
    }

    // Aggregate cpu per task to order the tree; counts are tiny so insertion sort is fine.
    ktl::vector<uint64_t> task_cpu;
    for (size_t i = 0; i < tasks.size(); ++i) {
        ktl::vector<ktl::ref<Thread>> th;
        tasks[i]->snapshot_threads(th);
        if (!task_cpu.push_back(aggregate(th).cpu_cycles)) {
            out.print("snapshot failed (oom)\n");
            return;
        }
    }
    for (size_t i = 1; i < tasks.size(); ++i) {
        for (size_t j = i; j > 0 && task_cpu[j] > task_cpu[j - 1]; --j) {
            ktl::ref<Task> tt = tasks[j];
            tasks[j]          = tasks[j - 1];
            tasks[j - 1]      = tt;
            uint64_t tc       = task_cpu[j];
            task_cpu[j]       = task_cpu[j - 1];
            task_cpu[j - 1]   = tc;
        }
    }

    print_header(out);
    for (size_t i = 0; i < tasks.size(); ++i) {
        auto& task = tasks[i];
        ktl::vector<ktl::ref<Thread>> threads;
        task->snapshot_threads(threads);

        // Threads within a task by cpu descending, same as sched top.
        for (size_t a = 1; a < threads.size(); ++a) {
            for (size_t b = a; b > 0 && threads[b]->stats().cpu_cycles > threads[b - 1]->stats().cpu_cycles; --b) {
                ktl::ref<Thread> tmp = threads[b];
                threads[b]           = threads[b - 1];
                threads[b - 1]       = tmp;
            }
        }

        bool owns_cur = false;
        for (size_t t = 0; cur && t < threads.size(); ++t) {
            if (threads[t]->id() == cur->id()) { owns_cur = true; }
        }

        out.sgr(C_TASK);
        char task_name[20];
        const char* tn = task->name();
        if (tn == nullptr) {
            ktl::format::format_to_buffer_raw(task_name, sizeof(task_name), "task#{0}", task->id());
            tn = task_name;
        }
        print_row(out, owns_cur ? "▸" : " ", static_cast<uint64_t>(task->id()), tn, "-", aggregate(threads), total, hz);
        out.reset_style();

        for (size_t t = 0; t < threads.size(); ++t) {
            auto& th    = threads[t];
            bool is_cur = cur && cur->id() == th->id();
            char name_buf[16];
            ktl::format::format_to_buffer_raw(name_buf, sizeof(name_buf), " {0}", th->name() ? th->name() : "?");
            print_row(out, is_cur ? ">" : " ", static_cast<uint64_t>(th->id()), name_buf, state_name(th->state()),
                      th->stats(), total, hz);
        }
    }
}

void cmd_top(ShellOutput& out) {
    if (out.protocol_mode()) {
        out.print("top: interactive only; disabled in protocol mode\n");
        return;
    }
    for (;;) {
        while (uart.received_data() != 0) {
            char c = uart.read();
            if (c == 'x' || c == 'q') {
                out.write("\x1b[0m\n");
                return;
            }
        }
        out.write("\x1b[H\x1b[2J");
        out.sgr(C_TITLE);
        out.write("top");
        out.sgr(C_DIM);
        out.write("  --  press x to exit\n\n");
        out.reset_style();
        render_tree(out);
        kernel::sched::sleep_ticks(500);
    }
}

void top_handler(int, const ktl::string_view[], ShellOutput& output) { cmd_top(output); }

}  // namespace

KSHELL_COMMAND(top, "top", "Live process/thread tree with scheduler stats", top_handler);

#endif  // CONFIG_KERNEL_SHELL
