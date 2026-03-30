#include <kernel/shell/output.h>

#if CONFIG_KERNEL_SHELL

extern kernel::driver::uart uart;

namespace kernel::shell {

void ShellOutput::write(const char* s) { uart.write_string(s); }

void ShellOutput::write_char(char c) { uart.write_byte(c); }

}  // namespace kernel::shell

#endif  // CONFIG_KERNEL_SHELL
