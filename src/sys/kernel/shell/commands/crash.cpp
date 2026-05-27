#include <kernel/config.h>

#if CONFIG_KERNEL_SHELL

#include <stdint.h>

#include <ktl/string_view>

#include "kernel/assert.h"
#include "kernel/panic.h"
#include "kernel/shell/shell.h"

namespace {

[[noreturn]] void trigger_pagefault() {
    volatile int* p = reinterpret_cast<int*>(0xdeadbeef);
    *p              = 0;
    for (;;) { asm volatile("hlt"); }
}

[[noreturn]] void trigger_invalid_opcode() {
    asm volatile("ud2");
    for (;;) { asm volatile("hlt"); }
}

[[noreturn]] void trigger_breakpoint() {
    asm volatile("int $3");
    for (;;) { asm volatile("hlt"); }
}

void crash_handler(int argc, const char* const argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: crash panic|assert|pagefault|invop|int3\n");
        return;
    }

    ktl::string_view sub(argv[1]);
    if (sub == "panic") {
        panic("crash command: induced panic");
    } else if (sub == "assert") {
        assert(false, "crash command: induced assertion");
    } else if (sub == "pagefault") {
        trigger_pagefault();
    } else if (sub == "invop") {
        trigger_invalid_opcode();
    } else if (sub == "int3") {
        trigger_breakpoint();
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(crash, "crash", "Deliberately crash the kernel (for testing)", crash_handler);

#endif  // CONFIG_KERNEL_SHELL
