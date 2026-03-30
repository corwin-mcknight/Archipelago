#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <ktl/string_view>

namespace {

void harness_handler(int argc, const char* const argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: harness enable|disable\n");
        return;
    }

    ktl::string_view sub(argv[1]);
    if (sub == "enable") {
        output.set_protocol_mode(true);
    } else if (sub == "disable") {
        output.set_protocol_mode(false);
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(harness, "harness", "Test harness protocol control", harness_handler);

#endif  // CONFIG_KERNEL_SHELL
