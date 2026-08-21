#pragma once

#include <kernel/config.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/arch.h>
#include <kernel/console.h>
#include <kernel/drivers/uart.h>
#include <kernel/json_escape.h>

#include <ktl/fixed_string>
#include <ktl/fmt>

extern kernel::driver::uart uart;

namespace kernel::shell {

class ShellOutput {
   public:
    bool protocol_mode() const { return protocol_mode_; }
    void set_protocol_mode(bool enabled) { protocol_mode_ = enabled; }

    template <typename... Args> void print(const char* fmt, const Args&... args) {
        ktl::fixed_string<512> buffer;
        ktl::format::format_to_buffer_raw(buffer.m_buffer, sizeof(buffer.m_buffer), fmt, args...);
        // Lines must reach the wire unspliced: a log write from interrupt context or from another
        // core landing mid-line corrupts the harness JSON stream.
        kernel::console::line_guard line;
        if (protocol_mode_) {
            write("@@HARNESS {\"event\":\"result\",\"text\":\"");
            write_json_escaped(buffer.c_str());
            write("\"}\n");
        } else {
            write(buffer.c_str());
        }
    }

    // Formats one @@HARNESS protocol line and writes it regardless of protocol mode: the prefix
    // is what test tooling scrapes, and interactive test runs still want to see progress.
    template <typename... Args> void event(const char* fmt, const Args&... args) {
        ktl::fixed_string<512> buffer;
        ktl::format::format_to_buffer_raw(buffer.m_buffer, sizeof(buffer.m_buffer), fmt, args...);
        kernel::console::line_guard line;
        write("@@HARNESS ");
        write(buffer.c_str());
        write("\n");
    }

    void write(const char* s);
    void write_char(char c);

    // Redirects all output to fn (tests capture bytes instead of driving the
    // UART). Pass nullptr to restore UART output.
    using sink_fn = void (*)(char c, void* ctx);
    void set_sink(sink_fn fn, void* ctx) {
        sink_     = fn;
        sink_ctx_ = ctx;
    }

    // Emits an SGR escape (color/style). Suppressed in protocol mode so
    // harness JSON stays byte-clean.
    void sgr(const char* code) {
        if (protocol_mode_) { return; }
        write_char('\x1b');
        write_char('[');
        write(code);
        write_char('m');
    }
    void reset_style() { sgr("0"); }

   private:
    void write_json_escaped(const char* s) {
        kernel::write_json_escaped([this](char c) { write_char(c); }, s);
    }

    bool protocol_mode_ = false;
    sink_fn sink_       = nullptr;
    void* sink_ctx_     = nullptr;
};

}  // namespace kernel::shell

#endif  // CONFIG_KERNEL_SHELL
