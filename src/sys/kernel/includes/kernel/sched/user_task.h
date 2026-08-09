#pragma once

#include <kernel/sched/task.h>
#include <stddef.h>

#include <ktl/ref>
#include <ktl/result>
#include <ktl/span>

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
ktl::result<ktl::ref<Task>> create_user_task(const char* name, const void* elf, size_t elf_size,
                                             ktl::span<const bootstrap_extra> extras = {});
// Reaper-only teardown after the task's final thread has been removed.
void teardown_user_task(ktl::ref<Task> task);
// Leave a user-mode fault after the trap handler has logged it and left fault context. This marks
// the current thread dead and hands it to the reaper; the reaper publishes TERMINATED only after
// the task's handles and address space are gone.
[[noreturn]] void terminate_current_user_task_from_fault(uint64_t cause, uint64_t detail, uintptr_t pc);

}  // namespace kernel::sched
