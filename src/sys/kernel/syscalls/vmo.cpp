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

// User-controlled memory (<abi/syscall.h>): the mm layer is already task-shaped -- per-task
// region trees, VMO lifetime by ref -- so these are verification plus dispatch. Mapping into the
// root region with no reserved ranges is deliberate: a task that unmaps its own stack or text
// only faults itself, and the IPC buffer's frames are pinned by the ipc_buffer's own ref.

// Where the kernel-picked search starts: clear of where images load, the stack, and the
// IPC-buffer window. A hint, not a reservation -- explicit maps may land below it.
constexpr uintptr_t USER_MAP_FLOOR = 0x20000000;

static_assert(::abi::syscall::VM_PAGE_SIZE == KERNEL_MINIMUM_PAGE_SIZE);
static_assert(::abi::syscall::VM_PROT_READ == kernel::mm::vm_prot::READ &&
              ::abi::syscall::VM_PROT_WRITE == kernel::mm::vm_prot::WRITE);

uint64_t sys_vmo_create(uint64_t size) {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    if (size == 0 || (size % KERNEL_MINIMUM_PAGE_SIZE) != 0) { return errc_of(ktl::errc::invalid_operation); }

    auto object = kernel::mm::create_anonymous_vmo(size / KERNEL_MINIMUM_PAGE_SIZE);
    if (object.get() == nullptr) { return errc_of(ktl::errc::oom); }

    auto task     = calling_task(self);
    auto inserted = task->handles().insert(ktl::move(object), kernel::obj::RIGHT_READ | kernel::obj::RIGHT_WRITE);
    if (inserted.is_err()) { return errc_of(inserted.unwrap_err()); }
    return kernel::obj::pack_handle(inserted.unwrap());
}

uint64_t sys_vmo_map(uint64_t handle, uint64_t vaddr, uint64_t vmo_offset, uint64_t length, uint64_t prot) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    // EXEC (or any unknown bit) is rejected outright: user-minted executable mappings wait for
    // the userspace-loader milestone, which is what keeps W^X trivially true.
    constexpr uint64_t MAPPABLE = ::abi::syscall::VM_PROT_READ | ::abi::syscall::VM_PROT_WRITE;
    if (prot == 0 || (prot & ~MAPPABLE) != 0) { return errc_of(ktl::errc::invalid_operation); }

    auto task    = calling_task(self);
    auto* aspace = task->aspace();
    if (aspace == nullptr) { return errc_of(ktl::errc::invalid_operation); }

    // Each requested protection needs the matching right on the handle.
    Rights needed = ((prot & ::abi::syscall::VM_PROT_READ) != 0 ? RIGHT_READ : 0) |
                    ((prot & ::abi::syscall::VM_PROT_WRITE) != 0 ? RIGHT_WRITE : 0);
    auto verified = task->handles().verify(unpack_handle(handle), needed, type_ids::VMO);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }
    auto object = ktl::static_ref_cast<kernel::mm::vmo>(verified.unwrap().object);

    auto kprot  = static_cast<kernel::mm::vm_prot_t>(prot) | kernel::mm::vm_prot::USER;
    if (vaddr == 0) {
        auto mapped = aspace->root().map_anywhere(USER_MAP_FLOOR, length, ktl::move(object), vmo_offset, kprot);
        if (mapped.is_err()) { return errc_of(mapped.unwrap_err()); }
        return mapped.unwrap();
    }
    auto mapped = aspace->root().map(vaddr, length, ktl::move(object), vmo_offset, kprot);
    if (mapped.is_err()) { return errc_of(mapped.unwrap_err()); }
    return vaddr;
}

uint64_t sys_vmo_unmap(uint64_t vaddr) {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    auto task    = calling_task(self);
    auto* aspace = task->aspace();
    if (aspace == nullptr) { return errc_of(ktl::errc::invalid_operation); }

    auto removed = aspace->root().unmap_binding(vaddr);
    return removed.is_ok() ? 0 : errc_of(removed.unwrap_err());
}

}  // namespace kernel::syscalls
