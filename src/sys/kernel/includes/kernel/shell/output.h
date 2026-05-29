#pragma once

#include <kernel/config.h>

#if CONFIG_KERNEL_SHELL

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
            write("@@HARNESS {\"event\":\"result\",\"text\":\"");
            write_json_escaped(buffer.c_str());
            write("\"}\n");
        } else {
            write(buffer.c_str());
        }
    }

    template <typename... Args> void event(const char* fmt, const Args&... args) {
        ktl::fixed_string<512> buffer;
        ktl::format::format_to_buffer_raw(buffer.m_buffer, sizeof(buffer.m_buffer), fmt, args...);
        if (protocol_mode_) {
            write("@@HARNESS ");
            write(buffer.c_str());
            write("\n");
        }
    }

    void write(const char* s);
    void write_char(char c);

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
};

}  // namespace kernel::shell

#endif  // CONFIG_KERNEL_SHELL
