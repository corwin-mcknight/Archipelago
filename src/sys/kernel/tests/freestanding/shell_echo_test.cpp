#include "kernel/testing/testing.h"
#include "shell_capture.h"

KTEST_MODULE("shell/echo");

// Arguments come back space-joined with a trailing newline; extra whitespace
// collapses in tokenization and UTF-8 bytes pass through untouched.
KTEST_CASE_INTEGRATION(shell_echo_prints_arguments) {
    KTEST_EXPECT_TRUE(run_shell("echo hello   w\xc3\xb6rld") == "hello w\xc3\xb6rld\n");
}
