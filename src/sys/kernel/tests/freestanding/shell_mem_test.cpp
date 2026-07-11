#include <stddef.h>
#include <stdint.h>

#include "kernel/config.h"
#include "kernel/mm/pmm.h"
#include "kernel/shell/shell.h"
#include "kernel/testing/testing.h"

#if CONFIG_KERNEL_SHELL

// Drives the mem shell command through run_line with a capture sink and
// asserts on the rendered text: every section present, accounting sane, and
// protocol mode free of raw escape bytes.

namespace {

constexpr size_t CAPTURE_MAX = 32768;
char g_capture[CAPTURE_MAX];
size_t g_capture_len = 0;

void capture_sink(char c, void*) {
    if (g_capture_len < CAPTURE_MAX - 1) { g_capture[g_capture_len++] = c; }
}

kernel::shell::ShellOutput make_capture_output() {
    g_capture_len = 0;
    g_capture[0]  = '\0';
    kernel::shell::ShellOutput out;
    out.set_sink(capture_sink, nullptr);
    return out;
}

bool captured_contains(const char* needle) {
    g_capture[g_capture_len] = '\0';
    size_t nlen              = 0;
    while (needle[nlen] != '\0') { ++nlen; }
    if (nlen == 0 || nlen > g_capture_len) { return false; }
    for (size_t i = 0; i + nlen <= g_capture_len; ++i) {
        size_t j = 0;
        while (j < nlen && g_capture[i + j] == needle[j]) { ++j; }
        if (j == nlen) { return true; }
    }
    return false;
}

}  // namespace

KTEST_INTEGRATION(shell_mem_full_view_has_all_sections, "shell/mem") {
    auto out = make_capture_output();
    kernel::shell::run_line("mem", out);
    KTEST_EXPECT_TRUE(captured_contains("physical"));
    KTEST_EXPECT_TRUE(captured_contains("pages"));
    KTEST_EXPECT_TRUE(captured_contains("heap (early)"));
    KTEST_EXPECT_TRUE(captured_contains("kernel aspace"));
    KTEST_EXPECT_TRUE(captured_contains("vmo"));
    KTEST_EXPECT_TRUE(captured_contains("allocs"));
    KTEST_EXPECT_TRUE(captured_contains("low-water"));
}

KTEST_INTEGRATION(shell_mem_subcommand_renders_single_section, "shell/mem") {
    auto out = make_capture_output();
    kernel::shell::run_line("mem heap", out);
    KTEST_EXPECT_TRUE(captured_contains("heap (early)"));
    KTEST_EXPECT_TRUE(captured_contains("largest free"));
    KTEST_EXPECT_TRUE(!captured_contains("kernel aspace"));
    KTEST_EXPECT_TRUE(!captured_contains("physical"));
}

KTEST_INTEGRATION(shell_mem_accounting_reconciles, "shell/mem") {
    auto s = kernel::mm::g_page_frame_allocator.stats();
    KTEST_EXPECT_TRUE(s.total_pages > 0);
    KTEST_EXPECT_TRUE(s.free_pages + s.reserved_pages <= s.total_pages);
    KTEST_EXPECT_TRUE(s.zeroed_pooled + s.zeroed_region_tail <= s.free_pages);
    KTEST_EXPECT_TRUE(s.low_water <= s.total_pages);
}

KTEST_INTEGRATION(shell_mem_protocol_mode_is_escape_clean, "shell/mem") {
    auto out = make_capture_output();
    out.set_protocol_mode(true);
    kernel::shell::run_line("mem", out);
    KTEST_EXPECT_TRUE(g_capture_len > 0);
    bool clean = true;
    for (size_t i = 0; i < g_capture_len; ++i) {
        if (g_capture[i] == '\x1b') { clean = false; }
    }
    KTEST_EXPECT_TRUE(clean);
    // Field lines still present for harness matching.
    KTEST_EXPECT_TRUE(captured_contains("physical"));
    KTEST_EXPECT_TRUE(captured_contains("allocs"));
}

KTEST_INTEGRATION(shell_mem_top_refuses_protocol_mode, "shell/mem") {
    auto out = make_capture_output();
    out.set_protocol_mode(true);
    kernel::shell::run_line("mem top", out);
    KTEST_EXPECT_TRUE(captured_contains("disabled in protocol mode"));
}

KTEST_INTEGRATION(shell_mem_unknown_subcommand_prints_usage, "shell/mem") {
    auto out = make_capture_output();
    kernel::shell::run_line("mem bogus", out);
    KTEST_EXPECT_TRUE(captured_contains("usage: mem"));
}

#endif  // CONFIG_KERNEL_SHELL
