#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/maybe>
#include <ktl/stack>

#include "kernel/mm/page.h"
#include "kernel/mm/pmm.h"

namespace kernel::mm {

class page_frame_allocator {
   public:
    ktl::stack<vm_paddr_t> m_active;
    ktl::stack<vm_paddr_t> m_zeroed;
    ktl::stack<vm_paddr_t> m_free;
    ktl::stack<vm_paddr_t> m_dirty;

    ktl::maybe<vm_paddr_t> alloc(vm_page_state state = vm_page_state::ACTIVE);
    void free(vm_paddr_t addr);
    void debug_print_state();

    void add_region(const vm_page_region& region) {
        for (size_t i = 0; i < region.count; i++) {
            vm_paddr_t addr = region.start + i * 0x1000;
            if (!m_free.push(addr)) { break; }
            ++m_free_pages;
            ++m_total_pages;
        }
    }

   private:
    size_t m_total_pages = 0;
    size_t m_free_pages  = 0;

    void zero_page(vm_paddr_t addr);
    void zero_more_pages(size_t count);
    void update_dirty_pages();

    ktl::stack<vm_paddr_t>* stack_for_state(vm_page_state state) {
        switch (state) {
            case vm_page_state::FREE: return &m_free;
            case vm_page_state::ACTIVE: return &m_active;
            case vm_page_state::ZEROED: return &m_zeroed;
            default: return nullptr;
        }
    }
};

extern page_frame_allocator g_page_frame_allocator;

};  // namespace kernel::mm