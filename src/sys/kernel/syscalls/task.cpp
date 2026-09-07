#include <kernel/sched/user_task.h>

#include "internal.h"

namespace kernel::syscalls {

uint64_t sys_task_kill(obj::HandleTable& table, uint64_t handle) {
    auto task = table.get<sched::Task>(obj::unpack_handle(handle), obj::RIGHT_WRITE);
    if (task.is_err()) { return errc_of(task.unwrap_err()); }
    auto killed = sched::task_kill(task.unwrap());
    return killed.is_ok() ? 0 : errc_of(killed.unwrap_err());
}

uint64_t sys_task_status(obj::HandleTable& table, uint64_t handle) {
    auto found = table.get<sched::Task>(obj::unpack_handle(handle), obj::RIGHT_READ);
    if (found.is_err()) { return errc_of(found.unwrap_err()); }
    auto task = found.unwrap();
    if (task->state() != sched::task_state::TERMINATED) { return errc_of(ktl::errc::would_block); }
    return task->exit_code();
}

}  // namespace kernel::syscalls
