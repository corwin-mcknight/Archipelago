
#include "kernel/log.h"
#include "kernel/mm/early_heap.h"
#include "kernel/panic.h"

void *operator new(size_t size) { return g_early_heap.alloc_from_head(size, sizeof(void *)); }
void *operator new[](size_t size) { return g_early_heap.alloc_from_head(size, sizeof(void *)); }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-noreturn"

void operator delete(void *) noexcept {
    g_log.fatal("operator delete invoked");
    panic("operator delete is unsupported");
    __builtin_unreachable();
}

void operator delete[](void *) noexcept {
    g_log.fatal("operator delete[] invoked");
    panic("operator delete[] is unsupported");
    __builtin_unreachable();
}

void operator delete(void *, size_t) noexcept {
    g_log.fatal("sized operator delete invoked");
    panic("sized operator delete is unsupported");
    __builtin_unreachable();
}

void operator delete[](void *, size_t) noexcept {
    g_log.fatal("sized operator delete[] invoked");
    panic("sized operator delete[] is unsupported");
    __builtin_unreachable();
}

#pragma GCC diagnostic pop