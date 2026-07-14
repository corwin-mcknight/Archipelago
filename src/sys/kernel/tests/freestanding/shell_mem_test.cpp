#include <stddef.h>
#include <stdint.h>

#include "kernel/config.h"
#include "kernel/shell/shell.h"
#include "kernel/testing/testing.h"

#if CONFIG_KERNEL_SHELL

// Drives the mem shell command through run_line with a capture sink and
// asserts on the rendered text: subcommand routing, protocol-mode escape
// hygiene, and error handling.

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

KTEST_MODULE("shell/mem");

// Normal-mode story: subcommand routing renders only the requested section, and unknown
// subcommands print usage. Each phase resets the capture buffer via make_capture_output.
KTEST_CASE_INTEGRATION(shell_mem_normal_mode_routing_and_usage) {
    {
        auto out = make_capture_output();
        kernel::shell::run_line("mem heap", out);
        KTEST_EXPECT_TRUE(captured_contains("heap (early)"));
        KTEST_EXPECT_TRUE(captured_contains("largest free"));
        KTEST_EXPECT_TRUE(!captured_contains("kernel aspace"));
        KTEST_EXPECT_TRUE(!captured_contains("physical"));
    }
    {
        auto out = make_capture_output();
        kernel::shell::run_line("mem bogus", out);
        KTEST_EXPECT_TRUE(captured_contains("usage: mem"));
    }
}

// Protocol-mode story: the full dump is escape-clean while keeping field lines, and
// interactive-only subcommands refuse to run.
KTEST_CASE_INTEGRATION(shell_mem_protocol_mode_escape_hygiene) {
    {
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
    {
        auto out = make_capture_output();
        out.set_protocol_mode(true);
        kernel::shell::run_line("mem top", out);
        KTEST_EXPECT_TRUE(captured_contains("disabled in protocol mode"));
    }
}

#endif  // CONFIG_KERNEL_SHELL
