#pragma once

#include <kernel/config.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/arch.h>
#include <kernel/drivers/uart.h>

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
        if (protocol_mode_) {
            // Protocol lines must reach the wire unspliced: a log write from interrupt context
            // (or from another thread after a preemption) landing mid-line corrupts the harness
            // JSON stream. Interrupts stay off for the whole line; protocol mode only runs under
            // QEMU, whose virtual UART drains fast enough that the window is microseconds.
            uint64_t flags = kernel::arch::save_and_disable_interrupts();
            write("@@HARNESS {\"event\":\"result\",\"text\":\"");
            write_json_escaped(buffer.c_str());
            write("\"}\n");
            kernel::arch::restore_interrupts(flags);
        } else {
            write(buffer.c_str());
        }
    }

    template <typename... Args> void event(const char* fmt, const Args&... args) {
        ktl::fixed_string<512> buffer;
        ktl::format::format_to_buffer_raw(buffer.m_buffer, sizeof(buffer.m_buffer), fmt, args...);
        if (protocol_mode_) {
            // Same anti-splice guard as print(); see the comment there.
            uint64_t flags = kernel::arch::save_and_disable_interrupts();
            write("@@HARNESS ");
            write(buffer.c_str());
            write("\n");
            kernel::arch::restore_interrupts(flags);
        }
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

    // Writes a complete, pre-formatted harness protocol line with interrupts disabled so log
    // writes from interrupt context (or another thread after a preemption) cannot splice into
    // it; see print() for the full rationale.
    void write_atomic(const char* s) {
        uint64_t flags = kernel::arch::save_and_disable_interrupts();
        write(s);
        kernel::arch::restore_interrupts(flags);
    }

   private:
    // Writes the string while escaping JSON-special characters so the result is safe to embed
    // inside a JSON string literal: double-quote, backslash, and control characters below 0x20.
    void write_json_escaped(const char* s) {
        static constexpr char kHex[] = "0123456789abcdef";
        for (const char* p = s; *p != '\0'; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            switch (c) {
                case '"':
                    write_char('\\');
                    write_char('"');
                    break;
                case '\\':
                    write_char('\\');
                    write_char('\\');
                    break;
                case '\n':
                    write_char('\\');
                    write_char('n');
                    break;
                case '\r':
                    write_char('\\');
                    write_char('r');
                    break;
                case '\t':
                    write_char('\\');
                    write_char('t');
                    break;
                default:
                    if (c < 0x20) {
                        write_char('\\');
                        write_char('u');
                        write_char('0');
                        write_char('0');
                        write_char(kHex[(c >> 4) & 0xf]);
                        write_char(kHex[c & 0xf]);
                    } else {
                        write_char(static_cast<char>(c));
                    }
                    break;
            }
        }
    }

    bool protocol_mode_ = false;
    sink_fn sink_       = nullptr;
    void* sink_ctx_     = nullptr;
};

}  // namespace kernel::shell

#endif  // CONFIG_KERNEL_SHELL
