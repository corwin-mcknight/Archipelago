#pragma once

#include <stddef.h>

void* operator new(size_t size);
void* operator new[](size_t size);
void* operator new(size_t, void* ptr) noexcept;
void* operator new[](size_t, void* ptr) noexcept;
void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete(void* ptr, size_t) noexcept;
void operator delete[](void* ptr, size_t) noexcept;
