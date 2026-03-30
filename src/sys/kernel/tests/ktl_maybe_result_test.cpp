#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/maybe>
#include <ktl/result>
#include <ktl/string_view>

using namespace kernel::testing;

KTEST(ktl_maybe_basic_operations, "ktl/maybe") {
    ktl::maybe<int> empty;
    ktl::maybe<int> value{5};

    KTEST_EXPECT_ALL(!empty.has_value(), value.has_value());
    KTEST_EXPECT_EQUAL(value.value(), 5);
    KTEST_EXPECT_EQUAL(empty.value_or(42), 42);

    KTEST_EXPECT_VALUE(value.map([](int v) { return v * 2; }), 10);
    KTEST_EXPECT_FALSE(empty.map([](int v) { return v + 1; }).has_value());

    KTEST_EXPECT_VALUE(value.and_then([](int v) { return ktl::maybe<int>(v + 3); }), 8);
    KTEST_EXPECT_FALSE(empty.and_then([](int v) { return ktl::maybe<int>(v + 3); }).has_value());

    KTEST_EXPECT_VALUE(empty.or_else([] { return ktl::maybe<int>(99); }), 99);
    KTEST_EXPECT_VALUE(value.or_else([] { return ktl::maybe<int>(111); }), 5);

    KTEST_EXPECT_TRUE(value.filter([](int v) { return v == 5; }).has_value());
    KTEST_EXPECT_FALSE(value.filter([](int v) { return v != 5; }).has_value());

    KTEST_EXPECT_EQUAL(empty.map_or([](int v) { return v * 3; }, 7), 21);
    KTEST_EXPECT_EQUAL(value.map_or([](int v) { return v * 3; }, 7), 15);

    KTEST_EXPECT_ALL(value == ktl::maybe<int>{5}, value != empty);
}

KTEST(ktl_maybe_filter_algorithm, "ktl/maybe") {
    ktl::maybe<int> arr[4] = {ktl::maybe<int>(1), ktl::maybe<int>(4), ktl::nothing, ktl::maybe<int>(9)};
    ktl::filter(arr, 4, [](int v) { return v >= 4; });

    KTEST_EXPECT_FALSE(arr[0].has_value());
    KTEST_EXPECT_VALUE(arr[1], 4);
    KTEST_EXPECT_FALSE(arr[2].has_value());
    KTEST_EXPECT_VALUE(arr[3], 9);
}

KTEST(ktl_result_ok_flow, "ktl/result") {
    auto ok_result = Result<int, const char*>::ok(10);

    KTEST_EXPECT_ALL(ok_result.is_ok(), !ok_result.is_err());
    KTEST_EXPECT_EQUAL(ok_result.unwrap(), 10);
    KTEST_EXPECT_EQUAL(ok_result.expect("should not fail"), 10);
    KTEST_EXPECT_EQUAL(ok_result.unwrap_or(5), 10);
    KTEST_EXPECT_ALL(ok_result.ok().has_value(), !ok_result.err().has_value());

    auto mapped = ok_result.map([](int v) { return v + 2; });
    KTEST_REQUIRE_TRUE(mapped.is_ok());
    KTEST_EXPECT_EQUAL(mapped.unwrap(), 12);

    auto chained = ok_result.and_then([](int v) { return Result<int, const char*>::ok(v * 3); });
    KTEST_REQUIRE_TRUE(chained.is_ok());
    KTEST_EXPECT_EQUAL(chained.unwrap(), 30);

    auto fallback = ok_result.or_else([](const char* msg) { return Result<int, const char*>::err(msg); });
    KTEST_REQUIRE_TRUE(fallback.is_ok());
    KTEST_EXPECT_EQUAL(fallback.unwrap(), 10);

    KTEST_EXPECT_TRUE((ok_result == Result<int, const char*>::ok(10)));
}

KTEST(ktl_result_error_flow, "ktl/result") {
    auto err_result = Result<int, const char*>::err("boom");

    KTEST_EXPECT_ALL(err_result.is_err(), !err_result.is_ok());
    KTEST_EXPECT_EQUAL(err_result.unwrap_or(7), 7);
    KTEST_EXPECT_TRUE(err_result.err().has_value());
    KTEST_EXPECT_EQUAL(ktl::string_view(err_result.unwrap_err()).compare("boom"), 0);
    KTEST_EXPECT_FALSE(err_result.ok().has_value());

    auto mapped_err = err_result.map([](int v) { return v + 1; });
    KTEST_REQUIRE_TRUE(mapped_err.is_err());
    KTEST_EXPECT_EQUAL(ktl::string_view(mapped_err.unwrap_err()).compare("boom"), 0);

    auto chained_err = err_result.and_then([](int v) { return Result<int, const char*>::ok(v + 1); });
    KTEST_REQUIRE_TRUE(chained_err.is_err());
    KTEST_EXPECT_EQUAL(ktl::string_view(chained_err.unwrap_err()).compare("boom"), 0);

    auto recovered = err_result.or_else([](const char*) { return Result<int, const char*>::ok(-1); });
    KTEST_REQUIRE_TRUE(recovered.is_ok());
    KTEST_EXPECT_EQUAL(recovered.unwrap(), -1);

    KTEST_EXPECT_TRUE((err_result != Result<int, const char*>::err("oops")));
}

#endif  // CONFIG_KERNEL_TESTING
