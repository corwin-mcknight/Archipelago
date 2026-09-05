#include <kernel/obj/socket.h>

#include "internal.h"

namespace kernel::syscalls {

uint64_t sys_socket_create(sched::Thread& self, uint64_t offset) {
    const auto& buffer = self.ipc();
    auto output        = buffer.range(offset, 2 * sizeof(uint64_t));
    if (output.is_err()) { return errc_of(output.unwrap_err()); }

    auto created = kernel::obj::Socket::create();
    if (created.is_err()) { return errc_of(created.unwrap_err()); }
    auto pair = created.unwrap();

    return install_pair(self.owner()->handles(), ktl::move(pair.first), ktl::move(pair.second),
                        kernel::obj::Socket::DEFAULT_RIGHTS, output.unwrap());
}

uint64_t sys_socket_write(sched::Thread& self, uint64_t handle, uint64_t offset, uint64_t length) {
    using namespace kernel::obj;
    const auto& buffer = self.ipc();
    auto checked       = buffer.range(offset, length);
    if (checked.is_err()) { return errc_of(checked.unwrap_err()); }
    auto bytes = checked.unwrap();

    auto task  = self.owner();
    auto found = task->handles().get<Socket>(unpack_handle(handle), RIGHT_WRITE);
    if (found.is_err()) { return errc_of(found.unwrap_err()); }
    auto socket   = found.unwrap();

    // After partial delivery, return the byte count even if a later page fails.
    uint64_t done = 0;
    for (auto chunk = bytes.next(); !chunk.empty(); chunk = bytes.next()) {
        auto wrote = socket->write(chunk.data(), chunk.size());
        if (wrote.is_err()) { return done != 0 ? done : errc_of(wrote.unwrap_err()); }
        size_t accepted = wrote.unwrap();
        done += accepted;
        if (accepted < chunk.size()) { break; }
    }
    return done;
}

uint64_t sys_socket_read(sched::Thread& self, uint64_t handle, uint64_t offset, uint64_t capacity) {
    using namespace kernel::obj;
    const auto& buffer = self.ipc();
    auto checked       = buffer.range(offset, capacity);
    if (checked.is_err()) { return errc_of(checked.unwrap_err()); }
    auto bytes = checked.unwrap();

    auto task  = self.owner();
    auto found = task->handles().get<Socket>(unpack_handle(handle), RIGHT_READ);
    if (found.is_err()) { return errc_of(found.unwrap_err()); }
    auto socket   = found.unwrap();

    uint64_t done = 0;
    for (auto chunk = bytes.next(); !chunk.empty(); chunk = bytes.next()) {
        auto got = socket->read(chunk.data(), chunk.size());
        if (got.is_err()) { return done != 0 ? done : errc_of(got.unwrap_err()); }
        size_t received = got.unwrap();
        done += received;
        if (received < chunk.size()) { break; }
    }
    return done;
}

}  // namespace kernel::syscalls
