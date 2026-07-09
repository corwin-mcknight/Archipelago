#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/obj/handle_table.h>
#include <kernel/sched/task.h>
#include <kernel/shell/output.h>

#include <ktl/string_view>

namespace {

void handle_handler(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: handle stats\n");
        return;
    }
    if (argv[1] == "stats") {
        output.print("Kernel handle table: {0} handles\n", kernel::sched::kernel_task()->handles().count());
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(handle, "handle", "Handle table inspection", handle_handler);

#endif  // CONFIG_KERNEL_SHELL
