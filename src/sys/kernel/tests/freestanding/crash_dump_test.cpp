#include <kernel/config.h>

#if CONFIG_KERNEL_TESTING

#include "kernel/arch.h"
#include "kernel/panic.h"
#include "kernel/testing/testing.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-noreturn"

// These tests deliberately crash the kernel to exercise the crash dump
// pipeline. Marked KTEST_CASE_CRASH: the harness inverts the outcome
// (a crash is pass; a clean exit is fail). One VM death per test.

KTEST_MODULE("crash");

KTEST_CASE_CRASH(crash_dump_panic) { panic("crash_dump_panic: induced panic"); }

KTEST_CASE_CRASH(crash_dump_pagefault) {
    // Non-canonical address: bits 48..63 don't sign-extend bit 47.
    // Guarantees #GP regardless of paging (Limine identity-maps low memory).
    volatile int* p = reinterpret_cast<int*>(0xdead'beef'dead'beefULL);
    *p              = 0;
}

KTEST_CASE_CRASH(crash_dump_invalid_opcode) { kernel::arch::trigger_invalid_opcode(); }

#pragma clang diagnostic pop

#endif  // CONFIG_KERNEL_TESTING
