#include "kernel/mm/pmm.h"

#include <std/string.h>

#include "kernel/log.h"
#include "kernel/mm/page.h"

extern uintptr_t g_hhdm_offset;

namespace kernel::mm {

ktl::maybe<vm_paddr_t> page_frame_allocator::alloc() {
    auto addr = m_zeroed.pop();
    if (!addr.has_value()) {
        // Fallback: zero one page inline. The background zeroing worker is
        // expected to keep the zeroed pool topped up; we land here only when
        // the worker has not run yet (early boot) or is behind demand.
        auto raw = pop_free_page();
        if (!raw.has_value()) { return ktl::nothing; }
        zero_page(raw.value());
        addr = raw;
    }
    --m_free_pages;
    return addr;
}

void page_frame_allocator::free(vm_paddr_t addr) {
    if (!m_dirty.push(addr)) {
        g_log.error("pmm: Failed to free page at 0x{0:p}", addr);
        return;
    }
    ++m_free_pages;
}

ktl::maybe<vm_paddr_t> page_frame_allocator::pop_free_page() {
    if (!m_dirty.empty()) {
        auto addr = m_dirty.pop();
        if (addr.has_value()) { return addr; }
    }

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

void page_frame_allocator::debug_print_state() {
    size_t total_kb    = (m_total_pages * 0x1000) / 1024;
    size_t free_kb     = (m_free_pages * 0x1000) / 1024;
    size_t reserved_kb = (m_reserved_pages * 0x1000) / 1024;
    size_t used_kb     = total_kb - free_kb;

    g_log.info("Page Frame Allocator State:");
    if (total_kb >= 1024) {
        g_log.info("  Total:    {0} MiB ({1} pages)", total_kb / 1024, m_total_pages);
    } else {
        g_log.info("  Total:    {0} KiB ({1} pages)", total_kb, m_total_pages);
    }
    if (free_kb >= 1024) {
        g_log.info("  Free:     {0} MiB ({1} pages)", free_kb / 1024, m_free_pages);
    } else {
        g_log.info("  Free:     {0} KiB ({1} pages)", free_kb, m_free_pages);
    }
    if (used_kb >= 1024) {
        g_log.info("  Used:     {0} MiB ({1} pages)", used_kb / 1024, m_total_pages - m_free_pages);
    } else {
        g_log.info("  Used:     {0} KiB ({1} pages)", used_kb, m_total_pages - m_free_pages);
    }
    if (reserved_kb >= 1024) {
        g_log.info("  Reserved: {0} MiB ({1} pages)", reserved_kb / 1024, m_reserved_pages);
    } else {
        g_log.info("  Reserved: {0} KiB ({1} pages)", reserved_kb, m_reserved_pages);
    }
    g_log.info("  Zeroed:   {0} pages", m_zeroed.size());
    g_log.info("  Dirty:    {0} pages", m_dirty.size());
    g_log.info("  Regions:  {0}", m_regions.size());
}

page_frame_allocator g_page_frame_allocator;

}  // namespace kernel::mm
