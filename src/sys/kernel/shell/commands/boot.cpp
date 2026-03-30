#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <ktl/string_view>

namespace {

void boot_handler(int argc, const char* const argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: boot continue\n");
        return;
    }

    ktl::string_view sub(argv[1]);
    if (sub == "continue") {
        kernel::shell::request_boot_continue();
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(boot, "boot", "Boot flow control", boot_handler);

#endif  // CONFIG_KERNEL_SHELL
