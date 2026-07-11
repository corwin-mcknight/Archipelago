#include <kernel/shell/output.h>

#if CONFIG_KERNEL_SHELL

extern kernel::driver::uart uart;

namespace kernel::shell {

void ShellOutput::write(const char* s) {
    if (sink_ != nullptr) {
        for (const char* p = s; *p != '\0'; ++p) { sink_(*p, sink_ctx_); }
        return;
    }
    uart.write_string(s);
}

void ShellOutput::write_char(char c) {
    if (sink_ != nullptr) {
        sink_(c, sink_ctx_);
        return;
    }
    uart.write_byte(c);
}

}  // namespace kernel::shell

#endif  // CONFIG_KERNEL_SHELL
