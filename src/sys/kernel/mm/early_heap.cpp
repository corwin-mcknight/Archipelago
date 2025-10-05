#include "kernel/mm/early_heap.h"

#include <stdint.h>

#include "kernel/config.h"
#include "kernel/log.h"
#include "kernel/panic.h"

namespace kernel::mm {

void early_heap::on_boot(uintptr_t start, size_t end) {
    heap_start   = start;
    heap_end     = end;
    current_head = start;
    current_tail = end;
}

void early_heap::debug_print_state() {
    uintptr_t remaining        = current_tail - current_head;
    uintptr_t allocated_so_far = heap_end - heap_start - remaining;
    if (remaining > heap_end - heap_start) { remaining = 0; }

    g_log.debug("Early heap state:");
    g_log.debug("  Heap start:      0x{0:p}", heap_start);
    g_log.debug("  Heap end:        0x{0:p}", heap_end);
    g_log.debug("  Current head:    0x{0:p}", current_head);
    g_log.debug("  Current tail:    0x{0:p}", current_tail);
    g_log.debug("  Allocated:       {0} bytes ({1} KiB, {2} pages)", allocated_so_far, allocated_so_far / 1024,
                allocated_so_far / KERNEL_MINIMUM_PAGE_SIZE);
    g_log.debug("  Remaining size:  {0} bytes ({1} KiB, {2} pages)", remaining, remaining / 1024,
                remaining / KERNEL_MINIMUM_PAGE_SIZE);
}

void *early_heap::alloc_from_head(size_t size, size_t alignment) {
    if (alignment & (alignment - 1)) {
        g_log.error("Alignment must be a power of two");
        panic("Early heap allocation alignment error");
        return nullptr;
    }

    uintptr_t aligned_head = (current_head + alignment - 1) & ~(alignment - 1);
    if (aligned_head + size > current_tail) {
        g_log.error("Early heap out of memory (head allocation of {0} bytes with alignment {1})", size, alignment);
        panic("Early heap out of memory");
        return nullptr;
    }

    void *ptr    = reinterpret_cast<void *>(aligned_head);
    current_head = aligned_head + size;
    return ptr;
}
}  // namespace kernel::mm

kernel::mm::early_heap g_early_heap;