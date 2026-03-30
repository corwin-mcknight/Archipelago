#pragma once
#include <stddef.h>
#include <stdint.h>

namespace kernel::mm {

struct early_heap_block;

class early_heap {
   public:
    void on_boot(uintptr_t start, uintptr_t end);
    void debug_print_state();

    void* alloc(size_t size, size_t alignment = 1);
    void free(void* ptr);

    template <typename T> T* alloc_object() { return static_cast<T*>(alloc(sizeof(T), alignof(T))); }

   private:
    early_heap_block* m_head;
    uintptr_t heap_start;
    uintptr_t heap_end;
};
}  // namespace kernel::mm

extern kernel::mm::early_heap g_early_heap;
