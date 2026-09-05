#include <kernel/obj/channel.h>

#include "internal.h"

namespace kernel::syscalls {

uint64_t sys_channel_create(sched::Thread& self, uint64_t offset) {
    const auto& buffer = self.ipc();
    auto output        = buffer.range(offset, 2 * sizeof(uint64_t));
    if (output.is_err()) { return errc_of(output.unwrap_err()); }

    auto created = kernel::obj::Channel::create();
    if (created.is_err()) { return errc_of(created.unwrap_err()); }
    auto pair = created.unwrap();

    return install_pair(self.owner()->handles(), ktl::move(pair.first), ktl::move(pair.second),
                        kernel::obj::Channel::DEFAULT_RIGHTS, output.unwrap());
}

uint64_t sys_channel_send(sched::Thread& self, uint64_t handle, uint64_t offset, uint64_t length,
                          uint64_t handles_offset, uint64_t handle_count) {
    using namespace kernel::obj;
    const auto& buffer = self.ipc();
    auto data_range    = buffer.range(offset, length);
    if (data_range.is_err()) { return errc_of(data_range.unwrap_err()); }
    auto bytes = data_range.unwrap();
    if (length > Channel::MAX_MESSAGE_BYTES) { return errc_of(ktl::errc::out_of_range); }
    if (handle_count > MessageBuffer::MAX_HANDLES) { return errc_of(ktl::errc::out_of_range); }
    auto handles_range = buffer.range(handle_count == 0 ? 0 : handles_offset, handle_count * sizeof(uint64_t));
    if (handles_range.is_err()) { return errc_of(handles_range.unwrap_err()); }
    auto handles  = handles_range.unwrap();

    auto task     = self.owner();
    Rights needed = RIGHT_WRITE | (handle_count != 0 ? RIGHT_TRANSFER : 0);
    auto found    = task->handles().get<Channel>(unpack_handle(handle), needed);
    if (found.is_err()) { return errc_of(found.unwrap_err()); }
    auto channel = found.unwrap();

    // Reject obvious flow-control failures before consuming handles. A racing send can still
    // fill the queue; once escrow starts, later failures close consumed handles (ABI contract).
    if (handle_count != 0) {
        uint32_t signals = channel->signals();
        if ((signals & Channel::SIGNAL_PEER_CLOSED) != 0) { return errc_of(ktl::errc::peer_closed); }
        if ((signals & Channel::SIGNAL_WRITABLE) == 0) { return errc_of(ktl::errc::capacity_exhausted); }
    }

    auto created = MessageBuffer::create(length);
    if (created.is_err()) { return errc_of(created.unwrap_err()); }
    auto message = created.unwrap();
    bytes.read(message.data(), length);

    uint64_t handle_values[MessageBuffer::MAX_HANDLES];
    handles.read(handle_values, handle_count * sizeof(uint64_t));
    for (uint64_t i = 0; i < handle_count; i++) {
        auto taken = task->handles().take(unpack_handle(handle_values[i]));
        if (taken.is_err()) { return errc_of(taken.unwrap_err()); }
        auto moved    = taken.unwrap();
        auto escrowed = kernel::sched::kernel_task()->handles().insert(ktl::move(moved.object), moved.rights);
        if (escrowed.is_err()) { return errc_of(escrowed.unwrap_err()); }
        message.attach_handle(escrowed.unwrap());
    }

    auto sent = channel->write(ktl::move(message));
    return sent.is_ok() ? 0 : errc_of(sent.unwrap_err());
}

uint64_t sys_channel_recv(sched::Thread& self, uint64_t handle, uint64_t offset, uint64_t capacity,
                          uint64_t handles_offset, uint64_t handle_capacity) {
    using namespace kernel::obj;
    const auto& buffer = self.ipc();
    auto data_range    = buffer.range(offset, capacity);
    if (data_range.is_err()) { return errc_of(data_range.unwrap_err()); }
    auto bytes = data_range.unwrap();
    if (handle_capacity > MessageBuffer::MAX_HANDLES) { return errc_of(ktl::errc::out_of_range); }
    auto handles_range = buffer.range(handle_capacity == 0 ? 0 : handles_offset, handle_capacity * sizeof(uint64_t));
    if (handles_range.is_err()) { return errc_of(handles_range.unwrap_err()); }
    auto handles = handles_range.unwrap();

    auto task    = self.owner();
    auto found   = task->handles().get<Channel>(unpack_handle(handle), RIGHT_READ);
    if (found.is_err()) { return errc_of(found.unwrap_err()); }
    auto channel  = found.unwrap();
    auto received = channel->read(capacity, handle_capacity);
    if (received.is_err()) { return errc_of(received.unwrap_err()); }
    auto message = received.unwrap();
    bytes.write(message.data(), message.size());

    // Failed inserts close those handles; the returned count reports successful deliveries.
    HandleId escrowed[MessageBuffer::MAX_HANDLES];
    size_t count = message.detach_handles(escrowed);
    uint64_t handle_values[MessageBuffer::MAX_HANDLES];
    uint64_t delivered = 0;
    for (size_t i = 0; i < count; i++) {
        auto taken = kernel::sched::kernel_task()->handles().take(escrowed[i]);
        if (taken.is_err()) { continue; }
        auto moved    = taken.unwrap();
        auto inserted = task->handles().insert(ktl::move(moved.object), moved.rights);
        if (inserted.is_err()) { continue; }
        handle_values[delivered++] = pack_handle(inserted.unwrap());
    }
    handles.write(handle_values, delivered * sizeof(uint64_t));
    return (delivered << 32) | message.size();
}

}  // namespace kernel::syscalls
