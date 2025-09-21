#pragma once

#include <kernel/config.h>

#if CONFIG_KERNEL_TESTING

namespace kernel::testing {

using test_fn = void (*)();

enum : unsigned {
    KTEST_FLAG_NONE = 0u,
    KTEST_FLAG_REQUIRES_CLEAN_ENV = 1u << 0,
};

struct alignas(alignof(void*)) ktest {
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

#define KTEST_WITH_FLAGS(name_sym, module_literal, flags_value)                                         \
    static void name_sym##_body();                                                                      \
    static void name_sym##_noop_init();                                                                 \
    static kernel::testing::ktest _kt_##name_sym KTEST_SEC = {#name_sym, module_literal, flags_value,   \
                                                              &name_sym##_noop_init, &name_sym##_body}; \
    static void name_sym##_noop_init() {}                                                               \
    static void name_sym##_body()

#define KTEST_WITH_INIT_FLAGS(name_sym, module_literal, init_sym, flags_value)                        \
    static void name_sym##_body();                                                                    \
    static void init_sym();                                                                           \
    static kernel::testing::ktest _kt_##name_sym KTEST_SEC = {#name_sym, module_literal, flags_value, \
                                                              &init_sym, &name_sym##_body};           \
    static void name_sym##_body()

#define KTEST(name_sym, module_literal) \
    KTEST_WITH_FLAGS(name_sym, module_literal, kernel::testing::KTEST_FLAG_NONE)

#define KTEST_WITH_INIT(name_sym, module_literal, init_sym) \
    KTEST_WITH_INIT_FLAGS(name_sym, module_literal, init_sym, kernel::testing::KTEST_FLAG_NONE)

#define KTEST_INTEGRATION(name_sym, module_literal) \
    KTEST_WITH_FLAGS(name_sym, module_literal, kernel::testing::KTEST_FLAG_REQUIRES_CLEAN_ENV)

#define KTEST_WITH_INIT_INTEGRATION(name_sym, module_literal, init_sym) \
    KTEST_WITH_INIT_FLAGS(name_sym, module_literal, init_sym, kernel::testing::KTEST_FLAG_REQUIRES_CLEAN_ENV)

#define KTEST_NOINIT(name_sym, module_literal) KTEST(name_sym, module_literal)

// Assertion helpers. EXPECT variants record the failure and allow execution to continue, while
// REQUIRE variants abort the current test immediately after logging the failure.
void ktest_expect(bool condition, const char* file, int line, const char* condition_str);
void ktest_require(bool condition, const char* file, int line, const char* condition_str);
void ktest_expect_equal(int actual, int expected, const char* file, int line, const char* actual_str,
                        const char* expected_str);
void ktest_require_equal(int actual, int expected, const char* file, int line, const char* actual_str,
                         const char* expected_str);
void ktest_expect_not_equal(int actual, int expected, const char* file, int line, const char* actual_str,
                            const char* expected_str);
void ktest_require_not_equal(int actual, int expected, const char* file, int line, const char* actual_str,
                             const char* expected_str);

#define KTEST_EXPECT(condition) ktest_expect(condition, __FILE__, __LINE__, #condition)
#define KTEST_EXPECT_EQUAL(actual, expected) \
    ktest_expect_equal(actual, expected, __FILE__, __LINE__, #actual, #expected)
#define KTEST_EXPECT_NOT_EQUAL(actual, expected) \
    ktest_expect_not_equal(actual, expected, __FILE__, __LINE__, #actual, #expected)
#define KTEST_EXPECT_TRUE(condition) KTEST_EXPECT(condition)
#define KTEST_EXPECT_FALSE(condition) KTEST_EXPECT(!(condition))

#define KTEST_REQUIRE(condition) ktest_require(condition, __FILE__, __LINE__, #condition)
#define KTEST_REQUIRE_EQUAL(actual, expected) \
    ktest_require_equal(actual, expected, __FILE__, __LINE__, #actual, #expected)
#define KTEST_REQUIRE_NOT_EQUAL(actual, expected) \
    ktest_require_not_equal(actual, expected, __FILE__, __LINE__, #actual, #expected)
#define KTEST_REQUIRE_TRUE(condition) KTEST_REQUIRE(condition)
#define KTEST_REQUIRE_FALSE(condition) KTEST_REQUIRE(!(condition))

#else  // CONFIG_KERNEL_TESTING

#define KTEST(name_sym, module_literal) \
    static void name_sym##_body();      \
    static void name_sym##_body()

#define KTEST_WITH_INIT(name_sym, module_literal, init_sym) \
    static void name_sym##_body();                          \
    static void init_sym();                                 \
    static void name_sym##_body()

#define KTEST_WITH_FLAGS(name_sym, module_literal, flags_value) KTEST(name_sym, module_literal)

#define KTEST_WITH_INIT_FLAGS(name_sym, module_literal, init_sym, flags_value) \
    KTEST_WITH_INIT(name_sym, module_literal, init_sym)

#define KTEST_INTEGRATION(name_sym, module_literal) KTEST(name_sym, module_literal)

#define KTEST_WITH_INIT_INTEGRATION(name_sym, module_literal, init_sym) \
    KTEST_WITH_INIT(name_sym, module_literal, init_sym)

#define KTEST_NOINIT(name_sym, module_literal) \
    static void name_sym##_body();             \
    static void name_sym##_body()

#define KTEST_EXPECT(condition) ((void)0)
#define KTEST_EXPECT_EQUAL(actual, expected) ((void)0)
#define KTEST_EXPECT_NOT_EQUAL(actual, expected) ((void)0)
#define KTEST_EXPECT_TRUE(condition) ((void)0)
#define KTEST_EXPECT_FALSE(condition) ((void)0)

#define KTEST_REQUIRE(condition) ((void)0)
#define KTEST_REQUIRE_EQUAL(actual, expected) ((void)0)
#define KTEST_REQUIRE_NOT_EQUAL(actual, expected) ((void)0)
#define KTEST_REQUIRE_TRUE(condition) ((void)0)
#define KTEST_REQUIRE_FALSE(condition) ((void)0)

#endif  // CONFIG_KERNEL_TESTING
