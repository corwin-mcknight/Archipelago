#include <kernel/config.h>

#if CONFIG_KERNEL_TESTING

#include <stdint.h>

#include <ktl/span>
#include <ktl/string_view>

#include "kernel/symbols.h"
#include "kernel/testing/testing.h"

namespace {

[[gnu::noinline]] int symbols_test_anchor_fn() {
    asm volatile("");
    return 0;
}

}  // namespace

KTEST(symbols_available, "symbols") { KTEST_EXPECT_TRUE(kernel::symbols::available()); }

KTEST(symbols_resolve_self, "symbols") {
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
}

KTEST(symbols_resolve_interior_offset, "symbols") {
    KTEST_REQUIRE_TRUE(kernel::symbols::available());

    uintptr_t base = reinterpret_cast<uintptr_t>(&symbols_test_anchor_fn);
    auto sym       = kernel::symbols::lookup(base + 2);

    KTEST_REQUIRE_TRUE(sym.has_value());
    KTEST_EXPECT_EQUAL(sym->offset, static_cast<size_t>(2));
}

KTEST(symbols_lookup_garbage_address, "symbols") {
    KTEST_REQUIRE_TRUE(kernel::symbols::available());

    // Userspace-shaped address: nothing there.
    auto sym = kernel::symbols::lookup(0x1000);
    KTEST_EXPECT_FALSE(sym.has_value());
}

KTEST(symbols_demangle_nested, "symbols") {
    char buf[128];
    KTEST_REQUIRE_TRUE(kernel::symbols::demangle("_ZN6kernel5shell10shell_mainEv", ktl::span(buf)));
    KTEST_EXPECT_TRUE(ktl::string_view(buf) == "kernel::shell::shell_main()");
}

KTEST(symbols_demangle_anonymous_namespace, "symbols") {
    char buf[128];
    KTEST_REQUIRE_TRUE(kernel::symbols::demangle(
        "_ZN12_GLOBAL__N_112execute_testERN6kernel5shell11ShellOutputERNS0_7testing5ktestE", ktl::span(buf)));
    KTEST_EXPECT_TRUE(ktl::string_view(buf) == "(anonymous namespace)::execute_test()");
}

KTEST(symbols_demangle_unscoped, "symbols") {
    char buf[64];
    KTEST_REQUIRE_TRUE(kernel::symbols::demangle("_Z9vsnprintfPcmPKc", ktl::span(buf)));
    KTEST_EXPECT_TRUE(ktl::string_view(buf) == "vsnprintf()");
}

KTEST(symbols_demangle_rejects_non_mangled, "symbols") {
    char buf[64];
    KTEST_EXPECT_FALSE(kernel::symbols::demangle("_start", ktl::span(buf)));
    KTEST_EXPECT_FALSE(kernel::symbols::demangle("init_global_constructors_array", ktl::span(buf)));
}

KTEST(symbols_demangle_rejects_complex, "symbols") {
    char buf[64];
    // Contains a destructor token (`D1`) we don't support; expect graceful fail.
    KTEST_EXPECT_FALSE(kernel::symbols::demangle("_ZN6kernelD1Ev", ktl::span(buf)));
}

#endif  // CONFIG_KERNEL_TESTING
