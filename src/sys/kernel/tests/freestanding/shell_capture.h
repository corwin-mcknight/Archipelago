#pragma once

#include <kernel/shell/shell.h>
#include <stddef.h>

#include <ktl/string_view>

// Shared capture fixture for the shell tests: run_shell() drives one command
// line through run_line with a capture sink and returns the captured bytes.

inline char g_shell_capture[32768];
inline size_t g_shell_capture_len = 0;

inline void capture_sink(char c, void*) {
    if (g_shell_capture_len < sizeof(g_shell_capture) - 1) { g_shell_capture[g_shell_capture_len++] = c; }
}

inline ktl::string_view run_shell(const char* line, bool protocol_mode = false) {
    g_shell_capture_len = 0;
    kernel::shell::ShellOutput out;
    out.set_protocol_mode(protocol_mode);
    out.set_sink(capture_sink, nullptr);
    kernel::shell::run_line(line, out);
    g_shell_capture[g_shell_capture_len] = '\0';
    return ktl::string_view(g_shell_capture, g_shell_capture_len);
}

inline bool contains(ktl::string_view haystack, ktl::string_view needle) {
    if (needle.size() > haystack.size()) { return false; }
    for (size_t i = 0; i + needle.size() <= haystack.size(); i++) {
        if (haystack.substr(i, needle.size()) == needle) { return true; }
    }
    return false;
}
