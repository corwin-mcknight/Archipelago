#include <kernel/testing/testing.h>

#include <ktl/maybe>
#include <ktl/result>
#include <ktl/string_view>

using namespace kernel::testing;

KTEST(ktl_maybe_basic_operations, "ktl/maybe") {
    ktl::maybe<int> empty;
    ktl::maybe<int> value{5};

    KTEST_REQUIRE_FALSE(empty.has_value());
    KTEST_REQUIRE_TRUE(value.has_value());
    KTEST_EXPECT_EQUAL(value.value(), 5);
    KTEST_EXPECT_EQUAL(empty.value_or(42), 42);

    auto mapped = value.map([](int v) { return v * 2; });
    KTEST_REQUIRE_TRUE(mapped.has_value());
    KTEST_EXPECT_EQUAL(mapped.value(), 10);

    auto mapped_empty = empty.map([](int v) { return v + 1; });
    KTEST_EXPECT_FALSE(mapped_empty.has_value());

    auto chained = value.and_then([](int v) { return ktl::maybe<int>(v + 3); });
    KTEST_REQUIRE_TRUE(chained.has_value());
    KTEST_EXPECT_EQUAL(chained.value(), 8);

    auto chained_empty = empty.and_then([](int v) { return ktl::maybe<int>(v + 3); });
    KTEST_EXPECT_FALSE(chained_empty.has_value());

    auto fallback = empty.or_else([] { return ktl::maybe<int>(99); });
    KTEST_REQUIRE_TRUE(fallback.has_value());
    KTEST_EXPECT_EQUAL(fallback.value(), 99);

    auto preserved = value.or_else([] { return ktl::maybe<int>(111); });
    KTEST_REQUIRE_TRUE(preserved.has_value());
    KTEST_EXPECT_EQUAL(preserved.value(), 5);

    auto filtered_keep = value.filter([](int v) { return v == 5; });
    KTEST_REQUIRE_TRUE(filtered_keep.has_value());
    auto filtered_drop = value.filter([](int v) { return v != 5; });
    KTEST_EXPECT_FALSE(filtered_drop.has_value());

    auto mapped_or = empty.map_or([](int v) { return v * 3; }, 7);
    KTEST_EXPECT_EQUAL(mapped_or, 21);

    auto mapped_or_value = value.map_or([](int v) { return v * 3; }, 7);
    KTEST_EXPECT_EQUAL(mapped_or_value, 15);

    ktl::maybe<int> copy_value{5};
    KTEST_EXPECT_TRUE(value == copy_value);
    KTEST_EXPECT_TRUE(value != empty);
}

KTEST(ktl_maybe_filter_algorithm, "ktl/maybe") {
    ktl::maybe<int> arr[4] = {ktl::maybe<int>(1), ktl::maybe<int>(4), ktl::nothing, ktl::maybe<int>(9)};

    ktl::filter(arr, 4, [](int v) { return v >= 4; });

    KTEST_EXPECT_FALSE(arr[0].has_value());
    KTEST_REQUIRE_TRUE(arr[1].has_value());
    KTEST_EXPECT_EQUAL(arr[1].value(), 4);
    KTEST_EXPECT_FALSE(arr[2].has_value());
    KTEST_REQUIRE_TRUE(arr[3].has_value());
    KTEST_EXPECT_EQUAL(arr[3].value(), 9);
}

KTEST(ktl_result_ok_flow, "ktl/result") {
    auto ok_result = Result<int, const char*>::ok(10);

    KTEST_REQUIRE_TRUE(ok_result.is_ok());
    KTEST_EXPECT_FALSE(ok_result.is_err());
    KTEST_EXPECT_EQUAL(ok_result.unwrap(), 10);
    KTEST_EXPECT_EQUAL(ok_result.expect("should not fail"), 10);
    KTEST_EXPECT_EQUAL(ok_result.unwrap_or(5), 10);
    KTEST_EXPECT_TRUE(ok_result.ok().has_value());
    KTEST_EXPECT_FALSE(ok_result.err().has_value());

    auto mapped = ok_result.map([](int v) { return v + 2; });
    KTEST_REQUIRE_TRUE(mapped.is_ok());
    KTEST_EXPECT_EQUAL(mapped.unwrap(), 12);

    auto chained = ok_result.and_then([](int v) { return Result<int, const char*>::ok(v * 3); });
    KTEST_REQUIRE_TRUE(chained.is_ok());
    KTEST_EXPECT_EQUAL(chained.unwrap(), 30);

    auto fallback = ok_result.or_else([](const char* msg) { return Result<int, const char*>::err(msg); });
    KTEST_REQUIRE_TRUE(fallback.is_ok());
    KTEST_EXPECT_EQUAL(fallback.unwrap(), 10);

    auto another_ok = Result<int, const char*>::ok(10);
    KTEST_EXPECT_TRUE(ok_result == another_ok);
}

KTEST(ktl_result_error_flow, "ktl/result") {
    auto err_result = Result<int, const char*>::err("boom");

    KTEST_REQUIRE_TRUE(err_result.is_err());
    KTEST_EXPECT_FALSE(err_result.is_ok());
    KTEST_EXPECT_EQUAL(err_result.unwrap_or(7), 7);

    auto err_maybe = err_result.err();
    KTEST_REQUIRE_TRUE(err_maybe.has_value());
    KTEST_EXPECT_EQUAL(ktl::string_view(err_maybe.value()).compare("boom"), 0);

    KTEST_EXPECT_EQUAL(ktl::string_view(err_result.unwrap_err()).compare("boom"), 0);

    auto ok_maybe = err_result.ok();
    KTEST_EXPECT_FALSE(ok_maybe.has_value());

    auto mapped_err = err_result.map([](int v) { return v + 1; });
    KTEST_REQUIRE_TRUE(mapped_err.is_err());
    KTEST_EXPECT_EQUAL(ktl::string_view(mapped_err.unwrap_err()).compare("boom"), 0);

    auto chained_err = err_result.and_then([](int v) { return Result<int, const char*>::ok(v + 1); });
    KTEST_REQUIRE_TRUE(chained_err.is_err());
    KTEST_EXPECT_EQUAL(ktl::string_view(chained_err.unwrap_err()).compare("boom"), 0);

    auto recovered = err_result.or_else([](const char* msg) {
        (void)msg;
        return Result<int, const char*>::ok(-1);
    });
    KTEST_REQUIRE_TRUE(recovered.is_ok());
    KTEST_EXPECT_EQUAL(recovered.unwrap(), -1);

    auto another_err = Result<int, const char*>::err("oops");
    KTEST_EXPECT_TRUE(err_result != another_err);
}
