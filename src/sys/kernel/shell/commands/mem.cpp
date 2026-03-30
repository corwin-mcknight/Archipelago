#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/mm/early_heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/shell/output.h>

#include <ktl/string_view>

namespace {

void mem_handler(int argc, const char* const argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: mem info|pmm\n");
        return;
    }
    ktl::string_view sub(argv[1]);
    if (sub == "info") {
        output.print("Early heap state:\n");
        g_early_heap.debug_print_state();
    } else if (sub == "pmm") {
        output.print("Physical memory state:\n");
        kernel::mm::g_page_frame_allocator.debug_print_state();
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(mem, "mem", "Memory subsystem inspection", mem_handler);

#endif  // CONFIG_KERNEL_SHELL
