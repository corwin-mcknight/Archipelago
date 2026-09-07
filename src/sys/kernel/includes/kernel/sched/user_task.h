#pragma once

#include <kernel/sched/task.h>
#include <stddef.h>

#include <ktl/ref>
#include <ktl/result>

namespace kernel::mm {
class vmo;
class vm_aspace;
}  // namespace kernel::mm

namespace kernel::sched {

class TaskFactory;

// Internal launch mechanism: takes ownership of an already prepared address space, including on
// failure. No executable bytes or format metadata cross this interface. Callers validate the
// entry and stack and install all mappings before handing ownership over.
ktl::result<ktl::ref<Task>> start_prepared_user_task(const char* name, kernel::mm::vm_aspace* aspace, uintptr_t entry,
                                                     uintptr_t stack_pointer,
                                                     ktl::ref<kernel::obj::Channel>* parent_end_out = nullptr,
                                                     ktl::ref<TaskFactory> factory                  = {});
ktl::result<kernel::obj::HandleId> start_user_thread(ktl::ref<Task> task, uintptr_t entry, uintptr_t stack);

// Reaper-only teardown after the task's final thread has been removed.
void teardown_user_task(ktl::ref<Task> task);
// Boot-only, one-shot launch of the fixed-layout non-ELF "init" module, endowed with every boot
// module as IMAGE mail. The one task the kernel starts on a normal boot; the shell's `boot
// continue` and the integration tests drive the same function. A failed endowment is logged but
// does not unlaunch the coordinator -- it serves whatever images it received.
ktl::result<ktl::ref<Task>> launch_coordinator();

// Mail one IMAGE message (<abi/message.h>) per boot module to `task`'s mailbox: a read-only wired
// VMO over the module's bytes rides each message, with the exact byte size and the module's role
// name in the payload. This is how the creator hands a task the images it may spawn from -- boot
// modules never reach the ABI, only VMOs and names do. Every module is attempted; each failure is
// logged with its module's name, the first error is returned, and messages already mailed stay
// delivered.
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
