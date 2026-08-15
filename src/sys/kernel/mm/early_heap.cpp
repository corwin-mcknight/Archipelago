#include "kernel/mm/early_heap.h"

#include <stddef.h>
#include <stdint.h>

#include <ktl/bit>

#include "kernel/log.h"
#include "kernel/panic.h"
#include "kernel/synchronization/execution_context.h"

namespace kernel::mm {

struct early_heap_block {
    size_t size;  // Total size of the block, including this header
    early_heap_block* next;
    bool free;
    uintptr_t payload_base;  // Pointer returned to the caller when allocated
};

static_assert(ktl::is_power_of_two(size_t{alignof(early_heap_block)}),
              "early_heap_block alignment must be a power of two");

namespace {

constexpr size_t heap_alignment_fallback = alignof(max_align_t);

using ktl::align_up;

constexpr size_t kMinimumSplitSize = sizeof(early_heap_block) + heap_alignment_fallback;

}  // namespace

// This heap backs operator new, and the timer tick reaches it: waking a sleeper pushes onto the
// run queue, which grows a deque, which allocates. Any block-list walk interrupted mid-splice by
// that path returns a block already handed out or drops one entirely.
//
// ponytail: masking interrupts is mutual exclusion only while one core allocates. A spinlock is the
// upgrade, and it needs a lock whose construction is constant-initialised -- on_boot() runs before
// the global constructors, so a lock built by a constructor would be assembled after first use.
using heap_guard = kernel::synchronization::critical_irq_section;

void early_heap::on_boot(uintptr_t start, uintptr_t end) {
    if (start >= end) { panic("Invalid early heap range"); }

    uintptr_t aligned_start = align_up(start, alignof(early_heap_block));
    uintptr_t aligned_end   = ktl::align_down(end, alignof(early_heap_block));

    if (aligned_end <= aligned_start + sizeof(early_heap_block)) { panic("Early heap region too small"); }

    heap_start           = aligned_start;
    heap_end             = aligned_end;

    m_head               = reinterpret_cast<early_heap_block*>(heap_start);
    m_head->size         = heap_end - heap_start;
    m_head->next         = nullptr;
    m_head->free         = true;
    m_head->payload_base = 0;

    m_used_bytes         = 0;
    m_alloc_calls        = 0;
    m_free_calls         = 0;
}

early_heap_stats early_heap::stats() {
    heap_guard guard;
    early_heap_stats s{};
    s.alloc_calls = m_alloc_calls;
    s.free_calls  = m_free_calls;
    for (early_heap_block* block = m_head; block != nullptr; block = block->next) {
        ++s.blocks;
        size_t payload = block->size > sizeof(early_heap_block) ? block->size - sizeof(early_heap_block) : 0;
        if (block->free) {
            s.free_bytes += payload;
        } else {
            s.used_bytes += payload;
        }
    }
    return s;
}

void* early_heap::alloc(size_t size, size_t alignment) {
    heap_guard guard;
    if (size == 0) { return nullptr; }

    if (!ktl::is_power_of_two(alignment)) {
        g_log.error("Alignment must be a power of two");
        panic("Early heap allocation alignment error");
        return nullptr;
    }

    if (alignment < heap_alignment_fallback) { alignment = heap_alignment_fallback; }

    early_heap_block* block = m_head;

    while (block != nullptr) {
        if (!block->free) {
            block = block->next;
            continue;
        }

        uintptr_t block_addr = reinterpret_cast<uintptr_t>(block);
        uintptr_t payload    = align_up(block_addr + sizeof(early_heap_block), alignment);

        size_t prefix        = payload - block_addr;
        if (prefix > block->size) {
            block = block->next;
            continue;
        }

        size_t usable = block->size - prefix;
        if (usable < size) {
            block = block->next;
            continue;
        }

        size_t total_consumed    = prefix + size;
        size_t remaining         = block->size - total_consumed;
        uintptr_t new_block_addr = block_addr + total_consumed;

        if (remaining > 0) {
            uintptr_t aligned_new_block_addr = align_up(new_block_addr, alignof(early_heap_block));
            size_t alignment_gap             = aligned_new_block_addr - new_block_addr;
            if (alignment_gap > 0) {
                if (remaining <= alignment_gap) {
                    remaining = 0;
                } else {
                    new_block_addr = aligned_new_block_addr;
                    total_consumed += alignment_gap;
                    remaining -= alignment_gap;
                }
            }
        }

        if (remaining >= kMinimumSplitSize) {
            auto* new_block         = reinterpret_cast<early_heap_block*>(new_block_addr);
            new_block->size         = remaining;
            new_block->next         = block->next;
            new_block->free         = true;
            new_block->payload_base = 0;
            block->next             = new_block;
            block->size             = total_consumed;
        }

        block->free         = false;
        block->payload_base = payload;

        ++m_alloc_calls;
        m_used_bytes += block->size - sizeof(early_heap_block);

        return reinterpret_cast<void*>(payload);
    }

    g_log.error("Early heap out of memory (allocation of {0} bytes with alignment {1})", size, alignment);
    panic("Early heap out of memory");
    return nullptr;
}

void early_heap::free(void* ptr) {
    if (ptr == nullptr) { return; }
    heap_guard guard;

    uintptr_t target        = reinterpret_cast<uintptr_t>(ptr);

    early_heap_block* prev  = nullptr;
    early_heap_block* block = m_head;

    while (block != nullptr) {
        if (!block->free && block->payload_base == target) {
            ++m_free_calls;
            m_used_bytes -= block->size - sizeof(early_heap_block);
            block->free         = true;
            block->payload_base = 0;

            while (block->next != nullptr && block->next->free) {
                block->size += block->next->size;
                block->next = block->next->next;
            }

            if (prev != nullptr && prev->free) {
                prev->size += block->size;
                prev->next = block->next;
                block      = prev;

                while (block->next != nullptr && block->next->free) {
                    block->size += block->next->size;
                    block->next = block->next->next;
                }
            }

            return;
        }

        prev  = block;
        block = block->next;
    }

    g_log.error("Attempted to free unmanaged pointer 0x{0:p}", target);
    panic("Invalid pointer free on early heap");
}

}  // namespace kernel::mm

kernel::mm::early_heap g_early_heap;
