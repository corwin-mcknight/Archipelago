#pragma once
#include <stddef.h>
#include <stdint.h>

namespace kernel::mm {
class early_heap {
   public:
    void on_boot(uintptr_t start, size_t size);
    void debug_print_state();

    void *alloc_from_head(size_t size, size_t alignment = 1);
    void *alloc_from_tail(size_t size, size_t alignment = 1);
    void *alloc_page(size_t count);

    template <typename T> T *alloc_object() { return static_cast<T *>(alloc_from_head(sizeof(T), alignof(T))); }

   private:
    uintptr_t heap_start;
    uintptr_t heap_end;
    uintptr_t current_head;
    uintptr_t current_tail;
};
}  // namespace kernel::mm

extern kernel::mm::early_heap g_early_heap;