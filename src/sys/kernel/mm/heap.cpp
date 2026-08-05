#include "kernel/mm/early_heap.h"
#include "kernel/mm/slab_heap.h"
#include "kernel/panic.h"
#include "std/new.h"

namespace std { const nothrow_t nothrow{}; }  // namespace std

// Which backend operator new uses. The early heap serves the window before the PMM exists (its
// alloc panics on exhaustion -- nothing in that window can continue without memory anyway); once
// init_memory calls heap_activate, allocation moves to the slab heap and failure becomes a value.
// Early-heap pointers freed after the flip are recognized by address range.
namespace {
bool g_slab_active = false;

void* heap_alloc(size_t size, size_t align) {
    if (size == 0) { size = 1; }  // both backends owe a unique pointer for a zero-size request
    if (g_slab_active) { return kernel::mm::g_slab_heap.alloc(size, align); }
    return g_early_heap.alloc(size, align);
}

void heap_free(void* ptr) {
    if (g_early_heap.contains(ptr)) {
        g_early_heap.free(ptr);
        return;
    }
    kernel::mm::g_slab_heap.free(ptr);
}
}  // namespace

namespace kernel::mm {
void heap_activate() { g_slab_active = true; }
bool heap_slab_active() { return g_slab_active; }
}  // namespace kernel::mm

// The ordinary forms keep the panic-on-failure contract: callers that can handle exhaustion use
// the nothrow forms (ktl::make_ref does), so a null here means an unprepared caller.
void* operator new(size_t size) {
    void* ptr = heap_alloc(size, sizeof(void*));
    if (ptr == nullptr) { panic("operator new: out of memory"); }
    return ptr;
}
void* operator new[](size_t size) { return operator new(size); }
void* operator new(size_t, void* ptr) noexcept { return ptr; }
void* operator new[](size_t, void* ptr) noexcept { return ptr; }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return heap_alloc(size, sizeof(void*)); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return heap_alloc(size, sizeof(void*)); }

void operator delete(void* ptr) noexcept {
    if (ptr == nullptr) { return; }  // deleting null is a well-defined no-op
    heap_free(ptr);
}

void operator delete[](void* ptr) noexcept { operator delete(ptr); }

void operator delete(void* ptr, size_t) noexcept { operator delete(ptr); }

void operator delete[](void* ptr, size_t) noexcept { operator delete(ptr); }

void operator delete(void* ptr, const std::nothrow_t&) noexcept { operator delete(ptr); }

void operator delete[](void* ptr, const std::nothrow_t&) noexcept { operator delete(ptr); }

// Aligned forms: required whenever a heap object's alignment exceeds
// __STDCPP_DEFAULT_NEW_ALIGNMENT__ (e.g. a cache-line-aligned spinlock member).
void* operator new(size_t size, std::align_val_t align) {
    void* ptr = heap_alloc(size, static_cast<size_t>(align));
    if (ptr == nullptr) { panic("operator new: out of memory"); }
    return ptr;
}

void* operator new[](size_t size, std::align_val_t align) { return operator new(size, align); }

void* operator new(size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    // The nothrow contract is nullptr on failure, and an alignment the heap cannot deliver is a
    // failure -- only the ordinary forms may treat it as a caller bug and panic.
    if (static_cast<size_t>(align) > kernel::mm::slab_heap::MAX_ALIGN) { return nullptr; }
    return heap_alloc(size, static_cast<size_t>(align));
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
