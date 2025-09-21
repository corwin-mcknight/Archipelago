#pragma once

#include <kernel/config.h>

#if CONFIG_KERNEL_TESTING

namespace kernel::testing {

using test_fn = void (*)();

enum : unsigned {
    KTEST_FLAG_NONE = 0u,
    KTEST_FLAG_REQUIRES_CLEAN_ENV = 1u << 0,
};

struct ktest {
    const char* name;
    const char* submodule;
    unsigned flags;
    test_fn init_fn;
    test_fn fn;
};

void test_runner();
void abort(unsigned char exit_code = 1);

}  // namespace kernel::testing

#if defined(__GNUC__)
#define KTEST_SEC __attribute__((section(".ktests"), used))
#else
#define KTEST_SEC
#endif

// Test definition macro

#define KTEST_WITH_FLAGS(name_sym, module_literal, flags_value)                                            \
    static void name_sym##_body();                                                                         \
    static void name_sym##_noop_init();                                                                    \
    static kernel::testing::ktest _kt_##name_sym KTEST_SEC = {#name_sym, module_literal, flags_value,      \
                                                              &name_sym##_noop_init, &name_sym##_body};    \
    static void name_sym##_noop_init() {}                                                                  \
    static void name_sym##_body()

#define KTEST_WITH_INIT_FLAGS(name_sym, module_literal, init_sym, flags_value)                             \
    static void name_sym##_body();                                                                         \
    static void init_sym();                                                                                \
    static kernel::testing::ktest _kt_##name_sym KTEST_SEC = {#name_sym, module_literal, flags_value,      \
                                                              &init_sym, &name_sym##_body};               \
    static void name_sym##_body()

#define KTEST(name_sym, module_literal) \
    KTEST_WITH_FLAGS(name_sym, module_literal, kernel::testing::KTEST_FLAG_NONE)

#define KTEST_WITH_INIT(name_sym, module_literal, init_sym)                                         \
    KTEST_WITH_INIT_FLAGS(name_sym, module_literal, init_sym, kernel::testing::KTEST_FLAG_NONE)

#define KTEST_INTEGRATION(name_sym, module_literal) \
    KTEST_WITH_FLAGS(name_sym, module_literal, kernel::testing::KTEST_FLAG_REQUIRES_CLEAN_ENV)

#define KTEST_WITH_INIT_INTEGRATION(name_sym, module_literal, init_sym)                             \
    KTEST_WITH_INIT_FLAGS(name_sym, module_literal, init_sym,                                      \
                          kernel::testing::KTEST_FLAG_REQUIRES_CLEAN_ENV)

#define KTEST_NOINIT(name_sym, module_literal) \
    KTEST(name_sym, module_literal)

void ktest_require(bool condition, const char* file, int line, const char* condition_str);
void ktest_require_equal(int actual, int expected, const char* file, int line, const char* actual_str,
                         const char* expected_str);

#define KTEST_REQUIRE(condition) ktest_require(condition, __FILE__, __LINE__, #condition)
#define KTEST_REQUIRE_EQUAL(actual, expected) \
    ktest_require_equal(actual, expected, __FILE__, __LINE__, #actual, #expected)

#else  // CONFIG_KERNEL_TESTING

#define KTEST(name_sym, module_literal) \
    static void name_sym##_body();      \
    static void name_sym##_body()

#define KTEST_WITH_INIT(name_sym, module_literal, init_sym) \
    static void name_sym##_body();                       \
    static void init_sym();                              \
    static void name_sym##_body()

#define KTEST_WITH_FLAGS(name_sym, module_literal, flags_value) \
    KTEST(name_sym, module_literal)

#define KTEST_WITH_INIT_FLAGS(name_sym, module_literal, init_sym, flags_value) \
    KTEST_WITH_INIT(name_sym, module_literal, init_sym)

#define KTEST_INTEGRATION(name_sym, module_literal) \
    KTEST(name_sym, module_literal)

#define KTEST_WITH_INIT_INTEGRATION(name_sym, module_literal, init_sym) \
    KTEST_WITH_INIT(name_sym, module_literal, init_sym)

#define KTEST_NOINIT(name_sym, module_literal) \
    static void name_sym##_body();             \
    static void name_sym##_body()

#endif  // CONFIG_KERNEL_TESTING
