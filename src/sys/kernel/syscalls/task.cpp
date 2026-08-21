#include <kernel/log.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/mm/vmo.h>
#include <kernel/obj/channel.h>
#include <kernel/obj/handle_dispatch.h>
#include <kernel/obj/port.h>
#include <kernel/obj/socket.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/user_task.h>
#include <kernel/time.h>

#include "internal.h"

namespace kernel::syscalls {

// Spawn returns two handles through the IPC buffer, which the declarative op table is kept free
// of. task_spawn is the buffer-free core; this wrapper is only verification and copy-out.
uint64_t sys_task_spawn(uint64_t handle, uint64_t offset) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, 2 * sizeof(uint64_t))) { return errc_of(ktl::errc::out_of_range); }

    auto task     = calling_task(self);
    auto verified = task->handles().verify(unpack_handle(handle), RIGHT_READ, type_ids::VMO);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }
    auto image   = ktl::static_ref_cast<kernel::mm::vmo>(verified.unwrap().object);

    auto spawned = kernel::sched::task_spawn(*task, ktl::move(image));
    if (spawned.is_err()) { return errc_of(spawned.unwrap_err()); }

    uint64_t handles[2] = {pack_handle(spawned.unwrap().task), pack_handle(spawned.unwrap().mailbox)};
    buffer_write(buffer, offset, handles, sizeof(handles));
    return 0;
}

}  // namespace kernel::syscalls
