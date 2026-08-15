#pragma once

#include <kernel/config.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/shell/output.h>
#include <stdint.h>

#include <ktl/maybe>
#include <ktl/string_view>

namespace kernel::shell {

// Decimal parse for command arguments; argv entries are string_views over the
// input line, not null-terminated, so parse by hand.
inline ktl::maybe<uint64_t> parse_u64(ktl::string_view sv) {
    if (sv.size() == 0) { return ktl::nothing; }
    uint64_t v = 0;
    for (size_t i = 0; i < sv.size(); ++i) {
        if (sv[i] < '0' || sv[i] > '9') { return ktl::nothing; }
        v = v * 10 + static_cast<uint64_t>(sv[i] - '0');
    }
    return v;
}

using shell_handler_fn = void (*)(int argc, const ktl::string_view argv[], ShellOutput& output);

struct shell_command {
    const char* name;
    const char* description;
    shell_handler_fn handler;
};

void shell_main();
// Tokenizes and dispatches one command line against the registry; the
// interactive loop and tests share this path.
void run_line(const char* line, ShellOutput& output);
ShellOutput& shell_output();
// End the interactive loop after the current command; the shell thread then exits.
void request_exit();

}  // namespace kernel::shell

#if defined(__GNUC__)
#define KSHELL_CMD_SEC __attribute__((section(".kshell_cmds"), used))
#else
#define KSHELL_CMD_SEC
#endif

#define KSHELL_COMMAND(name_sym, name_str, desc_str, handler_fn) \
    static kernel::shell::shell_command _kshell_cmd_##name_sym KSHELL_CMD_SEC = {name_str, desc_str, handler_fn}

#endif  // CONFIG_KERNEL_SHELL
