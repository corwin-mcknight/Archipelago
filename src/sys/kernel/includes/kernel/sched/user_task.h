#pragma once

#include <kernel/sched/task.h>
#include <stddef.h>

#include <ktl/ref>
#include <ktl/result>

namespace kernel::sched {

// Create and queue a user task running the ELF image in [elf, elf + elf_size). The image is a plain
// byte span rather than something this layer looks up itself, so where it came from -- a boot
// module today, a filesystem later -- stays the caller's business.
ktl::result<ktl::ref<Task>> create_user_task(const char* name, const void* elf, size_t elf_size);
// Reaper-only teardown after the task's final thread has been removed.
void teardown_user_task(ktl::ref<Task> task);
// Leave a user-mode fault after the trap handler has logged it and left fault context. This marks
// the current thread dead and hands it to the reaper; the reaper publishes TERMINATED only after
// the task's handles and address space are gone.
[[noreturn]] void terminate_current_user_task_from_fault(uint64_t cause, uint64_t detail, uintptr_t pc);

}  // namespace kernel::sched
