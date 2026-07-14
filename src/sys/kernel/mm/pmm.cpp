#include "kernel/mm/pmm.h"

#include <std/string.h>

#include "kernel/config.h"
#include "kernel/log.h"
#include "kernel/mm/page.h"
#include "kernel/mm/page_descriptor.h"

extern uintptr_t g_hhdm_offset;

namespace kernel::mm {

namespace { constexpr size_t PAGE_SIZE = KERNEL_MINIMUM_PAGE_SIZE; }  // namespace

ktl::maybe<vm_paddr_t> page_frame_allocator::alloc() {
    kernel::synchronization::critical_irq_lock_guard guard(m_lock);
    // Fetch a zeroed page: from the pool, a pre-zeroed region tail, or by
    // zeroing a free page inline as the last resort.
    auto page = m_zeroed.pop()
                    .or_else([this] {
                        bool pre_zeroed = false;
                        return pop_free_page(pre_zeroed).map([&](vm_paddr_t p) {
                            if (!pre_zeroed) { zero_page(p); }
                            return p;
                        });
                    })
                    .inspect([this](vm_paddr_t p) {
                        --m_free_pages;
                        ++m_alloc_count;
                        if (m_free_pages < m_low_water) { m_low_water = m_free_pages; }
                        g_page_descriptors.set_state(p, page_state::ACTIVE);
                    });
    if (!page.has_value()) { ++m_alloc_failures; }
    return page;
}

void page_frame_allocator::free(vm_paddr_t addr) {
    kernel::synchronization::critical_irq_lock_guard guard(m_lock);
    if (m_dirty.push(addr)) {
        ++m_free_pages;
        ++m_free_count;
        g_page_descriptors.set_state(addr, page_state::FREE);
    } else {
        g_log.error("pmm: Failed to free page at 0x{0:p}", addr);
    }
}

ktl::maybe<vm_paddr_t> page_frame_allocator::alloc_contiguous(size_t count) {
    kernel::synchronization::critical_irq_lock_guard guard(m_lock);
    for (size_t i = m_regions.size(); i-- > 0;) {
        auto& region = m_regions[i];
        if (region.count < count) { continue; }
        region.count -= count;
        vm_paddr_t base = region.start + region.count * PAGE_SIZE;
        // The carved run's high end may overlap the pre-zeroed tail; only the
        // low pages still need a memset.
        size_t pre      = region.zeroed_count < count ? region.zeroed_count : count;
        region.zeroed_count -= pre;
        m_region_zeroed -= pre;
        for (size_t p = 0; p < count - pre; ++p) { zero_page(base + p * PAGE_SIZE); }
        for (size_t p = 0; p < count; ++p) { g_page_descriptors.set_state(base + p * PAGE_SIZE, page_state::ACTIVE); }
        m_free_pages -= count;
        ++m_contig_count;
        if (m_free_pages < m_low_water) { m_low_water = m_free_pages; }
        return base;
    }
    ++m_alloc_failures;
    return ktl::nothing;
}

ktl::maybe<vm_paddr_t> page_frame_allocator::pop_free_page(bool& pre_zeroed) {
    pre_zeroed = false;
    if (auto addr = m_dirty.pop()) { return addr; }

    while (!m_regions.empty()) {
        auto& region = m_regions[m_regions.size() - 1];
        if (region.count == 0) {
            (void)m_regions.pop_back();
            continue;
        }
        if (region.zeroed_count > 0) {
            --region.zeroed_count;
            --m_region_zeroed;
            pre_zeroed = true;
        }
        return region.start + (--region.count) * PAGE_SIZE;
    }
    return ktl::nothing;
}

void page_frame_allocator::zero_page(vm_paddr_t addr) {
    memset(reinterpret_cast<void*>(addr + g_hhdm_offset), 0, PAGE_SIZE);
}

bool page_frame_allocator::zero_one_page() {
    kernel::synchronization::critical_irq_lock_guard guard(m_lock);
    // Zeroing under the lock keeps every page in exactly one place at all
    // times, so allocators never observe an in-flight frame.

    // Dirty pages always drain into the pool: they are pages already
    // circulating, so the pool never grows past what free() handed back.
    if (auto page = m_dirty.pop()) {
        zero_page(page.value());
        ++m_zeroer_pages;
        if (m_zeroed.push(page.value())) {
            g_page_descriptors.set_state(page.value(), page_state::ZEROED);
        } else if (!m_dirty.push(page.value())) {
            g_log.error("pmm: Failed to repool zeroed page at 0x{0:p}", page.value());
            --m_free_pages;  // the page is leaked; keep the count honest
        }
        return true;
    }

    // Pre-zero untouched region tails in place -- a counter per region, no
    // pool entries -- until all free memory is zeroed.
    if (zeroed_pages() >= m_free_pages) { return false; }
    for (size_t i = m_regions.size(); i-- > 0;) {
        auto& region = m_regions[i];
        if (region.zeroed_count == region.count) { continue; }
        vm_paddr_t addr = region.start + (region.count - region.zeroed_count - 1) * PAGE_SIZE;
        zero_page(addr);
        ++m_zeroer_pages;
        ++region.zeroed_count;
        ++m_region_zeroed;
        g_page_descriptors.set_state(addr, page_state::ZEROED);
        return true;
    }
    return false;
}

pmm_stats page_frame_allocator::stats() {
    kernel::synchronization::critical_irq_lock_guard guard(m_lock);
    return pmm_stats{
        .total_pages        = m_total_pages,
        .free_pages         = m_free_pages,
        .reserved_pages     = m_reserved_pages,
        .zeroed_pooled      = m_zeroed.size(),
        .zeroed_region_tail = m_region_zeroed,
        .dirty              = m_dirty.size(),
        .regions            = m_regions.size(),
        .low_water          = m_low_water,
        .alloc_count        = m_alloc_count,
        .free_count         = m_free_count,
        .contig_count       = m_contig_count,
        .alloc_failures     = m_alloc_failures,
        .zeroer_pages       = m_zeroer_pages,
    };
}

page_frame_allocator g_page_frame_allocator;

}  // namespace kernel::mm
