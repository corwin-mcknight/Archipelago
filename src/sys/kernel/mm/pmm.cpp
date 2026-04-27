#include "kernel/mm/pmm.h"

#include <std/string.h>

#include "kernel/log.h"
#include "kernel/mm/page.h"

extern uintptr_t g_hhdm_offset;

namespace kernel::mm {

ktl::maybe<vm_paddr_t> page_frame_allocator::alloc(vm_page_state state) {
    // We only allocate from pages that are zeroed.
    if (m_zeroed.empty()) { zero_more_pages(16); }

    auto addr = m_zeroed.pop();
    if (!addr.has_value()) { return ktl::nothing; }

    --m_free_pages;

    // Update the appropriate stack.
    auto* stack = stack_for_state(state);
    if (stack == nullptr) { return ktl::nothing; }
    if (!stack->push(addr.value())) { return ktl::nothing; }

    return addr;
}

void page_frame_allocator::free(vm_paddr_t addr) {
    // Finding the correct page state stack is non-trivial without additional metadata.
    // We push to dirty stack, so that pages can be freed and zeroed later.
    // This means memory is considered dirty AND it's original state.
    if (m_dirty.push(addr)) {
        ++m_free_pages;
    } else {
        g_log.error("pmm: Failed to free page at 0x{0:p}", addr);
    }

    update_dirty_pages();
}

ktl::maybe<vm_paddr_t> page_frame_allocator::pop_free_page() {
    // First drain dirty pages (recycled via free()).
    if (!m_dirty.empty()) {
        auto addr = m_dirty.pop();
        if (addr.has_value()) { return addr; }
    }

    // Then pop from the region list.
    while (!m_regions.empty()) {
        auto& region = m_regions[m_regions.size() - 1];
        if (region.count == 0) {
            m_regions.pop_back();
            continue;
        }
        --region.count;
        vm_paddr_t addr = region.start + region.count * 0x1000;
        return addr;
    }

    return ktl::nothing;
}

void page_frame_allocator::zero_page(vm_paddr_t addr) {
    memset(reinterpret_cast<void*>(addr + g_hhdm_offset), 0, 0x1000);
}

void page_frame_allocator::update_dirty_pages() {
    while (!m_dirty.empty()) {
        auto addr = m_dirty.pop();
        if (!addr.has_value()) { break; }
        zero_page(addr.value());
        if (!m_zeroed.push(addr.value())) { break; }
    }
}

void page_frame_allocator::zero_more_pages(size_t count) {
    for (size_t i = 0; i < count; i++) {
        auto addr = pop_free_page();
        if (!addr.has_value()) { break; }
        zero_page(addr.value());
        if (!m_zeroed.push(addr.value())) { break; }
    }
}

void page_frame_allocator::debug_print_state() {
    size_t total_kb = (m_total_pages * 0x1000) / 1024;
    size_t free_kb  = (m_free_pages * 0x1000) / 1024;
    size_t wired_kb = (m_wired_pages * 0x1000) / 1024;
    size_t used_kb  = total_kb - free_kb;

    g_log.info("Page Frame Allocator State:");
    if (total_kb >= 1024) {
        g_log.info("  Total:   {0} MiB ({1} pages)", total_kb / 1024, m_total_pages);
    } else {
        g_log.info("  Total:   {0} KiB ({1} pages)", total_kb, m_total_pages);
    }
    if (free_kb >= 1024) {
        g_log.info("  Free:    {0} MiB ({1} pages)", free_kb / 1024, m_free_pages);
    } else {
        g_log.info("  Free:    {0} KiB ({1} pages)", free_kb, m_free_pages);
    }
    if (used_kb >= 1024) {
        g_log.info("  Used:    {0} MiB ({1} pages)", used_kb / 1024, m_total_pages - m_free_pages);
    } else {
        g_log.info("  Used:    {0} KiB ({1} pages)", used_kb, m_total_pages - m_free_pages);
    }
    if (wired_kb >= 1024) {
        g_log.info("  Wired:   {0} MiB ({1} pages)", wired_kb / 1024, m_wired_pages);
    } else {
        g_log.info("  Wired:   {0} KiB ({1} pages)", wired_kb, m_wired_pages);
    }
    g_log.info("  Zeroed:  {0} pages", m_zeroed.size());
    g_log.info("  Active:  {0} pages", m_active.size());
    g_log.info("  Dirty:   {0} pages", m_dirty.size());
    g_log.info("  Regions: {0}", m_regions.size());
}

page_frame_allocator g_page_frame_allocator;

}  // namespace kernel::mm