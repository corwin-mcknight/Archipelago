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

void page_frame_allocator::zero_page(vm_paddr_t addr) {
    memset(reinterpret_cast<void*>(addr + g_hhdm_offset), 0, 0x1000);
}

void page_frame_allocator::update_dirty_pages() {
    while (!m_dirty.empty()) {
        auto addr = m_dirty.pop();
        if (!addr.has_value()) { break; }
        zero_page(addr.value());
        if (!m_free.push(addr.value())) { break; }
    }
}

void page_frame_allocator::zero_more_pages(size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (m_free.empty()) { break; }
        auto addr = m_free.pop();
        if (!addr.has_value()) { break; }
        zero_page(addr.value());
        if (!m_zeroed.push(addr.value())) { break; }
    }
}

void page_frame_allocator::debug_print_state() {
    g_log.info("Page Frame Allocator State:");
    g_log.info("  Free pages:   {0}", m_free_pages);
    g_log.info("  Free stack:   {0} pages", m_free.size());
    g_log.info("  Zeroed stack: {0} pages", m_zeroed.size());
    g_log.info("  Active stack: {0} pages", m_active.size());
    g_log.info("  Dirty stack:  {0} pages", m_dirty.size());
}

page_frame_allocator g_page_frame_allocator;

}  // namespace kernel::mm