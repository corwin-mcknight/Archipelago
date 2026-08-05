#include <stddef.h>
#include <stdint.h>

#include "kernel/config.h"
#include "kernel/shell/shell.h"
#include "kernel/testing/testing.h"

#if CONFIG_KERNEL_SHELL

// Drives the mem shell command through run_line with a capture sink and
// asserts that its plain summary is also clean in protocol mode.

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

KTEST_CASE_INTEGRATION(shell_mem_summary) {
    {
        auto out = make_capture_output();
        kernel::shell::run_line("mem", out);
        KTEST_EXPECT_TRUE(captured_contains("physical:"));
        KTEST_EXPECT_TRUE(captured_contains("pages:"));
        KTEST_EXPECT_TRUE(captured_contains("heap:"));
        KTEST_EXPECT_TRUE(captured_contains("kernel aspace:"));
    }
}

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
        KTEST_EXPECT_TRUE(captured_contains("physical:"));
        KTEST_EXPECT_TRUE(captured_contains("pmm:"));
    }
}

#endif  // CONFIG_KERNEL_SHELL
