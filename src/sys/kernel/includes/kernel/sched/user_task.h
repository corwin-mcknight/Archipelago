#pragma once

#include <kernel/sched/task.h>
#include <stddef.h>

#include <ktl/ref>
#include <ktl/result>
#include <ktl/span>

namespace kernel::mm { class vmo; }

namespace kernel::sched {

// A handle the creator endows a new task with, appended to its bootstrap message after the two
// self-handles (<abi/syscall.h>). This is how a task is wired to anything beyond itself -- a
// channel to a peer today, a channel to a service later.
struct bootstrap_extra {
    ktl::ref<kernel::obj::Object> object;
    kernel::obj::Rights rights;
};

// Create and queue a user task running the ELF image in [elf, elf + elf_size). The image is a plain
// byte span rather than something this layer looks up itself, so where it came from -- a boot
// module today, a filesystem later -- stays the caller's business. At most two extras fit: the
// bootstrap message's two leading handles are the self-handles and a message carries four.
//
// The bootstrap channel's parent end goes to exactly one owner, decided before the child can run:
// null parent_end_out leaves it in Task::mailbox (the kernel-as-parent arrangement), non-null
// receives it instead -- the spawn path, where the calling task is the parent and the kernel
// keeps only its owner-of-last-resort task handle.
ktl::result<ktl::ref<Task>> create_user_task(const char* name, const void* elf, size_t elf_size,
                                             ktl::span<const bootstrap_extra> extras        = {},
                                             ktl::ref<kernel::obj::Channel>* parent_end_out = nullptr);
// Reaper-only teardown after the task's final thread has been removed.
void teardown_user_task(ktl::ref<Task> task);
// What SYS_TASK_SPAWN hands the caller: both handles live in the caller's table. The caller is
// the child's parent -- it holds the task handle (kill, status, TERMINATED) and the parent end of
// the child's bootstrap channel (all further endowment and mail).
struct spawn_handles {
    kernel::obj::HandleId task;
    kernel::obj::HandleId mailbox;
};

// Spawn a task from an executable image VMO into `caller`'s handle table; the syscall layer's
// buffer-free core, so kernel-context tests can drive it directly. The image must be physically
// contiguous (every kernel-minted image VMO is); its debug name names the task. A failure after
// the child is already running kills it rather than leaking it.
ktl::result<spawn_handles> task_spawn(Task& caller, ktl::ref<kernel::mm::vmo> image);

// Launch the coordinator: the boot module named "init", kernel-parented, endowed with every boot
// module as IMAGE mail. The one task the kernel starts on a normal boot; the shell's `boot
// continue` and the integration tests drive the same function. A failed endowment is logged but
// does not unlaunch the coordinator -- it serves whatever images it received.
ktl::result<ktl::ref<Task>> launch_coordinator();

// Mail one IMAGE message (<abi/message.h>) per boot module to `task`'s mailbox: a read-only wired
// VMO over the module's bytes rides each message, with the exact byte size and the module's role
// name in the payload. This is how the creator hands a task the images it may spawn from -- boot
// modules never reach the ABI, only VMOs and names do. Stops at the first failure; messages
// already mailed stay delivered.
ktl::result<void> endow_boot_modules(const ktl::ref<Task>& task);
// Kill every thread of `task`: mark each, wake the blocked ones, and let each exit at its next
// kernel boundary. Asynchronous -- returns once every thread is marked and unblocked, not once the
// task is torn down; wait for SIGNAL_TERMINATED for that. A no-op on an already-terminated task;
// refused for task zero. Defined in user_task.cpp for kernel builds, stubbed by the host runner
// (which schedules nothing).
ktl::result<void> task_kill(const ktl::ref<Task>& task);
// Leave a user-mode fault after the trap handler has logged it and left fault context. This marks
// the current thread dead and hands it to the reaper; the reaper publishes TERMINATED only after
// the task's handles and address space are gone.
[[noreturn]] void terminate_current_user_task_from_fault(uint64_t cause, uint64_t detail, uintptr_t pc);

}  // namespace kernel::sched
