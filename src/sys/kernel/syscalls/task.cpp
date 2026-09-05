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

uint64_t sys_task_spawn(sched::Thread& self, uint64_t handle, uint64_t offset) {
    using namespace kernel::obj;
    const auto& buffer = self.ipc();
    auto output        = buffer.range(offset, 2 * sizeof(uint64_t));
    if (output.is_err()) { return errc_of(output.unwrap_err()); }

    auto task  = self.owner();
    auto found = task->handles().get<kernel::mm::vmo>(unpack_handle(handle), RIGHT_READ);
    if (found.is_err()) { return errc_of(found.unwrap_err()); }
    auto image   = found.unwrap();

    auto spawned = kernel::sched::task_spawn(*task, ktl::move(image));
    if (spawned.is_err()) { return errc_of(spawned.unwrap_err()); }

    uint64_t handles[2] = {pack_handle(spawned.unwrap().task), pack_handle(spawned.unwrap().mailbox)};
    output.unwrap().write(handles, sizeof(handles));
    return 0;
}

}  // namespace kernel::syscalls
