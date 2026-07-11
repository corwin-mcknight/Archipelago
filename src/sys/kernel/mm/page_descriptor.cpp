#include "kernel/mm/page_descriptor.h"

#include "kernel/config.h"
#include "kernel/log.h"
#include "kernel/mm/pmm.h"

extern uintptr_t g_hhdm_offset;

namespace kernel::mm {

namespace { constexpr size_t PAGE_SIZE = KERNEL_MINIMUM_PAGE_SIZE; }

bool page_descriptor_table::init(const vm_page_region* usable, size_t usable_count, const vm_page_region* wired,
                                 size_t wired_count) {
    vm_paddr_t max_end = 0;
    for (size_t i = 0; i < usable_count; ++i) {
        vm_paddr_t end = usable[i].start + usable[i].count * PAGE_SIZE;
        if (end > max_end) { max_end = end; }
    }
    for (size_t i = 0; i < wired_count; ++i) {
        vm_paddr_t end = wired[i].start + wired[i].count * PAGE_SIZE;
        if (end > max_end) { max_end = end; }
    }
    if (max_end == 0) { return false; }

    m_count            = max_end / PAGE_SIZE;
    size_t array_bytes = m_count * sizeof(page_descriptor);
    size_t array_pages = (array_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    auto base          = g_page_frame_allocator.alloc_contiguous(array_pages);
    if (!base.has_value()) { return false; }

    // Frames arrive zeroed: every descriptor starts {nullptr, 0, 0, MMIO},
    // so holes and firmware ranges default to not-memory.
    m_array = reinterpret_cast<page_descriptor*>(base.value() + g_hhdm_offset);

    for (size_t i = 0; i < usable_count; ++i) { mark_range(usable[i].start, usable[i].count, page_state::FREE); }
    for (size_t i = 0; i < wired_count; ++i) { mark_range(wired[i].start, wired[i].count, page_state::WIRED); }
    // The array's own frames were carved from a usable region (marked FREE
    // above); re-pin them last.
    mark_range(base.value(), array_pages, page_state::WIRED);

    g_log.info("vmm: page descriptors cover {0} frames ({1} KiB array)", m_count, array_bytes / 1024);
    return true;
}

page_descriptor* page_descriptor_table::lookup(vm_paddr_t paddr) {
    size_t pfn = paddr / PAGE_SIZE;
    if (m_array == nullptr || pfn >= m_count) { return nullptr; }
    return &m_array[pfn];
}

const page_descriptor* page_descriptor_table::lookup(vm_paddr_t paddr) const {
    return const_cast<page_descriptor_table*>(this)->lookup(paddr);
}

void page_descriptor_table::set_state(vm_paddr_t paddr, page_state state) {
    if (page_descriptor* desc = lookup(paddr)) { desc->state = state; }
}

void page_descriptor_table::mark_range(vm_paddr_t base, size_t pages, page_state state) {
    for (size_t i = 0; i < pages; ++i) { set_state(base + i * PAGE_SIZE, state); }
}

size_t page_descriptor_table::count(page_state state) const {
    size_t n = 0;
    for (size_t i = 0; i < m_count; ++i) {
        if (m_array[i].state == state) { ++n; }
    }
    return n;
}

page_descriptor_table g_page_descriptors;

}  // namespace kernel::mm
