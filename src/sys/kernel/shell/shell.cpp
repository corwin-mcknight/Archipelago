#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/drivers/uart.h>
#include <kernel/sched/scheduler.h>

#include <ktl/algorithm>
#include <ktl/maybe>
#include <ktl/string_view>
#include <ktl/utf8>

extern kernel::driver::uart uart;

extern "C" kernel::shell::shell_command __start__kshell_cmds[], __stop__kshell_cmds[];

namespace {

constexpr size_t kCommandBufferSize = 256;
constexpr size_t kMaxArgs           = 16;
constexpr size_t kHistoryDepth      = 8;

kernel::shell::ShellOutput g_output;
bool g_exit_requested = false;

// Ring of previously accepted lines; g_history_count only grows, so entry i lives at i % depth.
char g_history[kHistoryDepth][kCommandBufferSize];
size_t g_history_count = 0;

char read_char() {
    // Sleep-poll instead of busy-waiting in uart.read() so idle absorbs the wait between
    // keystrokes; buffered bytes drain back-to-back because received_data() stays set.
    // QEMU's chardev backpressure holds input while we sleep, so a 1 ms gap loses nothing.
    while (uart.received_data() == 0) { kernel::sched::sleep_ticks(1); }
    return uart.read();
}

// Erases the visible line, then loads src (empty when nullptr) into buffer and echoes it.
void replace_line(char* buffer, size_t& idx, const char* src) {
    while (idx > 0) {
        --idx;
        // One erase per codepoint, not per byte; the terminal shows one cell.
        if (!ktl::utf8_is_continuation(buffer[idx])) { g_output.write("\b \b"); }
    }
    if (src != nullptr) {
        for (; src[idx] != '\0'; ++idx) { buffer[idx] = src[idx]; }
    }
    buffer[idx] = '\0';
    g_output.write(buffer);
}

void read_line(char* buffer, size_t buffer_size) {
    size_t idx    = 0;
    size_t oldest = g_history_count > kHistoryDepth ? g_history_count - kHistoryDepth : 0;
    size_t browse = g_history_count;  // one past the newest entry = the line being typed
    for (;;) {
        char c = read_char();
        if (c == '\r' || c == '\n') {
            g_output.write("\r\n");
            break;
        }
        if (c == '\b' || c == 127) {
            if (idx > 0) {
                // Remove the whole trailing codepoint: continuation bytes plus its lead.
                --idx;
                while (idx > 0 && ktl::utf8_is_continuation(buffer[idx])) { --idx; }
                g_output.write("\b \b");
            }
            continue;
        }
        if (c == 0x1b) {
            // Consume the whole CSI/SS3 sequence so its bytes never reach the buffer; only
            // up/down arrows act, everything else is swallowed.
            char kind = read_char();
            if (kind != '[' && kind != 'O') { continue; }
            char fin = read_char();
            if (kind == '[') {
                while (fin < 0x40 || fin > 0x7e) { fin = read_char(); }
            }
            if (fin == 'A' && browse > oldest) {
                --browse;
                replace_line(buffer, idx, g_history[browse % kHistoryDepth]);
            } else if (fin == 'B' && browse < g_history_count) {
                ++browse;
                replace_line(buffer, idx, browse == g_history_count ? nullptr : g_history[browse % kHistoryDepth]);
            }
            continue;
        }
        // Drop other control bytes; UTF-8 (>= 0x80) passes through opaquely. On a full
        // buffer keep draining to the newline so an overlong line's tail cannot execute
        // as a second command.
        if (static_cast<unsigned char>(c) < 0x20 || idx >= buffer_size - 1) { continue; }
        buffer[idx++] = c;
        g_output.write_char(c);
    }
    // A full buffer may have cut a multibyte sequence mid-codepoint.
    idx         = ktl::utf8_trim_partial(buffer, idx);
    buffer[idx] = '\0';
    if (idx == 0) { return; }
    const char* last = g_history_count > 0 ? g_history[(g_history_count - 1) % kHistoryDepth] : nullptr;
    if (last != nullptr && ktl::string_view(buffer) == last) { return; }  // skip consecutive duplicates
    char* dst = g_history[g_history_count % kHistoryDepth];
    for (size_t i = 0; i <= idx; ++i) { dst[i] = buffer[i]; }
    ++g_history_count;
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

    while (!g_exit_requested) {
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

void request_exit() { g_exit_requested = true; }

}  // namespace kernel::shell

#endif  // CONFIG_KERNEL_SHELL
