#include "kernel/panic.h"

#include "kernel/crash.h"

[[noreturn]]
void panic(const char* message) {
    kernel::crash::dispatch(kernel::crash::trigger_kind::panic, nullptr, message);
}
