#pragma once

#include <kernel/config.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/registry.h>  // ktest record, flags, abort + report_assertion hooks
#include <stddef.h>

// The registry is walked as a packed array between __start__ktests/__stop__ktests, so the entries
// must stay contiguous. no_sanitize("address") keeps AddressSanitizer (host tier) from inserting
// redzones between them, which would break the iteration; it is a harmless no-op in the kernel build.
#if defined(__GNUC__)
#define KTEST_SEC __attribute__((section(".ktests"), used, no_sanitize("address")))
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

#define KTEST_WITH_INIT_FLAGS(name_sym, module_literal, init_sym, flags_value)                                   \
    static void name_sym##_body();                                                                               \
    static void init_sym();                                                                                      \
    static kernel::testing::ktest _kt_##name_sym KTEST_SEC = {#name_sym, module_literal, flags_value, &init_sym, \
                                                              &name_sym##_body};                                 \
    static void name_sym##_body()

#define KTEST(name_sym, module_literal) KTEST_WITH_FLAGS(name_sym, module_literal, kernel::testing::KTEST_FLAG_NONE)

#define KTEST_WITH_INIT(name_sym, module_literal, init_sym) \
    KTEST_WITH_INIT_FLAGS(name_sym, module_literal, init_sym, kernel::testing::KTEST_FLAG_NONE)

#define KTEST_INTEGRATION(name_sym, module_literal) \
    KTEST_WITH_FLAGS(name_sym, module_literal, kernel::testing::KTEST_FLAG_REQUIRES_CLEAN_ENV)

#define KTEST_WITH_INIT_INTEGRATION(name_sym, module_literal, init_sym) \
    KTEST_WITH_INIT_FLAGS(name_sym, module_literal, init_sym, kernel::testing::KTEST_FLAG_REQUIRES_CLEAN_ENV)

// A test that is expected to crash the kernel. The harness treats a crash as
// pass and a clean exit as fail. Implies REQUIRES_CLEAN_ENV (the test takes
// down the VM).
#define KTEST_CRASH_TEST(name_sym, module_literal) \
    KTEST_WITH_FLAGS(name_sym, module_literal,     \
                     kernel::testing::KTEST_FLAG_REQUIRES_CLEAN_ENV | kernel::testing::KTEST_FLAG_EXPECTS_CRASH)

#define KTEST_NOINIT(name_sym, module_literal) KTEST(name_sym, module_literal)

// File-scope defaults for the common one-module-per-file case. Declare once at the top of the file:
//     KTEST_MODULE("obj/object");                     // or
//     KTEST_MODULE_WITH_INIT("obj/object", my_init);  // my_init is defined by the file
// then define tests without repeating the module or init:
//     KTEST_CASE(obj_object_monotonic_ids) { ... }
// KTEST_CASE_INTEGRATION and KTEST_CASE_CRASH mirror KTEST_INTEGRATION / KTEST_CRASH_TEST.
#define KTEST_MODULE(module_literal)                                                   \
    [[maybe_unused]] static constexpr const char* _ktest_file_module = module_literal; \
    [[maybe_unused]] static void _ktest_file_init() {}

#define KTEST_MODULE_WITH_INIT(module_literal, init_sym)                               \
    [[maybe_unused]] static constexpr const char* _ktest_file_module = module_literal; \
    static void init_sym();                                                            \
    [[maybe_unused]] static void _ktest_file_init() { init_sym(); }

#define KTEST_CASE(name_sym) \
    KTEST_WITH_INIT_FLAGS(name_sym, _ktest_file_module, _ktest_file_init, kernel::testing::KTEST_FLAG_NONE)

#define KTEST_CASE_INTEGRATION(name_sym)                                  \
    KTEST_WITH_INIT_FLAGS(name_sym, _ktest_file_module, _ktest_file_init, \
                          kernel::testing::KTEST_FLAG_REQUIRES_CLEAN_ENV)

#define KTEST_CASE_CRASH(name_sym)                                        \
    KTEST_WITH_INIT_FLAGS(name_sym, _ktest_file_module, _ktest_file_init, \
                          kernel::testing::KTEST_FLAG_REQUIRES_CLEAN_ENV | kernel::testing::KTEST_FLAG_EXPECTS_CRASH)

// The expression-capturing EXPECT / REQUIRE live in <kernel/testing/expect.h> and route through
// kernel::testing::report_assertion (declared above, defined per-backend). The legacy KTEST_* macros
// below are thin aliases over them, so a test written in either style shares one backend and runs on
// either tier unchanged.
#include <kernel/testing/expect.h>

// Bare-condition checks wrap the whole expression in parentheses so it is taken as a single boolean
// (unary) check. This preserves legacy behaviour and stays valid for conditions containing && / ||,
// which the raw decomposer deliberately rejects. The _EQUAL / _NOT_EQUAL forms expose the top-level
// comparison to the decomposer so both operand values are captured in the failure record.
#define KTEST_EXPECT(condition) EXPECT((condition))
#define KTEST_EXPECT_EQUAL(actual, expected) EXPECT((actual) == (expected))
#define KTEST_EXPECT_NOT_EQUAL(actual, expected) EXPECT((actual) != (expected))
#define KTEST_EXPECT_TRUE(condition) KTEST_EXPECT(condition)
#define KTEST_EXPECT_FALSE(condition) KTEST_EXPECT(!(condition))

#define KTEST_REQUIRE(condition) REQUIRE((condition))
#define KTEST_REQUIRE_EQUAL(actual, expected) REQUIRE((actual) == (expected))
#define KTEST_REQUIRE_NOT_EQUAL(actual, expected) REQUIRE((actual) != (expected))
#define KTEST_REQUIRE_TRUE(condition) KTEST_REQUIRE(condition)
#define KTEST_REQUIRE_FALSE(condition) KTEST_REQUIRE(!(condition))

// Batch boolean assertions -- each condition is checked and reported individually.
// KTEST_EXPECT_ALL(a, b, c) expands to KTEST_EXPECT(a); KTEST_EXPECT(b); KTEST_EXPECT(c);
#define KTEST_EA_1(a) KTEST_EXPECT(a)
#define KTEST_EA_2(a, b) \
    KTEST_EXPECT(a);     \
    KTEST_EXPECT(b)
#define KTEST_EA_3(a, b, c) \
    KTEST_EXPECT(a);        \
    KTEST_EXPECT(b);        \
    KTEST_EXPECT(c)
#define KTEST_EA_4(a, b, c, d) \
    KTEST_EXPECT(a);           \
    KTEST_EXPECT(b);           \
    KTEST_EXPECT(c);           \
    KTEST_EXPECT(d)
#define KTEST_EA_5(a, b, c, d, e) \
    KTEST_EXPECT(a);              \
    KTEST_EXPECT(b);              \
    KTEST_EXPECT(c);              \
    KTEST_EXPECT(d);              \
    KTEST_EXPECT(e)
#define KTEST_EA_N(_1, _2, _3, _4, _5, N, ...) N
#define KTEST_EXPECT_ALL(...) \
    KTEST_EA_N(__VA_ARGS__, KTEST_EA_5, KTEST_EA_4, KTEST_EA_3, KTEST_EA_2, KTEST_EA_1)(__VA_ARGS__)

// Unwrap a ktl::result, requiring is_ok(). Declares `var` with the unwrapped value.
// Usage: KTEST_UNWRAP(id, table.emplace<ObjA>(RIGHTS_ALL));
#define KTEST_UNWRAP(var, expr)                   \
    auto _ktest_res_##var = (expr);               \
    KTEST_REQUIRE_TRUE(_ktest_res_##var.is_ok()); \
    auto var = _ktest_res_##var.unwrap()

// Check that a maybe/optional expression has a value equal to expected.
// Usage: KTEST_EXPECT_VALUE(dq.pop_front(), 0);
#define KTEST_EXPECT_VALUE(expr, expected)                 \
    do {                                                   \
        auto _ktest_mv = (expr);                           \
        KTEST_REQUIRE_TRUE(_ktest_mv.has_value());         \
        KTEST_EXPECT_EQUAL(_ktest_mv.value(), (expected)); \
    } while (0)

// Extract value from a maybe/optional, requiring has_value(). Declares `var`.
// Usage: KTEST_REQUIRE_VALUE(front, dq.front());  // then use `front` directly
#define KTEST_REQUIRE_VALUE(var, expr)               \
    auto _ktest_mv_##var = (expr);                   \
    KTEST_REQUIRE_TRUE(_ktest_mv_##var.has_value()); \
    auto var = _ktest_mv_##var.value()

// Check pointer alignment.
// Usage: KTEST_EXPECT_ALIGNED(ptr, 16);
#define KTEST_EXPECT_ALIGNED(ptr, alignment) \
    KTEST_EXPECT_EQUAL(reinterpret_cast<uintptr_t>(ptr) & ((alignment) - 1), static_cast<uintptr_t>(0))

#else  // CONFIG_KERNEL_TESTING

// Test bodies are dead code when testing is off; [[maybe_unused]] keeps the
// forward-declaration + definition pair from tripping -Wunused-function.
#define KTEST(name_sym, module_literal)             \
    [[maybe_unused]] static void name_sym##_body(); \
    [[maybe_unused]] static void name_sym##_body()

#define KTEST_WITH_INIT(name_sym, module_literal, init_sym) \
    [[maybe_unused]] static void name_sym##_body();         \
    [[maybe_unused]] static void init_sym();                \
    [[maybe_unused]] static void name_sym##_body()

#define KTEST_WITH_FLAGS(name_sym, module_literal, flags_value) KTEST(name_sym, module_literal)

#define KTEST_WITH_INIT_FLAGS(name_sym, module_literal, init_sym, flags_value) \
    KTEST_WITH_INIT(name_sym, module_literal, init_sym)

#define KTEST_INTEGRATION(name_sym, module_literal) KTEST(name_sym, module_literal)

#define KTEST_WITH_INIT_INTEGRATION(name_sym, module_literal, init_sym) \
    KTEST_WITH_INIT(name_sym, module_literal, init_sym)

#define KTEST_CRASH_TEST(name_sym, module_literal) KTEST(name_sym, module_literal)

#define KTEST_NOINIT(name_sym, module_literal) KTEST(name_sym, module_literal)

#define KTEST_MODULE(module_literal)
#define KTEST_MODULE_WITH_INIT(module_literal, init_sym) [[maybe_unused]] static void init_sym();

#define KTEST_CASE(name_sym)                        \
    [[maybe_unused]] static void name_sym##_body(); \
    [[maybe_unused]] static void name_sym##_body()

#define KTEST_CASE_INTEGRATION(name_sym) KTEST_CASE(name_sym)
#define KTEST_CASE_CRASH(name_sym) KTEST_CASE(name_sym)

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

#define KTEST_EXPECT_ALL(...) ((void)0)
#define KTEST_UNWRAP(var, expr) auto var = (expr).unwrap()
#define KTEST_EXPECT_VALUE(expr, expected) ((void)0)
#define KTEST_REQUIRE_VALUE(var, expr) auto var = (expr).value()
#define KTEST_EXPECT_ALIGNED(ptr, alignment) ((void)0)

#endif  // CONFIG_KERNEL_TESTING
