#include "kernel/mm/pmm.h"

#include <std/string.h>

#include "kernel/config.h"
#include "kernel/log.h"
#include "kernel/mm/page.h"

extern uintptr_t g_hhdm_offset;

namespace kernel::mm {

namespace {
constexpr size_t PAGE_SIZE = KERNEL_MINIMUM_PAGE_SIZE;

// Logs a labeled page count, auto-scaling to KiB or MiB. The label carries its
// own trailing padding so the columns line up.
void log_size(const char* label, size_t pages) {
    const size_t kb = pages * PAGE_SIZE / 1024;
    const bool mb   = kb >= 1024;
    g_log.info("  {0}{1} {3} ({2} pages)", label, mb ? (kb / 1024) : kb, pages, mb ? "MiB" : "KiB");
}
}  // namespace

ktl::maybe<vm_paddr_t> page_frame_allocator::alloc() {
    // Fetch a zerored page, or make one if we don't have one available.
    return m_zeroed.pop()
        .or_else([this] {
            return pop_free_page().map([this](vm_paddr_t p) {
                zero_page(p);
                return p;
            });
        })
        .inspect([this](vm_paddr_t) { --m_free_pages; });
}

void page_frame_allocator::free(vm_paddr_t addr) {
    if (m_dirty.push(addr)) {
        ++m_free_pages;
    } else {
        g_log.error("pmm: Failed to free page at 0x{0:p}", addr);
    }
}

ktl::maybe<vm_paddr_t> page_frame_allocator::pop_free_page() {
    if (auto addr = m_dirty.pop()) { return addr; }

    while (!m_regions.empty()) {
        auto& region = m_regions[m_regions.size() - 1];
        if (region.count == 0) {
            m_regions.pop_back();
            continue;
        }
        return region.start + (--region.count) * PAGE_SIZE;
    }
    return ktl::nothing;
}

void page_frame_allocator::zero_page(vm_paddr_t addr) {
    memset(reinterpret_cast<void*>(addr + g_hhdm_offset), 0, PAGE_SIZE);
}

void page_frame_allocator::debug_print_state() {
    g_log.info("Page Frame Allocator State:");
    log_size("Total:    ", m_total_pages);
    log_size("Free:     ", m_free_pages);
    log_size("Used:     ", m_total_pages - m_free_pages);
    log_size("Reserved: ", m_reserved_pages);
    g_log.info("  Zeroed:   {0} pages", m_zeroed.size());
    g_log.info("  Dirty:    {0} pages", m_dirty.size());
    g_log.info("  Regions:  {0}", m_regions.size());
}

page_frame_allocator g_page_frame_allocator;

}  // namespace kernel::mm
