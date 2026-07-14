#pragma once

#include <kernel/sched/task.h>

#include <ktl/ref>
#include <ktl/result>

namespace kernel::sched {

// Create and queue a user task running the architecture's embedded validation payload.
ktl::result<ktl::ref<Task>> create_user_task(const char* name);
// Reaper-only teardown after the task's final thread has been removed.
void teardown_user_task(ktl::ref<Task> task);

}  // namespace kernel::sched
