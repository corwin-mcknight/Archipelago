#include <stddef.h>

#include "kernel/config.h"
#include "kernel/shell/shell.h"
#include "kernel/testing/testing.h"

#if CONFIG_KERNEL_SHELL

#include <ktl/string_view>

namespace {

char g_capture[256];
size_t g_capture_len = 0;

void capture_sink(char c, void*) {
    if (g_capture_len < sizeof(g_capture) - 1) { g_capture[g_capture_len++] = c; }
}

}  // namespace

KTEST_MODULE("shell/echo");

// Arguments come back space-joined with a trailing newline; extra whitespace
// collapses in tokenization and UTF-8 bytes pass through untouched.
KTEST_CASE_INTEGRATION(shell_echo_prints_arguments) {
    kernel::shell::ShellOutput out;
    out.set_sink(capture_sink, nullptr);
    kernel::shell::run_line("echo hello   w\xc3\xb6rld", out);
    g_capture[g_capture_len] = '\0';
    KTEST_EXPECT_TRUE(ktl::string_view(g_capture) == "hello w\xc3\xb6rld\n");
}

#endif  // CONFIG_KERNEL_SHELL
