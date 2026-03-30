#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/obj/handle_table.h>
#include <kernel/shell/output.h>

#include <ktl/string_view>

namespace {

void handle_handler(int argc, const char* const argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: handle stats\n");
        return;
    }
    ktl::string_view sub(argv[1]);
    if (sub == "stats") {
        output.print("Kernel handle table: {0} handles\n", kernel::obj::g_handle_table.count());
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(handle, "handle", "Handle table inspection", handle_handler);

#endif  // CONFIG_KERNEL_SHELL
