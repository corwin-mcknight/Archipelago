#include <kernel/mm/vm_aspace.h>
#include <kernel/mm/vmo.h>

#include "internal.h"

namespace kernel::syscalls {

// Automatic mapping hint, clear of images, stacks and IPC buffers; explicit maps may land below it.
constexpr uintptr_t USER_MAP_FLOOR = 0x20000000;

static_assert(::abi::syscall::VM_PAGE_SIZE == KERNEL_MINIMUM_PAGE_SIZE);
static_assert(::abi::syscall::VM_PROT_READ == kernel::mm::vm_prot::READ &&
              ::abi::syscall::VM_PROT_WRITE == kernel::mm::vm_prot::WRITE);

uint64_t sys_vmo_create(sched::Thread& self, uint64_t size) {
    if (size == 0 || (size % KERNEL_MINIMUM_PAGE_SIZE) != 0) { return errc_of(ktl::errc::invalid_operation); }

    auto object = kernel::mm::create_anonymous_vmo(size / KERNEL_MINIMUM_PAGE_SIZE);
    if (object.get() == nullptr) { return errc_of(ktl::errc::oom); }

    auto task     = self.owner();
    auto inserted = task->handles().insert(ktl::move(object), kernel::obj::RIGHT_READ | kernel::obj::RIGHT_WRITE);
    if (inserted.is_err()) { return errc_of(inserted.unwrap_err()); }
    return kernel::obj::pack_handle(inserted.unwrap());
}

uint64_t sys_vmo_map(sched::Thread& self, uint64_t handle, uint64_t vaddr, uint64_t vmo_offset, uint64_t length,
                     uint64_t prot) {
    using namespace kernel::obj;
    // User-created mappings cannot be executable.
    constexpr uint64_t MAPPABLE = ::abi::syscall::VM_PROT_READ | ::abi::syscall::VM_PROT_WRITE;
    if (prot == 0 || (prot & ~MAPPABLE) != 0) { return errc_of(ktl::errc::invalid_operation); }

    auto task    = self.owner();
    auto* aspace = task->aspace();
    if (aspace == nullptr) { return errc_of(ktl::errc::invalid_operation); }

    Rights needed = ((prot & ::abi::syscall::VM_PROT_READ) != 0 ? RIGHT_READ : 0) |
                    ((prot & ::abi::syscall::VM_PROT_WRITE) != 0 ? RIGHT_WRITE : 0);
    auto found    = task->handles().get<kernel::mm::vmo>(unpack_handle(handle), needed);
    if (found.is_err()) { return errc_of(found.unwrap_err()); }
    auto object = found.unwrap();

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

uint64_t sys_vmo_unmap(sched::Thread& self, uint64_t vaddr) {
    auto task    = self.owner();
    auto* aspace = task->aspace();
    if (aspace == nullptr) { return errc_of(ktl::errc::invalid_operation); }

    auto removed = aspace->root().unmap_binding(vaddr);
    return removed.is_ok() ? 0 : errc_of(removed.unwrap_err());
}

}  // namespace kernel::syscalls
