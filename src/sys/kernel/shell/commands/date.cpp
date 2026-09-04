#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/boot.h>
#include <kernel/time.h>

#include <ktl/string_view>

namespace {

// Prints the current UTC date and time.
void date_handler(int, const ktl::string_view[], kernel::shell::ShellOutput& output) {
    int64_t boot_epoch = kernel::boot::collect().boot_epoch_seconds;
    if (boot_epoch <= 0) {
        output.write("date: boot protocol supplied no wall clock\n");
        return;
    }
    uint64_t epoch =
        static_cast<uint64_t>(boot_epoch) + static_cast<uint64_t>(kernel::time::ns_since_boot()) / 1'000'000'000u;

    uint64_t days  = epoch / 86400;
    uint64_t sod   = epoch % 86400;
    // Civil-from-days after Howard Hinnant's date algorithms; exact for every
    // non-negative epoch (era arithmetic simplified since days >= 0 here).
    uint64_t z     = days + 719468;
    uint64_t era   = z / 146097;
    uint64_t doe   = z % 146097;
    uint64_t yoe   = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint64_t doy   = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp    = (5 * doy + 2) / 153;
    uint64_t day   = doy - (153 * mp + 2) / 5 + 1;
    uint64_t month = mp < 10 ? mp + 3 : mp - 9;
    uint64_t year  = yoe + era * 400 + (month <= 2 ? 1 : 0);

    output.print("{0:04}-{1:02}-{2:02} {3:02}:{4:02}:{5:02} UTC\n", year, month, day, sod / 3600, (sod / 60) % 60,
                 sod % 60);
}

}  // namespace

KSHELL_COMMAND(date, "date", "Print the current UTC date and time", date_handler);

#endif  // CONFIG_KERNEL_SHELL
