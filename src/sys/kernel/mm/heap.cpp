#include "kernel/log.h"
#include "kernel/mm/early_heap.h"
#include "std/new.h"

namespace std { const nothrow_t nothrow{}; }  // namespace std

void* operator new(size_t size) { return g_early_heap.alloc(size, sizeof(void*)); }
void* operator new[](size_t size) { return g_early_heap.alloc(size, sizeof(void*)); }
void* operator new(size_t, void* ptr) noexcept { return ptr; }
void* operator new[](size_t, void* ptr) noexcept { return ptr; }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return g_early_heap.alloc(size, sizeof(void*)); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return g_early_heap.alloc(size, sizeof(void*)); }

void operator delete(void* ptr) noexcept {
    if (ptr == nullptr) {
        g_log.error("Attemped to free a nullptr");
        return;
    }
    g_early_heap.free(ptr);
}

void operator delete[](void* ptr) noexcept { operator delete(ptr); }

void operator delete(void* ptr, size_t) noexcept { operator delete(ptr); }

void operator delete[](void* ptr, size_t) noexcept { operator delete(ptr); }

void operator delete(void* ptr, const std::nothrow_t&) noexcept { operator delete(ptr); }

void operator delete[](void* ptr, const std::nothrow_t&) noexcept { operator delete(ptr); }
