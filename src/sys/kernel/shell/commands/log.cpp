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

void log_handler(int argc, const char* const argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: log show\n");
        return;
    }
    ktl::string_view sub(argv[1]);
    if (sub == "show") {
        g_log.for_each(0, [&output](const kernel::log_message* msg) {
            output.print("[{0}] {1}\n", level_name(msg->level()), msg->text.c_str());
        });
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(log, "log", "Log inspection and control", log_handler);

#endif  // CONFIG_KERNEL_SHELL
