#include <ktl/string_view>

#include "kernel/testing/testing.h"
#include "shell_capture.h"

// Drives the mem shell command through run_line with a capture sink and
// asserts that its plain summary is also clean in protocol mode.

KTEST_MODULE("shell/mem");

KTEST_CASE_INTEGRATION(shell_mem_summary) {
    ktl::string_view out = run_shell("mem");
    KTEST_EXPECT_TRUE(contains(out, "physical:"));
    KTEST_EXPECT_TRUE(contains(out, "pages:"));
    KTEST_EXPECT_TRUE(contains(out, "heap:"));
    KTEST_EXPECT_TRUE(contains(out, "kernel aspace:"));
}

KTEST_CASE_INTEGRATION(shell_mem_protocol_mode_escape_hygiene) {
    ktl::string_view out = run_shell("mem", true);
    KTEST_EXPECT_TRUE(out.size() > 0);
    bool clean = true;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\x1b') { clean = false; }
    }
    KTEST_EXPECT_TRUE(clean);
    KTEST_EXPECT_TRUE(contains(out, "physical:"));
    KTEST_EXPECT_TRUE(contains(out, "pmm:"));
}
