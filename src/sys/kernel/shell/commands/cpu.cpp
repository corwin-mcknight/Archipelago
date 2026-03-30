#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/config.h>
#include <kernel/shell/output.h>
#include <kernel/time.h>

#include <ktl/string_view>

namespace {

void cpu_handler(int argc, const char* const argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: cpu info\n");
        return;
    }
    ktl::string_view sub(argv[1]);
    if (sub == "info") {
        auto uptime_ns = kernel::time::ns_since_boot();
        auto uptime_ms = uptime_ns / 1000000;
        output.print("Uptime: {0} ms\n", uptime_ms);
        output.print("Max cores: {0}\n", CONFIG_MAX_CORES);
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(cpu, "cpu", "Processor state inspection", cpu_handler);

#endif  // CONFIG_KERNEL_SHELL
