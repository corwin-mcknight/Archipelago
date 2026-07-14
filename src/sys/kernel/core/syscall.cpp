#include <kernel/log.h>
#include <kernel/sched/scheduler.h>
#include <kernel/syscall.h>

namespace {

// ponytail: one global line buffer; per-task buffers when concurrent user tasks interleave output.
constexpr size_t k_debug_line_max = 120;
char g_debug_line[k_debug_line_max + 1];
size_t g_debug_len = 0;

void debug_putc(char c) {
    if (c != '\n') {
        g_debug_line[g_debug_len++] = c;
        if (g_debug_len < k_debug_line_max) { return; }
    }
    g_debug_line[g_debug_len] = '\0';
    g_log.info("user: {0}", static_cast<const char*>(g_debug_line));
    g_debug_len = 0;
}

}  // namespace

extern "C" uint64_t syscall_dispatch(uint64_t nr, uint64_t arg0) {
    switch (nr) {
        case kernel::syscall::SYS_EXIT: kernel::sched::exit_current(); break;  // noreturn; break is unreachable
        case kernel::syscall::SYS_YIELD: kernel::sched::yield(); return 0;
        case kernel::syscall::SYS_SLEEP: kernel::sched::sleep_ticks(arg0); return 0;
        case kernel::syscall::SYS_DEBUG_PUTC: debug_putc(static_cast<char>(arg0)); return 0;
        default: return static_cast<uint64_t>(-1);
    }
}
