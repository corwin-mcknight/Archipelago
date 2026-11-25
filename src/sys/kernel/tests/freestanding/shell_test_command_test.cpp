#include <kernel/testing/testing.h>

#include "shell_capture.h"

KTEST(shell_test_unknown_name_escaped, "shell/test") {
    auto line = run_shell("test run a\"b\\c\n\r\x01");
    KTEST_EXPECT_TRUE(line ==
                      "@@HARNESS {\"event\":\"error\",\"message\":\"Test not found: a\\\"b\\\\c\\n\\r\\u0001\"}\n");
}
