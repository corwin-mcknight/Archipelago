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

// Wait (nonzero mask) or poll (zero mask) on any object's signals; see <abi/syscall.h>. It stays
// outside the op table because it blocks, which the scheduler-free dispatch pipeline must never
// do. The verified ref pins the object for the whole wait, so a concurrent close of the handle
// cannot free it out from under the sleeping thread.
uint64_t sys_object_wait(uint64_t handle, uint64_t mask, uint64_t timeout_ns) {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    if ((mask >> 32) != 0) { return errc_of(ktl::errc::out_of_range); }

    auto task     = calling_task(self);
    auto verified = task->handles().verify(kernel::obj::unpack_handle(handle), kernel::obj::RIGHT_WAIT);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }

    auto object = verified.unwrap().object;
    if (mask == 0) { return object->signals(); }
    if (timeout_ns == 0) { return object->wait_signals(static_cast<uint32_t>(mask)); }

    // The timeout rounds up to ticks and the deadline saturates: a huge value waits forever
    // rather than wrapping to a deadline already in the past.
    ktime_t now      = kernel::time::now();
    ktime_t ticks    = kernel::time::ns_to_ticks_ceil(timeout_ns);
    ktime_t deadline = (now + ticks < now) ? UINT64_MAX : now + ticks;
    uint32_t got     = object->wait_signals_deadline(static_cast<uint32_t>(mask), deadline);
    if ((got & mask) == 0) { return errc_of(ktl::errc::timed_out); }
    return got;
}

}  // namespace kernel::syscalls
