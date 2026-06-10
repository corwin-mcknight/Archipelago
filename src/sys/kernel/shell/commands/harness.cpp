#include <kernel/crash.h>
#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

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
    } else if (argv[1] == "disable") {
        output.set_protocol_mode(false);
        kernel::crash::set_harness_enabled(false);
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(harness, "harness", "Test harness protocol control", harness_handler);

#endif  // CONFIG_KERNEL_SHELL
