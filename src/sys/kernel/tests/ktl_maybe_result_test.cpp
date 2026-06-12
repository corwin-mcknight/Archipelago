#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/tracking_value.h>

#include <ktl/maybe>
#include <ktl/result>
#include <ktl/string_view>
#include <ktl/utility>

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

    // map_or returns the default untouched when empty; the function only ever sees a real value.
    KTEST_EXPECT_EQUAL(empty.map_or([](int v) { return v * 3; }, 7), 7);
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

KTEST(ktl_maybe_operator_bool, "ktl/maybe") {
    ktl::maybe<int> empty;
    ktl::maybe<int> value{5};

    KTEST_EXPECT_TRUE(static_cast<bool>(value));
    KTEST_EXPECT_FALSE(static_cast<bool>(empty));
    if (!value) { KTEST_EXPECT_TRUE(false); }
}

KTEST(ktl_maybe_expect_returns_value, "ktl/maybe") {
    ktl::maybe<int> value{5};
    KTEST_EXPECT_EQUAL(value.expect("value must be present"), 5);

    // expect returns a mutable reference, like value().
    value.expect("mutable access") = 6;
    KTEST_EXPECT_EQUAL(value.value(), 6);
}

KTEST(ktl_maybe_reset_empties, "ktl/maybe") {
    ktl::maybe<int> value{5};
    value.reset();
    KTEST_EXPECT_FALSE(value.has_value());
}

KTEST(ktl_maybe_move_construction, "ktl/maybe") {
    tracking_value tv{7};
    ktl::maybe<tracking_value> moved_in{ktl::move(tv)};

    KTEST_REQUIRE_TRUE(moved_in.has_value());
    KTEST_EXPECT_EQUAL(moved_in.value().value, 7);
    KTEST_EXPECT_TRUE(moved_in.value().move_observed);
    KTEST_EXPECT_EQUAL(tv.value, -1);
}

KTEST(ktl_maybe_take_moves_value_out, "ktl/maybe") {
    ktl::maybe<tracking_value> source{tracking_value{42}};
    auto taken = source.take();

    KTEST_REQUIRE_TRUE(taken.has_value());
    KTEST_EXPECT_EQUAL(taken.value().value, 42);
    KTEST_EXPECT_TRUE(taken.value().move_observed);
    KTEST_EXPECT_FALSE(source.has_value());

    auto taken_again = source.take();
    KTEST_EXPECT_FALSE(taken_again.has_value());
}

KTEST(ktl_maybe_inspect_side_effect, "ktl/maybe") {
    ktl::maybe<int> empty;
    ktl::maybe<int> value{5};

    int seen = 0;
    value.inspect([&](int v) { seen = v; });
    KTEST_EXPECT_EQUAL(seen, 5);

    empty.inspect([&](int) { seen = -1; });
    KTEST_EXPECT_EQUAL(seen, 5);

    // The non-const overload exposes a mutable reference.
    value.inspect([](int& v) { v += 1; });
    KTEST_EXPECT_EQUAL(value.value(), 6);

    // inspect returns the maybe unchanged, so it chains.
    KTEST_EXPECT_VALUE(value.inspect([](int) {}).map([](int v) { return v + 1; }), 7);
}

KTEST(ktl_maybe_ref_basics, "ktl/maybe") {
    int x = 5;
    ktl::maybe<int&> ref{x};
    ktl::maybe<int&> empty;

    KTEST_EXPECT_ALL(ref.has_value(), !empty.has_value());
    KTEST_EXPECT_TRUE(static_cast<bool>(ref));
    KTEST_EXPECT_EQUAL(ref.value(), 5);
    KTEST_EXPECT_EQUAL(*ref, 5);

    // maybe<T&> aliases the referent: writes through it land at the origin.
    ref.value() = 7;
    KTEST_EXPECT_EQUAL(x, 7);

    int fallback = 9;
    KTEST_EXPECT_EQUAL(empty.value_or(fallback), 9);
    KTEST_EXPECT_EQUAL(ref.value_or(fallback), 7);
    KTEST_EXPECT_EQUAL(ref.expect("present"), 7);

    KTEST_EXPECT_TRUE(empty.ptr_or() == nullptr);
    KTEST_EXPECT_TRUE(ref.ptr_or() == &x);

    ref.reset();
    KTEST_EXPECT_FALSE(ref.has_value());
}

KTEST(ktl_maybe_ref_combinators, "ktl/maybe") {
    int x = 5;
    ktl::maybe<int&> ref{x};
    ktl::maybe<int&> empty;

    KTEST_EXPECT_VALUE(ref.map([](int& v) { return v * 2; }), 10);
    KTEST_EXPECT_FALSE(empty.map([](int& v) { return v * 2; }).has_value());

    KTEST_EXPECT_VALUE(ref.and_then([](int& v) { return ktl::maybe<int>(v + 1); }), 6);
    KTEST_EXPECT_FALSE(empty.and_then([](int& v) { return ktl::maybe<int>(v + 1); }).has_value());

    KTEST_EXPECT_TRUE(ref.filter([](int& v) { return v == 5; }).has_value());
    KTEST_EXPECT_FALSE(ref.filter([](int& v) { return v != 5; }).has_value());

    int seen = 0;
    ref.inspect([&](int& v) { seen = v; });
    KTEST_EXPECT_EQUAL(seen, 5);

    int y          = 1;
    auto recovered = empty.or_else([&]() -> ktl::maybe<int&> { return ktl::maybe<int&>(y); });
    KTEST_REQUIRE_TRUE(recovered.has_value());
    KTEST_EXPECT_TRUE(&recovered.value() == &y);
}

KTEST(ktl_from_ptr_bridges_nullable_pointers, "ktl/maybe") {
    int x        = 3;
    auto present = ktl::from_ptr(&x);
    KTEST_REQUIRE_TRUE(present.has_value());

    present.value() = 4;
    KTEST_EXPECT_EQUAL(x, 4);

    int* null_ptr = nullptr;
    KTEST_EXPECT_FALSE(ktl::from_ptr(null_ptr).has_value());
}

KTEST(ktl_maybe_ok_or_bridges_to_result, "ktl/result") {
    ktl::maybe<int> value{5};
    ktl::maybe<int> empty;

    auto ok = ktl::ok_or(value, ktl::errc::oom);
    KTEST_REQUIRE_TRUE(ok.is_ok());
    KTEST_EXPECT_EQUAL(ok.unwrap(), 5);

    auto err = ktl::ok_or(empty, ktl::errc::oom);
    KTEST_REQUIRE_TRUE(err.is_err());
    KTEST_EXPECT_TRUE(err.unwrap_err() == ktl::errc::oom);
}

namespace {
struct point {
    int x = 0;
    int y = 0;
};
}  // namespace

KTEST(ktl_maybe_value_round_trip, "ktl/maybe") {
    ktl::maybe<point> p{point{3, 4}};
    KTEST_REQUIRE_TRUE(p.has_value());

    // value(), operator* and operator-> all expose the stored value.
    KTEST_EXPECT_EQUAL(p.value().x, 3);
    KTEST_EXPECT_EQUAL((*p).y, 4);
    KTEST_EXPECT_EQUAL(p->x, 3);

    // Mutation through the non-const accessors round-trips.
    p.value().x = 10;
    (*p).y      = 20;
    p->x += 1;

    const ktl::maybe<point>& const_ref = p;
    KTEST_REQUIRE_TRUE(const_ref.has_value());
    KTEST_EXPECT_EQUAL(const_ref.value().x, 11);
    KTEST_EXPECT_EQUAL((*const_ref).y, 20);
    KTEST_EXPECT_EQUAL(const_ref->y, 20);
}

KTEST(ktl_result_void_basic, "ktl/result") {
    auto ok = Result<void, int>::ok();
    KTEST_EXPECT_ALL(ok.is_ok(), !ok.is_err(), static_cast<bool>(ok));
    ok.expect("void ok must not panic");
    ok.unwrap();
    KTEST_EXPECT_FALSE(ok.err().has_value());

    auto err = Result<void, int>::err(-4);
    KTEST_EXPECT_ALL(err.is_err(), !err.is_ok(), !static_cast<bool>(err));
    KTEST_EXPECT_EQUAL(err.unwrap_err(), -4);
    KTEST_EXPECT_VALUE(err.err(), -4);

    using VoidIntResult = Result<void, int>;
    KTEST_EXPECT_ALL(ok == VoidIntResult::ok(), err == VoidIntResult::err(-4), ok != err);

    auto copied = err;
    KTEST_EXPECT_ALL(copied.is_err(), copied.unwrap_err() == -4);
}

KTEST(ktl_result_void_combinators, "ktl/result") {
    auto ok      = Result<void, int>::ok();
    auto err     = Result<void, int>::err(-2);

    auto chained = ok.and_then([] { return Result<int, long>::ok(7); });
    KTEST_EXPECT_ALL(chained.is_ok(), chained.unwrap() == 7);
    auto short_circuited = err.and_then([] { return Result<int, long>::ok(7); });
    KTEST_EXPECT_ALL(short_circuited.is_err(), short_circuited.unwrap_err() == -2L);

    auto recovered = err.or_else([](int) { return Result<void, int>::ok(); });
    KTEST_EXPECT_TRUE(recovered.is_ok());
    auto kept = ok.or_else([](int) { return Result<void, int>::err(-9); });
    KTEST_EXPECT_TRUE(kept.is_ok());

    auto remapped = err.map_err([](int e) { return e * 10; });
    KTEST_EXPECT_ALL(remapped.is_err(), remapped.unwrap_err() == -20);
    auto ok_remapped = ok.map_err([](int e) { return e * 10; });
    KTEST_EXPECT_TRUE(ok_remapped.is_ok());
}

KTEST(ktl_result_map_err_primary, "ktl/result") {
    auto ok        = Result<int, int>::ok(5);
    auto err       = Result<int, int>::err(-3);

    auto ok_mapped = ok.map_err([](int e) { return e - 1; });
    KTEST_EXPECT_ALL(ok_mapped.is_ok(), ok_mapped.unwrap() == 5);

    auto err_mapped = err.map_err([](int e) { return e - 1; });
    KTEST_EXPECT_ALL(err_mapped.is_err(), err_mapped.unwrap_err() == -4);
}

namespace {

Result<int, int> ktry_source(bool succeed) {
    if (succeed) { return Result<int, int>::ok(21); }
    return ktl::err(-6);
}

Result<int, int> ktry_chain(bool succeed) {
    int v = KTRY(ktry_source(succeed));
    return Result<int, int>::ok(v * 2);
}

Result<void, int> ktry_void_source(bool succeed) {
    if (succeed) { return Result<void, int>::ok(); }
    return ktl::err(-7);
}

// Caller's error type (long) differs from the callee's (int): exercises carrier conversion.
Result<int, long> ktry_converting(bool succeed) {
    KTRY(ktry_void_source(succeed));
    return Result<int, long>::ok(1);
}

}  // namespace

KTEST(ktl_result_ktry, "ktl/result") {
    auto good = ktry_chain(true);
    KTEST_EXPECT_ALL(good.is_ok(), good.unwrap() == 42);

    auto bad = ktry_chain(false);
    KTEST_EXPECT_ALL(bad.is_err(), bad.unwrap_err() == -6);

    auto void_good = ktry_converting(true);
    KTEST_EXPECT_ALL(void_good.is_ok(), void_good.unwrap() == 1);

    auto void_bad = ktry_converting(false);
    KTEST_EXPECT_ALL(void_bad.is_err(), void_bad.unwrap_err() == -7L);
}

KTEST(ktl_result_errc_alias, "ktl/result") {
    ktl::result<int> good = ktl::result<int>::ok(3);
    KTEST_EXPECT_ALL(good.is_ok(), good.unwrap() == 3);

    ktl::result<void> fail = ktl::err(ktl::errc::oom);
    KTEST_EXPECT_ALL(fail.is_err(), fail.unwrap_err() == ktl::errc::oom);

    // errc preserves the retired RESULT_* numeric space for log continuity.
    KTEST_EXPECT_EQUAL(static_cast<int>(ktl::errc::oom), -4);
    KTEST_EXPECT_EQUAL(static_cast<int>(ktl::errc::registry_full), -9);
}

#endif  // CONFIG_KERNEL_TESTING
