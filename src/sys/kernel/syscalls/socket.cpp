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

// Socket syscalls use hand-rolled dispatch, the same verify pipeline as handle operations, and
// the IPC buffer as the only memory crossing the boundary. Bytes move ring-to-buffer in page
// runs, so a partial result mid-walk is returned as the count, never rewound.
uint64_t sys_socket_create(uint64_t offset) {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, 2 * sizeof(uint64_t))) { return errc_of(ktl::errc::out_of_range); }

    auto created = kernel::obj::Socket::create();
    if (created.is_err()) { return errc_of(created.unwrap_err()); }
    auto pair  = created.unwrap();

    auto task  = calling_task(self);
    auto first = task->handles().insert(pair.first, kernel::obj::Socket::DEFAULT_RIGHTS);
    if (first.is_err()) { return errc_of(first.unwrap_err()); }
    auto second = task->handles().insert(pair.second, kernel::obj::Socket::DEFAULT_RIGHTS);
    if (second.is_err()) {
        (void)task->handles().close(first.unwrap());
        return errc_of(second.unwrap_err());
    }

    uint64_t handles[2] = {kernel::obj::pack_handle(first.unwrap()), kernel::obj::pack_handle(second.unwrap())};
    buffer_write(buffer, offset, handles, sizeof(handles));
    return 0;
}

uint64_t sys_socket_write(uint64_t handle, uint64_t offset, uint64_t length) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, length)) { return errc_of(ktl::errc::out_of_range); }

    auto task     = calling_task(self);
    auto verified = task->handles().verify(unpack_handle(handle), RIGHT_WRITE, type_ids::SOCKET);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }
    auto socket   = ktl::static_ref_cast<Socket>(verified.unwrap().object);

    // Page run by page run; a short acceptance mid-walk is backpressure, reported as the count.
    // An error with bytes already accepted is likewise the count -- those bytes are delivered.
    uint64_t done = 0;
    while (done < length) {
        size_t run       = 0;
        const void* from = reinterpret_cast<const void*>(buffer.kernel_at(offset + done, run));
        size_t attempt   = run < length - done ? run : length - done;
        auto wrote       = socket->write(from, attempt);
        if (wrote.is_err()) { return done != 0 ? done : errc_of(wrote.unwrap_err()); }
        done += wrote.unwrap();
        if (wrote.unwrap() < attempt) { break; }
    }
    return done;
}

uint64_t sys_socket_read(uint64_t handle, uint64_t offset, uint64_t capacity) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, capacity)) { return errc_of(ktl::errc::out_of_range); }

    auto task     = calling_task(self);
    auto verified = task->handles().verify(unpack_handle(handle), RIGHT_READ, type_ids::SOCKET);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }
    auto socket   = ktl::static_ref_cast<Socket>(verified.unwrap().object);

    uint64_t done = 0;
    while (done < capacity) {
        size_t run   = 0;
        void* to     = reinterpret_cast<void*>(buffer.kernel_at(offset + done, run));
        size_t space = run < capacity - done ? run : capacity - done;
        auto got     = socket->read(to, space);
        if (got.is_err()) { return done != 0 ? done : errc_of(got.unwrap_err()); }
        done += got.unwrap();
        if (got.unwrap() < space) { break; }
    }
    return done;
}

}  // namespace kernel::syscalls
