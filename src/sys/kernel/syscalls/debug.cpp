#include <kernel/log.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/mm/vmo.h>
#include <kernel/obj/channel.h>
#include <kernel/obj/handle_dispatch.h>
#include <kernel/obj/port.h>
#include <kernel/obj/socket.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/user_task.h>
#include <kernel/time.h>

#include "internal.h"

namespace kernel::syscalls {

namespace {

// ponytail: one global line buffer; per-task buffers when concurrent user tasks interleave output.
constexpr size_t k_debug_line_max = 120;
char g_debug_line[k_debug_line_max + 1];
size_t g_debug_len = 0;

void log_flush_line() {
    g_debug_line[g_debug_len] = '\0';
    g_log.info("user: {0}", static_cast<const char*>(g_debug_line));
    g_debug_len = 0;
}

// Every exit leaves g_debug_len < k_debug_line_max, so the append below is always in bounds even
// when two threads' writes interleave at a flush boundary.
void log_putc(char c) {
    if (c == '\n') {
        log_flush_line();
        return;
    }
    g_debug_line[g_debug_len++] = c;
    if (g_debug_len == k_debug_line_max) { log_flush_line(); }
}

}  // namespace

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

}  // namespace kernel::syscalls
