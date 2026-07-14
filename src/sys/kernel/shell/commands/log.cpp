#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/log.h>
#include <kernel/shell/output.h>

#include <ktl/string_view>

namespace {

const char* level_name(kernel::log_level level) {
    switch (level) {
        case kernel::log_level::trace: return "trace";
        case kernel::log_level::debug: return "debug";
        case kernel::log_level::info: return "info";
        case kernel::log_level::warn: return "warn";
        case kernel::log_level::error: return "error";
        case kernel::log_level::fatal: return "fatal";
        default: return "unknown";
    }
}

void log_handler(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: log show | log color [on|off]\n");
        return;
    }
    if (argv[1] == "show") {
        g_log.for_each(0, [&output](const kernel::log_message* msg) {
            output.print("[{0}] {1}\n", level_name(msg->level()), msg->text.c_str());
        });
    } else if (argv[1] == "color") {
        if (argc < 3) {
            output.print("color: {0}\n", g_log.colors() ? "on" : "off");
        } else if (argv[2] == "on") {
            g_log.set_colors(true);
        } else if (argv[2] == "off") {
            g_log.set_colors(false);
        } else {
            output.print("usage: log color [on|off]\n");
        }
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(log, "log", "Log inspection and control", log_handler);

#endif  // CONFIG_KERNEL_SHELL
