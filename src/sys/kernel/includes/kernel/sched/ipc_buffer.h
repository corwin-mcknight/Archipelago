#pragma once

#include <kernel/config.h>
#include <kernel/mm/vmo.h>
#include <stddef.h>
#include <stdint.h>

#include <ktl/ref>
#include <ktl/result>

namespace kernel::mm { class vm_aspace; }

// A thread's IPC buffer: the only memory a syscall ever reads from its caller.
//
// No user pointer crosses the syscall boundary. Each thread that has an address space gets a
// committed anonymous VMO mapped into it, and the kernel caches the physmap address of every one of
// its frames at creation. A syscall argument is then an offset and a length into that buffer, so
// validation is a bounds check and the copy is a memcpy through the physmap -- no page-table walk,
// no VMM lock, no fault, and no window in which the mapping can change under the kernel.
//
// The buffer is committed and never decommitted, so the cached frames stay valid for the thread's
// life. A task that unmaps its own buffer only blinds itself: this reference keeps the VMO alive and
// the kernel keeps writing to the same frames, so there is no way to turn it into a kernel fault.

namespace kernel::sched {

// A cap, because wired memory a task can demand of the kernel is the classic unbounded-resource
// hole. A real per-task quota belongs with the task/IPC milestone; this is the placeholder.
constexpr size_t IPC_BUFFER_MAX_PAGES      = 16;
constexpr size_t IPC_BUFFER_DEFAULT_PAGES  = 1;

// Buffers live in slots of a reserved address-space region, one per thread, because the VMM has no
// first-fit search yet and every thread in a task needs a distinct address. Placed well clear of
// where images load and where the stack sits. The address never reaches user space as a constant --
// it arrives in a register at thread entry -- so this is free to change when first-fit lands.
constexpr uintptr_t IPC_BUFFER_REGION_BASE = 0x10000000;
constexpr size_t IPC_BUFFER_SLOT_BYTES     = IPC_BUFFER_MAX_PAGES * KERNEL_MINIMUM_PAGE_SIZE;
// ponytail: 64 slots is one bitmap word per task; widen the bitmap if a task needs more threads.
constexpr size_t IPC_BUFFER_MAX_SLOTS      = 64;

class Task;

class ipc_buffer {
   public:
    ipc_buffer() = default;

    bool valid() const { return m_pages != 0; }
    uintptr_t user_base() const { return m_user_base; }
    size_t size_bytes() const { return m_pages * KERNEL_MINIMUM_PAGE_SIZE; }
    size_t slot() const { return m_slot; }

    // Whether [offset, offset + length) lies inside the buffer, computed without overflow so a
    // crafted offset/length pair cannot wrap past the end and look in-bounds.
    bool contains(uint64_t offset, uint64_t length) const {
        uint64_t size = size_bytes();
        return offset <= size && length <= size - offset;
    }

    // Kernel-readable address of `offset`, plus how many bytes remain in that page. Callers walk a
    // range page by page because the backing frames are not physically contiguous.
    uintptr_t kernel_at(uint64_t offset, size_t& run_out) const;

    // Allocate, commit, map into `aspace` at the given slot, and cache the frames.
    static ktl::result<ipc_buffer> create(kernel::mm::vm_aspace& aspace, size_t pages, size_t slot);

   private:
    ktl::ref<kernel::mm::vmo> m_backing;
    uintptr_t m_user_base                    = 0;
    size_t m_pages                           = 0;
    size_t m_slot                            = 0;
    // Physmap address of each frame, resolved once at creation. Fixed-size so creation needs no
    // second allocation that could fail after the VMO already exists.
    uintptr_t m_frames[IPC_BUFFER_MAX_PAGES] = {};
};

// Unmap a dead thread's buffer and return its slot to the task's pool. Paired with
// Task::remove_thread by every caller that drops a thread; kept out of Task itself because that
// class is linked by the host test tier and must not pull in the VMM.
void release_thread_ipc(Task& task, const ipc_buffer& buffer);

}  // namespace kernel::sched
