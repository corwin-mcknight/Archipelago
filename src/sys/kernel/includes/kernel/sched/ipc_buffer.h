#pragma once

#include <kernel/config.h>
#include <kernel/mm/vmo.h>
#include <stddef.h>
#include <stdint.h>

#include <ktl/ref>
#include <ktl/result>
#include <ktl/span>

namespace kernel::mm { class vm_aspace; }

// IPC buffers pin their backing frames. Syscalls access checked offsets through the physmap,
// so user unmapping cannot invalidate the kernel's view.

namespace kernel::sched {

// A cap, because wired memory a task can demand of the kernel is the classic unbounded-resource
// hole. A real per-task quota belongs with the task/IPC milestone; this is the placeholder.
constexpr size_t IPC_BUFFER_MAX_PAGES      = 16;
constexpr size_t IPC_BUFFER_DEFAULT_PAGES  = 1;

// Buffers live in slots of a reserved address-space region, one per thread -- fixed slots predate
// the VMM's first-fit search. Placed well clear of where images load and where the stack sits.
// The address never reaches user space as a constant -- it arrives in a register at thread
// entry -- so this is free to move onto first-fit.
constexpr uintptr_t IPC_BUFFER_REGION_BASE = 0x10000000;
constexpr size_t IPC_BUFFER_SLOT_BYTES     = IPC_BUFFER_MAX_PAGES * KERNEL_MINIMUM_PAGE_SIZE;
// ponytail: 64 slots is one bitmap word per task; widen the bitmap if a task needs more threads.
constexpr size_t IPC_BUFFER_MAX_SLOTS      = 64;

class Task;
class ipc_buffer;

// Borrows a buffer that must remain alive and unchanged. next() consumes one bounded page chunk.
class IpcRange {
   public:
    size_t size() const { return m_length; }
    ktl::span<uint8_t> next();
    void read(void* dst, size_t length) const;
    void write(const void* src, size_t length) const;

   private:
    friend class ipc_buffer;
    IpcRange(const ipc_buffer& buffer, uint64_t offset, size_t length)
        : m_buffer(&buffer), m_offset(offset), m_length(length) {}

    const ipc_buffer* m_buffer;
    uint64_t m_offset;
    size_t m_length;
};

class ipc_buffer {
   public:
    ipc_buffer() = default;

    bool valid() const { return m_pages != 0; }
    uintptr_t user_base() const { return m_user_base; }
    size_t size_bytes() const { return m_pages * KERNEL_MINIMUM_PAGE_SIZE; }
    size_t slot() const { return m_slot; }

    ktl::result<IpcRange> range(uint64_t offset, uint64_t length) const&;
    ktl::result<IpcRange> range(uint64_t, uint64_t) const&& = delete;

    // Allocate, commit, map into `aspace` at the given slot, and cache the frames.
    static ktl::result<ipc_buffer> create(kernel::mm::vm_aspace& aspace, size_t pages, size_t slot);

   private:
    friend class IpcRange;
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
