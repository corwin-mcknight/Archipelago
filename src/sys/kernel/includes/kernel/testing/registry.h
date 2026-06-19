#pragma once

#include <stddef.h>

// Minimal cross-tier test-registration ABI: the ktest record, its flags, and the backend hooks (abort
// plus the assertion sink). This is the contract shared by every test and both backends -- the kernel
// shell on the QEMU tier and the host fork runner. The host harness depends only on this header, so it
// is not dragged through the assertion macros or the formatter (which need the kernel <std> include
// path and cannot compile in the host harness translation unit).

namespace kernel::testing {

using test_fn = void (*)();

enum : unsigned {
    KTEST_FLAG_NONE               = 0u,
    KTEST_FLAG_REQUIRES_CLEAN_ENV = 1u << 0,
    KTEST_FLAG_EXPECTS_CRASH      = 1u << 1,
};

struct alignas(alignof(void*)) ktest {
    const char* name;
    const char* submodule;
    unsigned flags;
    test_fn init_fn;
    test_fn fn;
};

[[noreturn]]
void abort(unsigned char exit_code = 1);

// Expression-capturing assertion sink (see <kernel/testing/expect.h>). Defined by the active backend:
// the kernel shell on the QEMU tier, the fork runner on the host tier.
void report_assertion(bool passed, bool fatal, const char* file, int line, const char* expr_text, const char* lhs,
                      const char* op, const char* rhs);

}  // namespace kernel::testing
