#include <ktl/string_view>

#include "kernel/boot.h"
#include "kernel/testing/testing.h"
#include "shell_capture.h"

KTEST_MODULE("shell/date");

// Known epochs pin the civil-from-days conversion: origin, a leap day on a
// century year, and the last second before a non-leap century rollover.
KTEST_CASE_INTEGRATION(shell_date_formats_known_epochs) {
    KTEST_EXPECT_TRUE(run_shell("date 0") == "1970-01-01 00:00:00 UTC\n");
    KTEST_EXPECT_TRUE(run_shell("date 951782400") == "2000-02-29 00:00:00 UTC\n");
    KTEST_EXPECT_TRUE(run_shell("date 4102444799") == "2099-12-31 23:59:59 UTC\n");
    KTEST_EXPECT_TRUE(run_shell("date notanumber") == "usage: date [epoch-seconds]\n");
}

// End to end through the boot protocol: Limine supplies a date at boot, so the
// no-argument form prints a real current date rather than the fallback message.
KTEST_CASE_INTEGRATION(shell_date_reads_boot_protocol_clock) {
    KTEST_EXPECT_TRUE(kernel::boot::collect().boot_epoch_seconds > 0);
    ktl::string_view line = run_shell("date");
    KTEST_EXPECT_TRUE(line.size() == sizeof("YYYY-MM-DD HH:MM:SS UTC\n") - 1);
    // The kernel was built in the 21st century; a sane wall clock cannot predate its own code.
    KTEST_EXPECT_TRUE(line.substr(0, 2) == "20");
    KTEST_EXPECT_TRUE(line[4] == '-' && line[7] == '-' && line[10] == ' ' && line[13] == ':' && line[16] == ':');
}
