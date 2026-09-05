#include <kernel/obj/port.h>
#include <kernel/time.h>

#include "internal.h"

namespace kernel::syscalls {

uint64_t sys_port_create(sched::Thread& self) {
    auto task    = self.owner();
    auto created = task->handles().emplace<kernel::obj::Port>(kernel::obj::Port::DEFAULT_RIGHTS);
    if (created.is_err()) { return errc_of(created.unwrap_err()); }
    return kernel::obj::pack_handle(created.unwrap());
}

uint64_t sys_port_bind(sched::Thread& self, uint64_t port_handle, uint64_t object_handle, uint64_t key, uint64_t mask) {
    using namespace kernel::obj;
    if (mask == 0 || (mask >> 32) != 0) { return errc_of(ktl::errc::out_of_range); }

    auto task = self.owner();
    auto port = task->handles().get<Port>(unpack_handle(port_handle), RIGHT_WRITE);
    if (port.is_err()) { return errc_of(port.unwrap_err()); }
    auto object = task->handles().verify(unpack_handle(object_handle), RIGHT_WAIT);
    if (object.is_err()) { return errc_of(object.unwrap_err()); }

    auto bound = port.unwrap()->bind(ktl::move(object.unwrap().object), key, static_cast<uint32_t>(mask));
    return bound.is_ok() ? 0 : errc_of(bound.unwrap_err());
}

uint64_t sys_port_unbind(sched::Thread& self, uint64_t port_handle, uint64_t key) {
    using namespace kernel::obj;
    auto task = self.owner();
    auto port = task->handles().get<Port>(unpack_handle(port_handle), RIGHT_WRITE);
    if (port.is_err()) { return errc_of(port.unwrap_err()); }
    return port.unwrap()->unbind(key);
}

uint64_t sys_port_wait(sched::Thread& self, uint64_t port_handle, uint64_t offset, uint64_t timeout_ns) {
    using namespace kernel::obj;
    const auto& buffer = self.ipc();
    auto output        = buffer.range(offset, 2 * sizeof(uint64_t));
    if (output.is_err()) { return errc_of(output.unwrap_err()); }

    auto task    = self.owner();
    auto claimed = task->handles().get<Port>(unpack_handle(port_handle), RIGHT_READ | RIGHT_WAIT);
    if (claimed.is_err()) { return errc_of(claimed.unwrap_err()); }
    auto port        = claimed.unwrap();

    ktime_t deadline = 0;
    if (timeout_ns != 0) {
        ktime_t now   = kernel::time::now();
        ktime_t ticks = kernel::time::ns_to_ticks_ceil(timeout_ns);
        deadline      = (now + ticks < now) ? UINT64_MAX : now + ticks;
    }

    // READABLE may be stale after another reader drains the queue; retry with the same deadline.
    // Killed threads must return to the dispatcher's exit boundary instead of waiting again.
    Port::Packet packet;
    while (!port->dequeue(packet)) {
        if (self.killed()) { return errc_of(ktl::errc::timed_out); }
        if (timeout_ns == 0) {
            (void)port->wait_signals(Port::SIGNAL_READABLE);
            continue;
        }
        uint32_t got = port->wait_signals_deadline(Port::SIGNAL_READABLE, deadline);
        if ((got & Port::SIGNAL_READABLE) == 0) { return errc_of(ktl::errc::timed_out); }
    }
    uint64_t out[2] = {packet.key, packet.signals};
    output.unwrap().write(out, sizeof(out));
    return 0;
}

}  // namespace kernel::syscalls
