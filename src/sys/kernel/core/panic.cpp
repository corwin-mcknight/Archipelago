#include "kernel/panic.h"

#include "kernel/crash.h"

[[noreturn]] void hcf() {
    asm volatile("cli");
    for (;;) { asm volatile("hlt"); }
}

[[noreturn]]
void panic(const char* message) {
    kernel::crash::dispatch(kernel::crash::trigger_kind::panic, nullptr, message);
}
