#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/maybe>
#include <ktl/vector>

#include "kernel/mm/page.h"
#include "kernel/synchronization/spinlock.h"

namespace kernel::mm {

// Consistent point-in-time view of the allocator, taken under its lock.
struct pmm_stats {
    size_t total_pages;
    size_t free_pages;
    size_t reserved_pages;
    size_t zeroed_pooled;
    size_t zeroed_region_tail;
    size_t dirty;
    size_t regions;
    size_t low_water;  // lowest free_pages ever observed
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t contig_count;
    uint64_t alloc_failures;
    uint64_t zeroer_pages;  // pages zeroed by zero_one_page over all time
};

class page_frame_allocator {
   public:
    ktl::maybe<vm_paddr_t> alloc();
    void free(vm_paddr_t addr);
    // Carve a physically contiguous, zeroed run of pages from an untouched
    // region tail. No free_contiguous: boot-lifetime callers (the page
    // descriptor array) never free, and the heap's multi-page runs return
    // through free() page by page, at the documented tail-depletion cost.
    ktl::maybe<vm_paddr_t> alloc_contiguous(size_t count);
    // Zero one free page; false when there is nothing left to do. Dirty pages
    // always drain into the zeroed pool; region tail pages are zeroed in place
    // (tracked by a per-region count, not pool entries) until all free memory
    // is zeroed. The zeroer thread's work loop; safe to race with alloc/free.
    bool zero_one_page();

    pmm_stats stats();

    // In-order visit of the remaining regions (shell observability). Runs
    // under the lock; fn must not allocate or re-enter the allocator.
    template <typename F> void for_each_region(F&& fn) {
        kernel::synchronization::critical_irq_lock_guard guard(m_lock);
        for (size_t i = 0; i < m_regions.size(); ++i) { fn(m_regions[i]); }
    }

    size_t free_pages() const { return m_free_pages; }
    // Total pages ready to serve without a memset: pooled plus region tails.
    size_t zeroed_pages() const { return m_zeroed.size() + m_region_zeroed; }

    void add_region(const vm_page_region& region) {
        if (!m_regions.push_back(region)) { return; }  // drop region on OOM rather than corrupt page accounting
        m_free_pages += region.count;
        m_total_pages += region.count;
        m_low_water = m_free_pages;
    }

    void add_reserved(size_t pages) {
        m_reserved_pages += pages;
        m_total_pages += pages;
    }

   private:
    // Guards the pools and counters: the zeroer thread mutates m_dirty/m_zeroed
    // concurrently with allocating threads.
    kernel::synchronization::spinlock m_lock;

    size_t m_total_pages      = 0;
    size_t m_free_pages       = 0;
    size_t m_reserved_pages   = 0;
    size_t m_region_zeroed    = 0;  // sum of all regions' zeroed_count
    size_t m_low_water        = 0;

    uint64_t m_alloc_count    = 0;
    uint64_t m_free_count     = 0;
    uint64_t m_contig_count   = 0;
    uint64_t m_alloc_failures = 0;
    uint64_t m_zeroer_pages   = 0;

    // The page pools are intrusive: each free frame's first word holds the next frame's address,
    // written and read through the physmap, so the pools own no storage and pushing can never
    // fail or allocate. That last property is load-bearing -- the PMM sits below the heap, and a
    // pool that grew through operator new would re-enter the PMM inside its own lock (the exact
    // deadlock the slab-heap switchover exposed). pop() re-zeroes the link word on the way out,
    // which is what keeps the zeroed pool's all-zero guarantee intact.
    class frame_stack {
       public:
        void push(vm_paddr_t addr);
        ktl::maybe<vm_paddr_t> pop();
        size_t size() const { return m_count; }

       private:
        // Physical 0 is a valid frame address in principle, so empty is a sentinel, not zero.
        static constexpr vm_paddr_t NIL = ~static_cast<vm_paddr_t>(0);
        vm_paddr_t m_head               = NIL;
        size_t m_count                  = 0;
    };

    frame_stack m_zeroed;
    frame_stack m_dirty;
    ktl::vector<vm_page_region> m_regions;

    // Sets pre_zeroed when the popped page needs no memset (a pre-zeroed
    // region tail page).
    ktl::maybe<vm_paddr_t> pop_free_page(bool& pre_zeroed);
    void zero_page(vm_paddr_t addr);
};

extern page_frame_allocator g_page_frame_allocator;

};  // namespace kernel::mm
