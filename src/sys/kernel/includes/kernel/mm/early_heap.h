#pragma once
#include <stddef.h>
#include <stdint.h>

namespace kernel::mm {

struct early_heap_block;

// Point-in-time view of the heap; used/free/largest come from a block walk.
struct early_heap_stats {
    size_t blocks;
    size_t used_bytes;
    size_t free_bytes;
    size_t largest_free;
    size_t peak_used;
    uint64_t alloc_calls;
    uint64_t free_calls;
    uintptr_t start;
    uintptr_t end;
};

class early_heap {
   public:
    void on_boot(uintptr_t start, uintptr_t end);
    void debug_print_state();

    void* alloc(size_t size, size_t alignment = 1);
    void free(void* ptr);

    early_heap_stats stats();
    // Visits blocks in address order as (ctx, payload_bytes, is_free); the
    // block type stays private to the implementation.
    void for_each_block(void (*fn)(void* ctx, size_t payload_bytes, bool is_free), void* ctx);

    template <typename T> T* alloc_object() { return static_cast<T*>(alloc(sizeof(T), alignof(T))); }

   private:
    early_heap_block* m_head;
    uintptr_t heap_start;
    uintptr_t heap_end;
    size_t m_used_bytes    = 0;
    size_t m_peak_used     = 0;
    uint64_t m_alloc_calls = 0;
    uint64_t m_free_calls  = 0;
};
}  // namespace kernel::mm

extern kernel::mm::early_heap g_early_heap;
