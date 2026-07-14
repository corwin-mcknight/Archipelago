#include <kernel/crash.h>
#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/log.h>

#include <ktl/string_view>

namespace {

void harness_handler(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: harness enable|disable\n");
        return;
    }

    if (argv[1] == "enable") {
        output.set_protocol_mode(true);
        kernel::crash::set_harness_enabled(true);
        // Log flushes write ANSI color straight to the UART, out of band from the shell's
        // own SGR suppression; force them plain so they cannot splice color into the
        // byte-clean harness JSON stream.
        g_log.set_colors(false);
    } else if (argv[1] == "disable") {
        output.set_protocol_mode(false);
        kernel::crash::set_harness_enabled(false);
        g_log.set_colors(CONFIG_KERNEL_LOG_COLORS);
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(harness, "harness", "Test harness protocol control", harness_handler);

#endif  // CONFIG_KERNEL_SHELL
