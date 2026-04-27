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
    ktl::stack<vm_paddr_t> m_active;
    ktl::stack<vm_paddr_t> m_zeroed;
    ktl::stack<vm_paddr_t> m_dirty;

    ktl::maybe<vm_paddr_t> alloc(vm_page_state state = vm_page_state::ACTIVE);
    void free(vm_paddr_t addr);
    void debug_print_state();

    void add_region(const vm_page_region& region) {
        m_regions.push_back(region);
        m_free_pages += region.count;
        m_total_pages += region.count;
    }

    void add_wired(size_t pages) {
        m_wired_pages += pages;
        m_total_pages += pages;
    }

   private:
    size_t m_total_pages = 0;
    size_t m_free_pages  = 0;
    size_t m_wired_pages = 0;

    // Regions with remaining pages to hand out. Pages are popped from the back.
    ktl::vector<vm_page_region> m_regions;

    ktl::maybe<vm_paddr_t> pop_free_page();
    void zero_page(vm_paddr_t addr);
    void zero_more_pages(size_t count);
    void update_dirty_pages();

    ktl::stack<vm_paddr_t>* stack_for_state(vm_page_state state) {
        switch (state) {
            case vm_page_state::ACTIVE: return &m_active;
            case vm_page_state::ZEROED: return &m_zeroed;
            default: return nullptr;
        }
    }
};

extern page_frame_allocator g_page_frame_allocator;

};  // namespace kernel::mm