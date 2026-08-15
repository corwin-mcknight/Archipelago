#pragma once
#include <stddef.h>
#include <stdint.h>

namespace kernel::mm {

struct early_heap_block;

// Point-in-time view of the heap; used/free come from a block walk.
struct early_heap_stats {
    size_t blocks;
    size_t used_bytes;
    size_t free_bytes;
    uint64_t alloc_calls;
    uint64_t free_calls;
};

class early_heap {
   public:
    void on_boot(uintptr_t start, uintptr_t end);

    void* alloc(size_t size, size_t alignment = 1);
    void free(void* ptr);

    // Whether ptr came from this heap's region -- how operator delete routes frees between the
    // early heap and the slab heap after the switchover.
    bool contains(const void* ptr) const {
        uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
        return address >= heap_start && address < heap_end;
    }

    early_heap_stats stats();

    template <typename T> T* alloc_object() { return static_cast<T*>(alloc(sizeof(T), alignof(T))); }

   private:
    early_heap_block* m_head;
    uintptr_t heap_start;
    uintptr_t heap_end;
    size_t m_used_bytes    = 0;
    uint64_t m_alloc_calls = 0;
    uint64_t m_free_calls  = 0;
};
}  // namespace kernel::mm

extern kernel::mm::early_heap g_early_heap;
