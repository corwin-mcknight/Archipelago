#include "kernel/log.h"

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