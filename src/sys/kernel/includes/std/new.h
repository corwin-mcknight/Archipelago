#pragma once

#include <stddef.h>

namespace std {
struct nothrow_t {
    explicit nothrow_t() = default;
};
extern const nothrow_t nothrow;
}  // namespace std

void* operator new(size_t size);
void* operator new[](size_t size);
void* operator new(size_t, void* ptr) noexcept;
void* operator new[](size_t, void* ptr) noexcept;
// Nothrow forms: may return nullptr on allocation failure. Unlike the ordinary forms above,
// the compiler must preserve caller-side null checks on these (used by ktl::make_ref, F032).
void* operator new(size_t size, const std::nothrow_t&) noexcept;
void* operator new[](size_t size, const std::nothrow_t&) noexcept;
void operator delete(void* ptr, const std::nothrow_t&) noexcept;
void operator delete[](void* ptr, const std::nothrow_t&) noexcept;
void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete(void* ptr, size_t) noexcept;
void operator delete[](void* ptr, size_t) noexcept;
