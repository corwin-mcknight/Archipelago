#pragma once

#include <stddef.h>

namespace std {
struct nothrow_t {
    explicit nothrow_t() = default;
};
extern const nothrow_t nothrow;

// Standard-mandated tag for the alignment-aware allocation/deallocation overloads. Any heap
// object whose alignment exceeds __STDCPP_DEFAULT_NEW_ALIGNMENT__ (e.g. a cache-line-aligned
// spinlock member) binds to these at the delete-expression regardless of how it was allocated,
// so the overloads must exist even though nothing here calls them directly.
enum class align_val_t : size_t {};
}  // namespace std

void* operator new(size_t size);
void* operator new[](size_t size);
void* operator new(size_t, void* ptr) noexcept;
void* operator new[](size_t, void* ptr) noexcept;
// Nothrow forms: may return nullptr on allocation failure. Unlike the ordinary forms above,
// the compiler must preserve caller-side null checks on these (used by ktl::make_ref).
void* operator new(size_t size, const std::nothrow_t&) noexcept;
void* operator new[](size_t size, const std::nothrow_t&) noexcept;
void operator delete(void* ptr, const std::nothrow_t&) noexcept;
void operator delete[](void* ptr, const std::nothrow_t&) noexcept;
void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete(void* ptr, size_t) noexcept;
void operator delete[](void* ptr, size_t) noexcept;

// Aligned forms.
void* operator new(size_t size, std::align_val_t align);
void* operator new[](size_t size, std::align_val_t align);
void* operator new(size_t size, std::align_val_t align, const std::nothrow_t&) noexcept;
void* operator new[](size_t size, std::align_val_t align, const std::nothrow_t&) noexcept;
void operator delete(void* ptr, std::align_val_t) noexcept;
void operator delete[](void* ptr, std::align_val_t) noexcept;
void operator delete(void* ptr, size_t, std::align_val_t) noexcept;
void operator delete[](void* ptr, size_t, std::align_val_t) noexcept;
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept;
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept;
