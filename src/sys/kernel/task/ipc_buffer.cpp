#include <kernel/mm/physmap.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/sched/ipc_buffer.h>
#include <kernel/sched/task.h>

namespace kernel::sched {

namespace { constexpr size_t PAGE_SIZE = KERNEL_MINIMUM_PAGE_SIZE; }  // namespace

uintptr_t ipc_buffer::kernel_at(uint64_t offset, size_t& run_out) const {
    size_t page    = static_cast<size_t>(offset / PAGE_SIZE);
    size_t in_page = static_cast<size_t>(offset % PAGE_SIZE);
    run_out        = PAGE_SIZE - in_page;
    return m_frames[page] + in_page;
}

void release_thread_ipc(Task& task, const ipc_buffer& buffer) {
    if (!buffer.valid()) { return; }
    // Unmap before returning the slot: the slot is only reusable once its address range is free,
    // and the next thread handed it would otherwise fail to map over the corpse of this one.
    // Deliberately not done inside Task -- that class is linked by the host tier and must stay free
    // of the VMM.
    if (task.aspace() != nullptr) { (void)task.aspace()->root().unmap(buffer.user_base(), buffer.size_bytes()); }
    task.release_ipc_slot(buffer.slot());
}

ktl::result<ipc_buffer> ipc_buffer::create(kernel::mm::vm_aspace& aspace, size_t pages, size_t slot) {
    using namespace kernel::mm;

    if (pages == 0 || pages > IPC_BUFFER_MAX_PAGES) { return ktl::err(ktl::errc::out_of_range); }
    if (slot >= IPC_BUFFER_MAX_SLOTS) { return ktl::err(ktl::errc::capacity_exhausted); }

    auto backing = create_anonymous_vmo(pages);
    if (!backing) { return ktl::err(ktl::errc::oom); }

    // Committed up front and never decommitted: the cached frames below are only valid because
    // nothing gives these pages back to the PMM before the thread dies.
    auto committed = backing->commit(0, pages);
    if (committed.is_err()) { return ktl::err(committed.unwrap_err()); }

    ipc_buffer buffer;
    for (size_t page = 0; page < pages; ++page) {
        auto frame = backing->resident_frame(page);
        if (!frame.has_value()) { return ktl::err(ktl::errc::oom); }
        buffer.m_frames[page] = kernel::mm::direct_map_address(kernel::mm::physical_address(frame.value()));
    }

    uintptr_t base = IPC_BUFFER_REGION_BASE + slot * IPC_BUFFER_SLOT_BYTES;
    auto mapped =
        aspace.root().map(base, pages * PAGE_SIZE, backing, 0, vm_prot::USER | vm_prot::READ | vm_prot::WRITE);
    if (mapped.is_err()) { return ktl::err(mapped.unwrap_err()); }

    buffer.m_backing   = ktl::move(backing);
    buffer.m_user_base = base;
    buffer.m_pages     = pages;
    buffer.m_slot      = slot;
    return ktl::result<ipc_buffer>::ok(ktl::move(buffer));
}

}  // namespace kernel::sched
