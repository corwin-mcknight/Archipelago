#include <kernel/config.h>

#if CONFIG_KERNEL_TESTING

#include <stdint.h>

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

    uintptr_t addr   = reinterpret_cast<uintptr_t>(&symbols_test_anchor_fn);
    size_t off       = 12345;
    const char* name = kernel::symbols::lookup(addr, &off);

    KTEST_REQUIRE_TRUE(name != nullptr);
    KTEST_EXPECT_EQUAL(off, static_cast<size_t>(0));

    // The mangled name must contain the unmangled identifier as a substring.
    bool found          = false;
    const char needle[] = "symbols_test_anchor_fn";
    for (const char* p = name; *p != '\0'; p++) {
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

    uintptr_t base   = reinterpret_cast<uintptr_t>(&symbols_test_anchor_fn);
    size_t off       = 0;
    const char* name = kernel::symbols::lookup(base + 2, &off);

    KTEST_REQUIRE_TRUE(name != nullptr);
    KTEST_EXPECT_EQUAL(off, static_cast<size_t>(2));
}

KTEST(symbols_lookup_garbage_address, "symbols") {
    KTEST_REQUIRE_TRUE(kernel::symbols::available());

    // Userspace-shaped address: nothing there.
    const char* name = kernel::symbols::lookup(0x1000, nullptr);
    KTEST_EXPECT_TRUE(name == nullptr);
}

namespace {

bool str_equal(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a++ != *b++) { return false; }
    }
    return *a == '\0' && *b == '\0';
}

}  // namespace

KTEST(symbols_demangle_nested, "symbols") {
    char buf[128];
    KTEST_REQUIRE_TRUE(kernel::symbols::demangle("_ZN6kernel5shell10shell_mainEv", buf, sizeof(buf)));
    KTEST_EXPECT_TRUE(str_equal(buf, "kernel::shell::shell_main()"));
}

KTEST(symbols_demangle_anonymous_namespace, "symbols") {
    char buf[128];
    KTEST_REQUIRE_TRUE(kernel::symbols::demangle(
        "_ZN12_GLOBAL__N_112execute_testERN6kernel5shell11ShellOutputERNS0_7testing5ktestE", buf, sizeof(buf)));
    KTEST_EXPECT_TRUE(str_equal(buf, "(anonymous namespace)::execute_test()"));
}

KTEST(symbols_demangle_unscoped, "symbols") {
    char buf[64];
    KTEST_REQUIRE_TRUE(kernel::symbols::demangle("_Z9vsnprintfPcmPKc", buf, sizeof(buf)));
    KTEST_EXPECT_TRUE(str_equal(buf, "vsnprintf()"));
}

KTEST(symbols_demangle_rejects_non_mangled, "symbols") {
    char buf[64];
    KTEST_EXPECT_FALSE(kernel::symbols::demangle("_start", buf, sizeof(buf)));
    KTEST_EXPECT_FALSE(kernel::symbols::demangle("init_global_constructors_array", buf, sizeof(buf)));
}

KTEST(symbols_demangle_rejects_complex, "symbols") {
    char buf[64];
    // Contains a destructor token (`D1`) we don't support; expect graceful fail.
    KTEST_EXPECT_FALSE(kernel::symbols::demangle("_ZN6kernelD1Ev", buf, sizeof(buf)));
}

#endif  // CONFIG_KERNEL_TESTING
