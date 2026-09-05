#include <kernel/log.h>

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

// Use the logger to serialize output with kernel logs and test protocol lines.
uint64_t sys_write(sched::Thread& self, uint64_t offset, uint64_t length) {
    const auto& buffer = self.ipc();
    auto checked       = buffer.range(offset, length);
    if (checked.is_err()) { return errc_of(checked.unwrap_err()); }
    auto bytes       = checked.unwrap();

    uint64_t written = 0;
    for (auto chunk = bytes.next(); !chunk.empty(); chunk = bytes.next()) {
        for (uint8_t byte : chunk) { log_putc(static_cast<char>(byte)); }
        written += chunk.size();
    }
    return written;
}

}  // namespace kernel::syscalls
