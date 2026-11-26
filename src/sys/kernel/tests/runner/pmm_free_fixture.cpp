#include <kernel/config.h>
#include <kernel/mm/page_descriptor.h>
#include <kernel/mm/pmm.h>
#include <kernel/testing/testing.h>

// The portability check links the real PMM and leaves the direct map uninitialized.
// A free-list write would therefore raise a different panic before it could touch memory.
namespace kernel::mm {
namespace {
page_descriptor frame{};

void release_with_state(page_state state, vm_paddr_t address = 0) {
    g_page_descriptors.init(nullptr, 0, nullptr, 0);
    frame.state = state;
    g_page_frame_allocator.free(address);
}
}  // namespace

// The synthetic table covers just frame zero after init. Each test starts in a fresh child.
page_descriptor_table g_page_descriptors;

bool page_descriptor_table::init(const vm_page_region*, size_t, const vm_page_region*, size_t) {
    m_array = &frame;
    m_count = 1;
    return true;
}

page_descriptor* page_descriptor_table::lookup(vm_paddr_t address) {
    return address < coverage_end() ? m_array : nullptr;
}

void page_descriptor_table::set_state(vm_paddr_t, page_state) { panic("fixture: unexpected descriptor update"); }

KTEST_MODULE("mm/pmm");

KTEST_CASE_CRASH(pmm_free_unaligned) { release_with_state(page_state::ACTIVE, 1); }
KTEST_CASE_CRASH(pmm_free_before_descriptors) { g_page_frame_allocator.free(0); }
KTEST_CASE_CRASH(pmm_free_outside_coverage) { release_with_state(page_state::ACTIVE, KERNEL_MINIMUM_PAGE_SIZE); }
KTEST_CASE_CRASH(pmm_free_wired) { release_with_state(page_state::WIRED); }
KTEST_CASE_CRASH(pmm_free_mmio) { release_with_state(page_state::MMIO); }
KTEST_CASE_CRASH(pmm_free_already_free) { release_with_state(page_state::FREE); }
KTEST_CASE_CRASH(pmm_free_already_zeroed) { release_with_state(page_state::ZEROED); }
}  // namespace kernel::mm
