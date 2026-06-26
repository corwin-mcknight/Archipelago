#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/maybe>
#include <ktl/stack>
#include <ktl/vector>

#include "kernel/mm/page.h"

namespace kernel::mm {

class page_frame_allocator {
   public:
    ktl::maybe<vm_paddr_t> alloc();
    void free(vm_paddr_t addr);
    void debug_print_state();

    void add_region(const vm_page_region& region) {
        if (!m_regions.push_back(region)) { return; }  // drop region on OOM rather than corrupt page accounting
        m_free_pages += region.count;
        m_total_pages += region.count;
    }

    void add_reserved(size_t pages) {
        m_reserved_pages += pages;
        m_total_pages += pages;
    }

   private:
    size_t m_total_pages    = 0;
    size_t m_free_pages     = 0;
    size_t m_reserved_pages = 0;

    ktl::stack<vm_paddr_t> m_zeroed;
    ktl::stack<vm_paddr_t> m_dirty;
    ktl::vector<vm_page_region> m_regions;

    ktl::maybe<vm_paddr_t> pop_free_page();
    void zero_page(vm_paddr_t addr);
};

extern page_frame_allocator g_page_frame_allocator;

};  // namespace kernel::mm
