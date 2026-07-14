#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/drivers/uart.h>
#include <kernel/sched/scheduler.h>

#include <ktl/algorithm>
#include <ktl/maybe>
#include <ktl/string_view>

extern kernel::driver::uart uart;

extern "C" kernel::shell::shell_command __start__kshell_cmds[], __stop__kshell_cmds[];

namespace {

constexpr size_t kCommandBufferSize = 256;
constexpr size_t kMaxArgs           = 16;

kernel::shell::ShellOutput g_output;
bool g_boot_continue = false;

void read_line(char* buffer, size_t buffer_size) {
    size_t idx = 0;
    while (idx < buffer_size - 1) {
        // Sleep-poll instead of busy-waiting in uart.read() so idle absorbs the wait between
        // keystrokes; buffered bytes drain back-to-back because received_data() stays set.
        // QEMU's chardev backpressure holds input while we sleep, so a 1 ms gap loses nothing.
        while (uart.received_data() == 0) { kernel::sched::sleep_ticks(1); }
        char c = uart.read();
        if (c == '\r' || c == '\n') {
            g_output.write("\r\n");
            break;
        }
        if (c == '\b' || c == 127) {
            if (idx > 0) {
                --idx;
                g_output.write("\b \b");
            }
            continue;
        }
        buffer[idx++] = c;
        g_output.write_char(c);
    }
    buffer[idx] = '\0';
}

int tokenize(const char* buffer, ktl::string_view argv[], size_t max_args) {
    int argc      = 0;
    const char* p = buffer;
    while (*p && static_cast<size_t>(argc) < max_args) {
        while (*p == ' ' || *p == '\t') { ++p; }
        if (*p == '\0') { break; }
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t') { ++p; }
        argv[argc++] = ktl::string_view(start, static_cast<size_t>(p - start));
    }
    return argc;
}

ktl::maybe<kernel::shell::shell_command&> find_command(ktl::string_view name) {
    return ktl::find_if(__start__kshell_cmds, __stop__kshell_cmds,
                        [&](const kernel::shell::shell_command& cmd) { return name == cmd.name; });
}

void dispatch(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    if (argc == 0) { return; }

    if (argv[0] == "help") {
        output.print("Available commands:\n");
        output.print("  help -- show this message\n");
        for (auto* cmd = __start__kshell_cmds; cmd != __stop__kshell_cmds; ++cmd) {
            output.print("  {0} -- {1}\n", cmd->name, cmd->description);
        }
        return;
    }

    auto cmd = find_command(argv[0]);
    if (!cmd) {
        output.print("unknown command: {0}\n", argv[0]);
        return;
    }

    cmd->handler(argc, argv, output);
}

}  // namespace

namespace kernel::shell {

void shell_main() {
    char buffer[kCommandBufferSize];
    ktl::string_view argv[kMaxArgs];

    while (!g_boot_continue) {
        if (g_output.protocol_mode()) {
            g_output.event("{{\"event\":\"ready\",\"protocol\":2}}");
        } else {
            g_output.write("% ");
        }

        read_line(buffer, sizeof(buffer));
        int argc = tokenize(buffer, argv, kMaxArgs);
        dispatch(argc, argv, g_output);
    }
}

void run_line(const char* line, ShellOutput& output) {
    ktl::string_view argv[kMaxArgs];
    int argc = tokenize(line, argv, kMaxArgs);
    dispatch(argc, argv, output);
}

ShellOutput& shell_output() { return g_output; }

void request_boot_continue() { g_boot_continue = true; }

}  // namespace kernel::shell

#endif  // CONFIG_KERNEL_SHELL
