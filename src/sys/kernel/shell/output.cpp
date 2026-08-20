#include <kernel/console.h>
#include <kernel/shell/output.h>

#if CONFIG_KERNEL_SHELL

namespace kernel::shell {

void ShellOutput::write(const char* s) {
    if (sink_ != nullptr) {
        for (const char* p = s; *p != '\0'; ++p) { sink_(*p, sink_ctx_); }
        return;
    }
    kernel::console::write_string(s);
}

void ShellOutput::write_char(char c) {
    if (sink_ != nullptr) {
        sink_(c, sink_ctx_);
        return;
    }
    kernel::console::write_byte(c);
}

}  // namespace kernel::shell

#endif  // CONFIG_KERNEL_SHELL
