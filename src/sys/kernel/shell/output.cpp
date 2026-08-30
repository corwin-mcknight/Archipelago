#include <kernel/console.h>
#include <kernel/shell/output.h>

#if CONFIG_KERNEL_SHELL

namespace kernel::shell {

void ShellOutput::write(const char* s) {
    auto line = kernel::g_console.lock_line();
    if (sink_ != nullptr) {
        for (const char* p = s; *p != '\0'; ++p) { sink_(*p, sink_ctx_); }
        return;
    }
    kernel::g_console.write(s);
}

void ShellOutput::write_char(char c) {
    auto line = kernel::g_console.lock_line();
    if (sink_ != nullptr) {
        sink_(c, sink_ctx_);
        return;
    }
    kernel::g_console.write(c);
}

}  // namespace kernel::shell

#endif  // CONFIG_KERNEL_SHELL
