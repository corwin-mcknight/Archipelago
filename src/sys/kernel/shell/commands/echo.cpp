#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <ktl/string_view>

namespace {

void echo_handler(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    for (int i = 1; i < argc; ++i) { output.print(i + 1 < argc ? "{0} " : "{0}", argv[i]); }
    output.write("\n");
}

}  // namespace

KSHELL_COMMAND(echo, "echo", "Print arguments back to the console", echo_handler);

#endif  // CONFIG_KERNEL_SHELL
