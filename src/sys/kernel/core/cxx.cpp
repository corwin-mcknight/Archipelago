#include <stddef.h>

#include "kernel/log.h"
#include "kernel/panic.h"

typedef void initfunc_t(void);
extern initfunc_t *__init_array_start[], *__init_array_end[];

extern "C" void init_global_constructors_array(void) {
    for (initfunc_t **p = __init_array_start; p != __init_array_end; p++) (*p)();
}

// Define cxa_pure_virtual to prevent undefined reference errors

extern "C" [[noreturn]] void __cxa_pure_virtual() {
    // Log the call
    g_log.error("Pure virtual function called");
    // Halt the system
    asm("cli");
    while (1) {}
}

extern "C" void *__cxa_begin_catch(void *) {
    g_log.fatal("__cxa_begin_catch invoked");
    panic("unexpected __cxa_begin_catch");
    __builtin_unreachable();
}

extern "C" [[noreturn]] void __cxa_end_catch() {
    g_log.fatal("__cxa_end_catch invoked");
    panic("unexpected __cxa_end_catch");
}

namespace std {
[[noreturn]] void terminate() noexcept {
    g_log.fatal("std::terminate invoked");
    panic("std::terminate called");
}
}  // namespace std
