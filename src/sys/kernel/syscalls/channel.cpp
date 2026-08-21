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

// Channel syscalls take up to five arguments, more than the declarative handle-op table carries,
// and need the calling thread's IPC buffer, which that pipeline is kept free of. They run the
// same HandleTable::verify checks; only the dispatch is hand-rolled.

uint64_t sys_channel_create(uint64_t offset) {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, 2 * sizeof(uint64_t))) { return errc_of(ktl::errc::out_of_range); }

    auto created = kernel::obj::Channel::create();
    if (created.is_err()) { return errc_of(created.unwrap_err()); }
    auto pair  = created.unwrap();

    auto task  = calling_task(self);
    auto first = task->handles().insert(pair.first, kernel::obj::Channel::DEFAULT_RIGHTS);
    if (first.is_err()) { return errc_of(first.unwrap_err()); }
    auto second = task->handles().insert(pair.second, kernel::obj::Channel::DEFAULT_RIGHTS);
    if (second.is_err()) {
        (void)task->handles().close(first.unwrap());
        return errc_of(second.unwrap_err());
    }

    uint64_t handles[2] = {kernel::obj::pack_handle(first.unwrap()), kernel::obj::pack_handle(second.unwrap())};
    buffer_write(buffer, offset, handles, sizeof(handles));
    return 0;
}

uint64_t sys_channel_send(uint64_t handle, uint64_t offset, uint64_t length, uint64_t handles_offset,
                          uint64_t handle_count) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, length)) { return errc_of(ktl::errc::out_of_range); }
    if (length > Channel::MAX_MESSAGE_BYTES) { return errc_of(ktl::errc::out_of_range); }
    if (handle_count > MessageBuffer::MAX_HANDLES) { return errc_of(ktl::errc::out_of_range); }
    if (handle_count != 0 && !buffer.contains(handles_offset, handle_count * sizeof(uint64_t))) {
        return errc_of(ktl::errc::out_of_range);
    }

    // Sending handles is gated by the transfer right on the channel handle itself.
    auto task     = calling_task(self);
    Rights needed = RIGHT_WRITE | (handle_count != 0 ? RIGHT_TRANSFER : 0);
    auto verified = task->handles().verify(unpack_handle(handle), needed, type_ids::CHANNEL);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }
    auto channel = ktl::static_ref_cast<Channel>(verified.unwrap().object);

    // Once escrow begins, the handles belong to the message: any later failure closes them (see
    // <abi/syscall.h>). So fail the ordinary flow-control cases -- full queue, dead peer -- before
    // consuming anything, by the same signals a waiting sender uses. A racing writer on this same
    // endpoint can still fill the queue between this check and the write; that residual race is
    // what the close-on-failure rule is for.
    if (handle_count != 0) {
        uint32_t signals = channel->signals();
        if ((signals & Channel::SIGNAL_PEER_CLOSED) != 0) { return errc_of(ktl::errc::peer_closed); }
        if ((signals & Channel::SIGNAL_WRITABLE) == 0) { return errc_of(ktl::errc::capacity_exhausted); }
    }

    // The message page is contiguous, so staging is one memcpy per source page run.
    auto created = MessageBuffer::create(length);
    if (created.is_err()) { return errc_of(created.unwrap_err()); }
    auto message = created.unwrap();
    if (length != 0) { buffer_read(buffer, offset, message.data(), length); }

    uint64_t handle_values[MessageBuffer::MAX_HANDLES];
    if (handle_count != 0) { buffer_read(buffer, handles_offset, handle_values, handle_count * sizeof(uint64_t)); }
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

uint64_t sys_channel_recv(uint64_t handle, uint64_t offset, uint64_t capacity, uint64_t handles_offset,
                          uint64_t handle_capacity) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, capacity)) { return errc_of(ktl::errc::out_of_range); }
    if (handle_capacity > MessageBuffer::MAX_HANDLES) { return errc_of(ktl::errc::out_of_range); }
    if (handle_capacity != 0 && !buffer.contains(handles_offset, handle_capacity * sizeof(uint64_t))) {
        return errc_of(ktl::errc::out_of_range);
    }

    auto task     = calling_task(self);
    auto verified = task->handles().verify(unpack_handle(handle), RIGHT_READ, type_ids::CHANNEL);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }

    auto channel  = ktl::static_ref_cast<Channel>(verified.unwrap().object);
    auto received = channel->read(capacity, handle_capacity);
    if (received.is_err()) { return errc_of(received.unwrap_err()); }
    auto message = received.unwrap();
    if (message.size() != 0) { buffer_write(buffer, offset, message.data(), message.size()); }

    // Move each handle out of escrow into the caller's table. An insert the receiver's table
    // cannot make (OOM) closes that handle -- the message is already dequeued, so the returned
    // count is how the caller learns what actually arrived.
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
    if (delivered != 0) { buffer_write(buffer, handles_offset, handle_values, delivered * sizeof(uint64_t)); }
    return (delivered << 32) | message.size();
}

}  // namespace kernel::syscalls
