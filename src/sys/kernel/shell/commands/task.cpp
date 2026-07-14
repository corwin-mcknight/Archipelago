#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/arch.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/sched/task.h>
#include <kernel/sched/user_task.h>
#include <kernel/shell/output.h>

#include <ktl/string_view>
#include <ktl/vector>

namespace {

const char* state_name(kernel::sched::task_state state) {
    switch (state) {
        case kernel::sched::task_state::NEW: return "new";
        case kernel::sched::task_state::RUNNING: return "running";
        case kernel::sched::task_state::TERMINATED: return "terminated";
        default: return "?";
    }
}

void task_handler(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    using namespace kernel::sched;
    if (argc >= 2 && argv[1] == "demo") {
        auto created = create_user_task("udemo");
        if (created.is_err()) {
            output.print("task: create failed\n");
            return;
        }
        output.print("task: launched id={0}\n", created.unwrap()->id());
        return;
    }
    if (argc >= 2 && argv[1] != "list") {
        output.print("usage: task list|demo\n");
        return;
    }

    ktl::vector<ktl::ref<Task>> tasks;
    if (!snapshot_tasks(tasks)) {
        output.print("task: snapshot failed\n");
        return;
    }
    constexpr const char* ROW_FMT = "{0:4} {1:-16} {2:-10} {3:7} {4:7}\n";
    output.print(ROW_FMT, "ID", "NAME", "STATE", "THREADS", "FAULTS");
    for (size_t i = 0; i < tasks.size(); ++i) {
        auto& task      = tasks[i];
        // The reaper deletes the aspace during teardown; interrupts-off keeps it from
        // running between the null check and the read (single scheduling core).
        uint64_t flags  = kernel::arch::save_and_disable_interrupts();
        uint64_t faults = task->aspace() ? task->aspace()->fault_count() : 0;
        kernel::arch::restore_interrupts(flags);
        output.print(ROW_FMT, task->id(), task->name() ? task->name() : "?", state_name(task->state()),
                     task->thread_count(), faults);
    }
}

}  // namespace

KSHELL_COMMAND(task, "task", "Task debug view: list tasks, launch demo payload", task_handler);

#endif  // CONFIG_KERNEL_SHELL
