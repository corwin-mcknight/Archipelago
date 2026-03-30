#pragma once

#include <kernel/config.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/shell/output.h>

namespace kernel::shell {

using shell_handler_fn = void (*)(int argc, const char* const argv[], ShellOutput& output);

struct shell_command {
    const char* name;
    const char* description;
    shell_handler_fn handler;
};

void shell_main();
ShellOutput& shell_output();
void request_boot_continue();

}  // namespace kernel::shell

#if defined(__GNUC__)
#define KSHELL_CMD_SEC __attribute__((section(".kshell_cmds"), used))
#else
#define KSHELL_CMD_SEC
#endif

#define KSHELL_COMMAND(name_sym, name_str, desc_str, handler_fn) \
    static kernel::shell::shell_command _kshell_cmd_##name_sym KSHELL_CMD_SEC = {name_str, desc_str, handler_fn}

#endif  // CONFIG_KERNEL_SHELL
