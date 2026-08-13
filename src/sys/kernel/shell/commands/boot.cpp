#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/boot.h>

#include <ktl/string_view>

namespace {

void boot_handler(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: boot continue|shell\n");
        return;
    }

    // Both subcommands resume the boot sequence; they differ in what happens to the shell.
    // `continue` hands the machine to userspace and exits the shell; `shell` keeps the prompt
    // live alongside the booted system.
    if (argv[1] == "continue" || argv[1] == "shell") {
        if (kernel::boot::continue_boot()) {
            output.print("boot: continuing\n");
        } else {
            output.print("boot: already continued\n");
        }
        if (argv[1] == "continue") { kernel::shell::request_exit(); }
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(boot, "boot", "Boot flow control", boot_handler);

#endif  // CONFIG_KERNEL_SHELL
