#include <kernel/config.h>

#if CONFIG_KERNEL_TESTING

#include <stdint.h>

#include <ktl/span>
#include <ktl/string_view>

#include "kernel/symbols.h"
#include "kernel/testing/testing.h"

KTEST_MODULE("symbols");

namespace {

[[gnu::noinline]] int symbols_test_anchor_fn() {
    asm volatile("");
    return 0;
}

}  // namespace

// One lookup story over the anchor function: the table is available, an exact function start
// resolves at offset 0 with the right name, an interior address reports its offset, and
// addresses outside the kernel image are rejected.
KTEST_CASE(symbols_lookup_resolves_and_rejects) {
    KTEST_REQUIRE_TRUE(kernel::symbols::available());

    uintptr_t addr = reinterpret_cast<uintptr_t>(&symbols_test_anchor_fn);
    auto sym       = kernel::symbols::lookup(addr);

    KTEST_REQUIRE_TRUE(sym.has_value());
    KTEST_EXPECT_EQUAL(sym->offset, static_cast<size_t>(0));

    // The mangled name must contain the unmangled identifier as a substring.
    bool found          = false;
    const char needle[] = "symbols_test_anchor_fn";
    for (const char* p = sym->name; *p != '\0'; p++) {
        bool match = true;
        for (size_t k = 0; needle[k] != '\0'; k++) {
            if (p[k] == '\0' || p[k] != needle[k]) {
                match = false;
                break;
            }
        }
        if (match) {
            found = true;
            break;
        }
    }
    KTEST_EXPECT_TRUE(found);

    // An interior address resolves to the containing symbol with the right offset.
    auto interior = kernel::symbols::lookup(addr + 2);
    KTEST_REQUIRE_TRUE(interior.has_value());
    KTEST_EXPECT_EQUAL(interior->offset, static_cast<size_t>(2));

    // Userspace-shaped address: nothing there.
    KTEST_EXPECT_FALSE(kernel::symbols::lookup(0x1000).has_value());
}

// The supported mangled-name forms: nested namespaces, anonymous namespaces, and unscoped names.
KTEST_CASE(symbols_demangle_supported_forms) {
    char buf[128];
    KTEST_REQUIRE_TRUE(kernel::symbols::demangle("_ZN6kernel5shell10shell_mainEv", ktl::span(buf)));
    KTEST_EXPECT_TRUE(ktl::string_view(buf) == "kernel::shell::shell_main()");

    KTEST_REQUIRE_TRUE(kernel::symbols::demangle(
        "_ZN12_GLOBAL__N_112execute_testERN6kernel5shell11ShellOutputERNS0_7testing5ktestE", ktl::span(buf)));
    KTEST_EXPECT_TRUE(ktl::string_view(buf) == "(anonymous namespace)::execute_test()");

    KTEST_REQUIRE_TRUE(kernel::symbols::demangle("_Z9vsnprintfPcmPKc", ktl::span(buf)));
    KTEST_EXPECT_TRUE(ktl::string_view(buf) == "vsnprintf()");
}

// Unsupported inputs must fail gracefully rather than emit garbage.
KTEST_CASE(symbols_demangle_rejects_unsupported) {
    char buf[64];
    // Plain (non-mangled) symbols.
    KTEST_EXPECT_FALSE(kernel::symbols::demangle("_start", ktl::span(buf)));
    KTEST_EXPECT_FALSE(kernel::symbols::demangle("init_global_constructors_array", ktl::span(buf)));
    // Contains a destructor token (`D1`) we don't support; expect graceful fail.
    KTEST_EXPECT_FALSE(kernel::symbols::demangle("_ZN6kernelD1Ev", ktl::span(buf)));
}

#endif  // CONFIG_KERNEL_TESTING
