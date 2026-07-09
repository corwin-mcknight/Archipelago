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
    if (ptr == nullptr) { return; }  // deleting null is a well-defined no-op
    g_early_heap.free(ptr);
}

void operator delete[](void* ptr) noexcept { operator delete(ptr); }

void operator delete(void* ptr, size_t) noexcept { operator delete(ptr); }

void operator delete[](void* ptr, size_t) noexcept { operator delete(ptr); }

void operator delete(void* ptr, const std::nothrow_t&) noexcept { operator delete(ptr); }

void operator delete[](void* ptr, const std::nothrow_t&) noexcept { operator delete(ptr); }

// Aligned forms: required whenever a heap object's alignment exceeds
// __STDCPP_DEFAULT_NEW_ALIGNMENT__ (e.g. a cache-line-aligned spinlock member). early_heap::alloc
// already takes an alignment, so these just forward it through.
void* operator new(size_t size, std::align_val_t align) { return g_early_heap.alloc(size, static_cast<size_t>(align)); }

void* operator new[](size_t size, std::align_val_t align) { return operator new(size, align); }

void* operator new(size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return g_early_heap.alloc(size, static_cast<size_t>(align));
}

void* operator new[](size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return operator new(size, align, std::nothrow);
}

void operator delete(void* ptr, std::align_val_t) noexcept { operator delete(ptr); }

void operator delete[](void* ptr, std::align_val_t) noexcept { operator delete(ptr); }

void operator delete(void* ptr, size_t, std::align_val_t) noexcept { operator delete(ptr); }

void operator delete[](void* ptr, size_t, std::align_val_t) noexcept { operator delete(ptr); }

void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { operator delete(ptr); }

void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { operator delete(ptr); }
