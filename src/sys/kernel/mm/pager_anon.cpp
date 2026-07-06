#include "kernel/mm/pager.h"
#include "kernel/mm/pmm.h"

namespace kernel::mm {

// Zero-fill: every page of an anonymous VMO materializes as a fresh zeroed
// PMM frame. The PMM alloc hook marks the descriptor ACTIVE; the VMO stamps
// ownership.
ktl::result<vm_paddr_t> anonymous_pager::fill(uint64_t page) {
    (void)page;
    return ktl::ok_or(g_page_frame_allocator.alloc(), ktl::errc::oom);
}

}  // namespace kernel::mm
