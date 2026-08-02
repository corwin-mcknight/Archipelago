#include <kernel/log.h>
#include <kernel/obj/handle_dispatch.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>
#include <kernel/synchronization/execution_context.h>
#include <kernel/syscall.h>

namespace {

// ponytail: one global line buffer; per-task buffers when concurrent user tasks interleave output.
constexpr size_t k_debug_line_max = 120;
char g_debug_line[k_debug_line_max + 1];
size_t g_debug_len = 0;

void log_putc(char c) {
    if (c != '\n') {
        g_debug_line[g_debug_len++] = c;
        if (g_debug_len < k_debug_line_max) { return; }
    }
    g_debug_line[g_debug_len] = '\0';
    g_log.info("user: {0}", static_cast<const char*>(g_debug_line));
    g_debug_len = 0;
}

// Emit [offset, offset + length) of the calling thread's IPC buffer. Output goes through the log
// rather than straight at the UART so it stays serialized against kernel log output and the test
// harness's protocol lines, which share the device.
//
// No user pointer is involved: the buffer's frames were resolved when the thread was created, so
// this validates a range against a size the kernel already knows and then reads its own physmap.
uint64_t sys_write(uint64_t offset, uint64_t length) {
    auto self = kernel::sched::current();
    if (!self) { return static_cast<uint64_t>(ktl::errc::invalid_operation); }

    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, length)) { return static_cast<uint64_t>(ktl::errc::out_of_range); }

    // Page by page: the backing frames are not physically contiguous, so a multi-page buffer is
    // several runs. A one-page buffer is a single pass.
    uint64_t written = 0;
    while (written < length) {
        size_t run         = 0;
        const char* chunk  = reinterpret_cast<const char*>(buffer.kernel_at(offset + written, run));
        uint64_t remaining = length - written;
        uint64_t take      = run < remaining ? run : remaining;
        for (uint64_t i = 0; i < take; ++i) { log_putc(chunk[i]); }
        written += take;
    }
    return written;
}

// A handle names an entry in the calling task's table, so resolve the caller before entering the
// pipeline. Kernel threads land on task zero's table, which is what lets kernel-context tests
// drive the real path end to end.
uint64_t handle_syscall(uint64_t nr, uint64_t a0, uint64_t a1) {
    auto self = kernel::sched::current();
    if (!self) { return static_cast<uint64_t>(ktl::errc::invalid_operation); }
    auto task = ktl::static_ref_cast<kernel::sched::Task>(self->owner());
    if (!task) { task = kernel::sched::kernel_task(); }
    return kernel::obj::dispatch_handle_op(task->handles(), nr, a0, a1);
}

}  // namespace

extern "C" uint64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                     uint64_t a5) {
    // a2..a5 are carried by the entry paths but no operation reads past a1 yet.
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    kernel::synchronization::syscall_enter();
    uint64_t ret = 0;
    switch (nr) {
        case kernel::syscall::SYS_EXIT:
            kernel::synchronization::syscall_exit();
            kernel::sched::exit_current();
            break;
        case kernel::syscall::SYS_YIELD: kernel::sched::yield(); break;
        case kernel::syscall::SYS_SLEEP: kernel::sched::sleep_ticks(a0); break;
        case kernel::syscall::SYS_WRITE: ret = sys_write(a0, a1); break;
        case kernel::syscall::SYS_HANDLE_CLOSE:
        case kernel::syscall::SYS_HANDLE_DUPLICATE:
        case kernel::syscall::SYS_OBJ_INFO: ret = handle_syscall(nr, a0, a1); break;
        default: kernel::synchronization::syscall_exit(); return static_cast<uint64_t>(-1);
    }
    kernel::synchronization::syscall_exit();
    return ret;
}
