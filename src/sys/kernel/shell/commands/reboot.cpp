#include <kernel/config.h>

#if CONFIG_KERNEL_SHELL

#include <ktl/string_view>

#include "kernel/platform.h"
#include "kernel/shell/shell.h"

namespace {

void reboot_handler(int, const ktl::string_view[], kernel::shell::ShellOutput& output) {
    output.print("rebooting...\n");
    kernel::platform::reboot();
    output.print("reboot: this board has no reset path\n");
}

}  // namespace

KSHELL_COMMAND(reboot, "reboot", "Reset the machine", reboot_handler);

#endif  // CONFIG_KERNEL_SHELL
