#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/obj/handle_table.h>
#include <kernel/obj/type_registry.h>
#include <kernel/sched/task.h>
#include <kernel/shell/output.h>

#include <ktl/string_view>
#include <ktl/vector>

namespace {

// Every handle in every task's table, one task at a time. Both snapshots copy under their locks
// and print after, so a slow console never holds the task list or a handle table.
void print_all_handles(kernel::shell::ShellOutput& output) {
    ktl::vector<ktl::ref<kernel::sched::Task>> tasks;
    if (!kernel::sched::snapshot_tasks(tasks)) {
        output.print("task snapshot failed: out of memory\n");
        return;
    }
    for (size_t t = 0; t < tasks.size(); t++) {
        auto& task = tasks[t];
        ktl::vector<kernel::obj::HandleInfo> handles;
        bool complete    = task->handles().snapshot(handles);
        const char* name = task->name() ? task->name() : "?";
        output.print("task {0} ({1}): {2} handle(s)\n", task->id(), name, task->handles().count());
        for (size_t i = 0; i < handles.size(); i++) {
            const auto& h              = handles[i];
            ktl::string_view type_name = "?";
            kernel::obj::g_type_registry.lookup(h.type_id).inspect(
                [&](const kernel::obj::TypeDescriptor& d) { type_name = d.name; });
            output.print("  {0:x}: {1} obj={2} rights={3:x}\n", kernel::obj::pack_handle(h.id), type_name, h.object_id,
                         h.rights);
        }
        if (!complete) { output.print("  ... snapshot incomplete: out of memory\n"); }
    }
}

void handle_handler(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: handle stats|all\n");
        return;
    }
    if (argv[1] == "stats") {
        output.print("Kernel handle table: {0} handles\n", kernel::sched::kernel_task()->handles().count());
    } else if (argv[1] == "all") {
        print_all_handles(output);
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(handle, "handle", "Handle table inspection", handle_handler);

#endif  // CONFIG_KERNEL_SHELL
