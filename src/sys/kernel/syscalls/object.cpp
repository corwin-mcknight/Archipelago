#include <kernel/time.h>

#include "internal.h"

namespace kernel::syscalls {

// The verified reference keeps the object alive across waits and concurrent handle closes.
uint64_t sys_object_wait(sched::Thread& self, uint64_t handle, uint64_t mask, uint64_t timeout_ns) {
    if ((mask >> 32) != 0) { return errc_of(ktl::errc::out_of_range); }

    auto task     = self.owner();
    auto verified = task->handles().verify(kernel::obj::unpack_handle(handle), kernel::obj::RIGHT_WAIT);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }

    auto object = verified.unwrap().object;
    if (mask == 0) { return object->signals(); }
    if (timeout_ns == 0) { return object->wait_signals(static_cast<uint32_t>(mask)); }

    // Round up to ticks and saturate the deadline to prevent overflow.
    ktime_t now      = kernel::time::now();
    ktime_t ticks    = kernel::time::ns_to_ticks_ceil(timeout_ns);
    ktime_t deadline = (now + ticks < now) ? UINT64_MAX : now + ticks;
    uint32_t got     = object->wait_signals_deadline(static_cast<uint32_t>(mask), deadline);
    if ((got & mask) == 0) { return errc_of(ktl::errc::timed_out); }
    return got;
}

}  // namespace kernel::syscalls
