#include "kernel/mm/pager.h"
#include "kernel/mm/pmm.h"

namespace kernel::mm {

// Zero-fill: every page of an anonymous VMO materializes as a fresh zeroed
// PMM frame. The PMM alloc hook marks the descriptor ACTIVE; the VMO stamps
// ownership.
ktl::result<vm_paddr_t> anonymous_pager::fill(uint64_t page) {
    (void)page;
    auto frame = g_page_frame_allocator.alloc();
    if (!frame.has_value()) { return ktl::err(ktl::errc::oom); }
    return ktl::result<vm_paddr_t>::ok(frame.value());
}

}  // namespace kernel::mm
