#include <stddef.h>

#include "kernel/boot.h"
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

ktl::string_view run(const char* line) {
    g_capture_len = 0;
    kernel::shell::ShellOutput out;
    out.set_sink(capture_sink, nullptr);
    kernel::shell::run_line(line, out);
    g_capture[g_capture_len] = '\0';
    return ktl::string_view(g_capture);
}

}  // namespace

KTEST_MODULE("shell/date");

// Known epochs pin the civil-from-days conversion: origin, a leap day on a
// century year, and the last second before a non-leap century rollover.
KTEST_CASE_INTEGRATION(shell_date_formats_known_epochs) {
    KTEST_EXPECT_TRUE(run("date 0") == "1970-01-01 00:00:00 UTC\n");
    KTEST_EXPECT_TRUE(run("date 951782400") == "2000-02-29 00:00:00 UTC\n");
    KTEST_EXPECT_TRUE(run("date 4102444799") == "2099-12-31 23:59:59 UTC\n");
    KTEST_EXPECT_TRUE(run("date notanumber") == "usage: date [epoch-seconds]\n");
}

// End to end through the boot protocol: Limine supplies a date at boot, so the
// no-argument form prints a real current date rather than the fallback message.
KTEST_CASE_INTEGRATION(shell_date_reads_boot_protocol_clock) {
    KTEST_EXPECT_TRUE(kernel::boot::collect().boot_epoch_seconds > 0);
    ktl::string_view line = run("date");
    KTEST_EXPECT_TRUE(line.size() == sizeof("YYYY-MM-DD HH:MM:SS UTC\n") - 1);
    // The kernel was built in the 21st century; a sane wall clock cannot predate its own code.
    KTEST_EXPECT_TRUE(line.substr(0, 2) == "20");
    KTEST_EXPECT_TRUE(line[4] == '-' && line[7] == '-' && line[10] == ' ' && line[13] == ':' && line[16] == ':');
}

#endif  // CONFIG_KERNEL_SHELL
