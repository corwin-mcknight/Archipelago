#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/sched/task.h>
#include <kernel/sched/user_task.h>
#include <kernel/shell/output.h>

#include <ktl/string_view>
#include <ktl/vector>

namespace {

// argv entries are string_views over the input line, not null-terminated -- parse by hand.
ktl::maybe<uint64_t> parse_u64(ktl::string_view sv) {
    if (sv.size() == 0) { return ktl::maybe<uint64_t>{}; }
    uint64_t value = 0;
    for (size_t i = 0; i < sv.size(); ++i) {
        if (sv[i] < '0' || sv[i] > '9') { return ktl::maybe<uint64_t>{}; }
        value = value * 10 + static_cast<uint64_t>(sv[i] - '0');
    }
    return ktl::maybe<uint64_t>{value};
}

// Queue the words after the id on a task's mailbox -- the parent's end of its bootstrap channel.
// This is the parent speaking: the message lands on the task's slot-0 endpoint like any other
// channel mail. The task decides whether it ever reads it; an undrained message dies with the task.
void task_msg(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    using namespace kernel::sched;
    auto id = parse_u64(argv[2]);
    if (!id.has_value()) {
        output.print("task: bad id\n");
        return;
    }

    ktl::vector<ktl::ref<Task>> tasks;
    if (!snapshot_tasks(tasks)) {
        output.print("task: snapshot failed\n");
        return;
    }
    ktl::ref<Task> target;
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i]->id() == *id) { target = tasks[i]; }
    }
    if (!target || !target->mailbox()) {
        output.print("task: no task {0} with a mailbox\n", *id);
        return;
    }

    size_t length = 0;
    for (int i = 3; i < argc; ++i) { length += argv[i].size() + (i > 3 ? 1 : 0); }
    auto created = kernel::obj::MessageBuffer::create(length);
    if (created.is_err()) {
        output.print("task: message too large or no memory\n");
        return;
    }
    auto message = created.unwrap();
    size_t at    = 0;
    for (int i = 3; i < argc; ++i) {
        if (i > 3) { message.data()[at++] = ' '; }
        for (size_t j = 0; j < argv[i].size(); ++j) { message.data()[at++] = static_cast<uint8_t>(argv[i][j]); }
    }

    auto sent = target->mailbox()->write(ktl::move(message));
    if (sent.is_err()) {
        output.print("task: mailbox full or closed\n");
        return;
    }
    output.print("task: queued {0} bytes for task {1}\n", length, *id);
}

const char* state_name(kernel::sched::task_state state) {
    switch (state) {
        case kernel::sched::task_state::NEW: return "new";
        case kernel::sched::task_state::RUNNING: return "running";
        case kernel::sched::task_state::TERMINATED: return "terminated";
        default: return "?";
    }
}

// Launch the echo server and init wired together: one channel pair, one end in each program's
// bootstrap message. The pairing is pure composition at this call site -- the kernel spawn path
// knows nothing about pairs. Spawn order does not matter (bootstrap messages queue), and a failed
// second spawn needs no rollback: the survivor's endpoint just reports PEER_CLOSED, the hangup a
// server must handle anyway.
void task_pair(kernel::shell::ShellOutput& output) {
    using namespace kernel::sched;
    const auto* init_mod = kernel::boot::find_module("init");
    const auto* echo_mod = kernel::boot::find_module("echo");
    if (init_mod == nullptr || echo_mod == nullptr) {
        output.print("task: boot image lacks 'init' or 'echo' module\n");
        return;
    }

    auto pair = kernel::obj::Channel::create();
    if (pair.is_err()) {
        output.print("task: channel create failed\n");
        return;
    }
    auto ends                    = pair.unwrap();

    bootstrap_extra echo_extra[] = {{ends.first, kernel::obj::Channel::DEFAULT_RIGHTS}};
    auto server                  = create_user_task("echo", echo_mod->data, echo_mod->size, echo_extra);
    if (server.is_err()) {
        output.print("task: echo create failed\n");
        return;
    }

    bootstrap_extra init_extra[] = {{ends.second, kernel::obj::Channel::DEFAULT_RIGHTS}};
    auto client                  = create_user_task("init", init_mod->data, init_mod->size, init_extra);
    if (client.is_err()) {
        output.print("task: init create failed (echo id={0} will see hangup)\n", server.unwrap()->id());
        return;
    }
    output.print("task: launched echo id={0}, init id={1}\n", server.unwrap()->id(), client.unwrap()->id());
}

void task_handler(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    using namespace kernel::sched;
    if (argc >= 2 && argv[1] == "pair") {
        task_pair(output);
        return;
    }
    if (argc >= 2 && argv[1] == "demo") {
        // Missing module and unloadable image are distinct failures: one means the boot image is
        // wrong, the other means the binary is. The loader logs which rejection it was.
        const auto* module = kernel::boot::find_module("init");
        if (module == nullptr) {
            output.print("task: no 'init' module in the boot image\n");
            return;
        }
        auto created = create_user_task("udemo", module->data, module->size);
        if (created.is_err()) {
            output.print("task: create failed\n");
            return;
        }
        output.print("task: launched id={0}\n", created.unwrap()->id());
        return;
    }
    if (argc >= 4 && argv[1] == "msg") {
        task_msg(argc, argv, output);
        return;
    }
    if (argc >= 2 && argv[1] != "list") {
        output.print("usage: task list|demo|pair|msg <id> <text>\n");
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

KSHELL_COMMAND(task, "task", "Task debug view: list tasks, launch demo payload, mail a task", task_handler);

#endif  // CONFIG_KERNEL_SHELL
