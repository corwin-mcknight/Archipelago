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

// A handle names an entry in the calling task's table, so resolve the caller before entering the
// pipeline. Kernel threads land on task zero's table, which is what lets kernel-context tests
// drive the real path end to end.
ktl::ref<kernel::sched::Task> calling_task(ktl::ref<kernel::sched::Thread>& self) {
    auto task = ktl::static_ref_cast<kernel::sched::Task>(self->owner());
    if (!task) { task = kernel::sched::kernel_task(); }
    return task;
}

uint64_t handle_syscall(uint64_t nr, uint64_t a0, uint64_t a1) {
    auto self = kernel::sched::current();
    if (!self) { return static_cast<uint64_t>(ktl::errc::invalid_operation); }
    auto task = calling_task(self);
    return kernel::obj::dispatch_handle_op(task->handles(), nr, a0, a1);
}

uint64_t errc_of(ktl::errc error) { return static_cast<uint64_t>(error); }

// The installed error codes in <abi/syscall.h> are spelled as literals there; pin each to its
// kernel-internal value so the two cannot drift.
static_assert(static_cast<int64_t>(ktl::errc::truncated) == ::abi::syscall::ERR_TRUNCATED);
static_assert(static_cast<int64_t>(ktl::errc::would_block) == ::abi::syscall::ERR_WOULD_BLOCK);
static_assert(static_cast<int64_t>(ktl::errc::peer_closed) == ::abi::syscall::ERR_PEER_CLOSED);
static_assert(static_cast<int64_t>(ktl::errc::timed_out) == ::abi::syscall::ERR_TIMED_OUT);

// Copy between a pre-validated IPC buffer range and contiguous kernel memory, page run by page
// run -- the backing frames are not physically contiguous. Callers check contains() first.
void buffer_write(const kernel::sched::ipc_buffer& buffer, uint64_t offset, const void* src, size_t length) {
    size_t done = 0;
    while (done < length) {
        size_t run  = 0;
        void* to    = reinterpret_cast<void*>(buffer.kernel_at(offset + done, run));
        size_t take = run < length - done ? run : length - done;
        __builtin_memcpy(to, static_cast<const uint8_t*>(src) + done, take);
        done += take;
    }
}

void buffer_read(const kernel::sched::ipc_buffer& buffer, uint64_t offset, void* dst, size_t length) {
    size_t done = 0;
    while (done < length) {
        size_t run       = 0;
        const void* from = reinterpret_cast<const void*>(buffer.kernel_at(offset + done, run));
        size_t take      = run < length - done ? run : length - done;
        __builtin_memcpy(static_cast<uint8_t*>(dst) + done, from, take);
        done += take;
    }
}

}  // namespace kernel::syscalls
