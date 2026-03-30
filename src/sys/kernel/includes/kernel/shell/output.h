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
            write(buffer.c_str());
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
    bool protocol_mode_ = false;
};

}  // namespace kernel::shell

#endif  // CONFIG_KERNEL_SHELL
