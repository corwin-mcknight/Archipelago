#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/circular_buffer>
#include <ktl/maybe>

using namespace kernel::testing;

namespace {

struct tracking_value {
    int value;
    bool moved = false;

    tracking_value() = default;
    explicit tracking_value(int v) : value(v) {}
    tracking_value(const tracking_value&) = default;
    tracking_value& operator=(const tracking_value&) = default;

    tracking_value(tracking_value&& other) noexcept : value(other.value), moved(true) {
        other.value = -1;
        other.moved = true;
    }

    tracking_value& operator=(tracking_value&& other) noexcept {
        value = other.value;
        moved = true;
        other.value = -1;
        other.moved = true;
        return *this;
    }
};

}  // namespace

KTEST(ktl_circular_buffer_push_pop, "ktl/circular_buffer") {
    ktl::circular_buffer<int, 4> buf;

    KTEST_REQUIRE_TRUE(buf.empty());
    KTEST_REQUIRE_FALSE(buf.full());

    buf.push(1);
    buf.push(2);
    buf.push(3);
    buf.push(4);

    KTEST_REQUIRE_TRUE(buf.full());
    KTEST_REQUIRE_TRUE(buf.size() == 4);

    auto v1 = buf.pop();
    auto v2 = buf.pop();

    KTEST_REQUIRE_TRUE(v1.has_value());
    KTEST_REQUIRE_TRUE(v2.has_value());
    KTEST_EXPECT_EQUAL(v1.value(), 1);
    KTEST_EXPECT_EQUAL(v2.value(), 2);
    KTEST_EXPECT_TRUE(buf.size() == 2);
}

KTEST(ktl_circular_buffer_overwrite_when_full, "ktl/circular_buffer") {
    ktl::circular_buffer<int, 3> buf;

    buf.push(1);
    buf.push(2);
    buf.push(3);
    buf.push(4);  // overwrites oldest

    int out = -1;
    buf.copy_last(&out, 1);

    KTEST_EXPECT_EQUAL(out, 4);
    auto v = buf.pop();
    KTEST_REQUIRE_TRUE(v.has_value());
    KTEST_EXPECT_EQUAL(v.value(), 2);
}

KTEST(ktl_circular_buffer_emplace_moves, "ktl/circular_buffer") {
    ktl::circular_buffer<tracking_value, 2> buf;

    tracking_value source{7};
    buf.emplace(ktl::move(source));

    KTEST_EXPECT_EQUAL(source.value, -1);
    KTEST_EXPECT_TRUE(source.moved);

    auto v = buf.peek();
    KTEST_REQUIRE_TRUE(v.has_value());
    KTEST_EXPECT_EQUAL(v.value().value, 7);
    KTEST_EXPECT_TRUE(v.value().moved);
}

KTEST(ktl_circular_buffer_copy_last_limits, "ktl/circular_buffer") {
    ktl::circular_buffer<int, 5> buf;
    for (int i = 0; i < 5; ++i) { buf.push(i); }

    int dest[3] = {-1, -1, -1};

    size_t copied = buf.copy_last(dest, 3);

    KTEST_REQUIRE_TRUE(copied == 3);
    KTEST_EXPECT_EQUAL(dest[0], 2);
    KTEST_EXPECT_EQUAL(dest[1], 3);
    KTEST_EXPECT_EQUAL(dest[2], 4);
}

KTEST(ktl_circular_buffer_copy_last_shortens, "ktl/circular_buffer") {
    ktl::circular_buffer<int, 4> buf;
    buf.push(99);

    int dest[4] = {-1, -1, -1, -1};
    size_t copied = buf.copy_last(dest, 4);

    KTEST_REQUIRE_TRUE(copied == 1);
    KTEST_EXPECT_EQUAL(dest[0], 99);
    KTEST_EXPECT_EQUAL(dest[1], -1);
    KTEST_EXPECT_EQUAL(dest[2], -1);
    KTEST_EXPECT_EQUAL(dest[3], -1);
}

KTEST(ktl_circular_buffer_pop_empty, "ktl/circular_buffer") {
    ktl::circular_buffer<int, 2> buf;

    auto empty_pop = buf.pop();
    auto empty_peek = buf.peek();
    KTEST_EXPECT_FALSE(empty_pop.has_value());
    KTEST_EXPECT_FALSE(empty_peek.has_value());
}

KTEST(ktl_circular_buffer_for_each, "ktl/circular_buffer") {
    ktl::circular_buffer<int, 3> buf;
    buf.push(10);
    buf.push(11);
    buf.push(12);
    buf.push(13);

    int sum = 0;
    buf.for_each([&](int value) { sum += value; });

    KTEST_EXPECT_EQUAL(sum, 11 + 12 + 13);
}

KTEST(ktl_circular_buffer_load_factor, "ktl/circular_buffer") {
    ktl::circular_buffer<int, 5> buf;

    buf.push(1);
    buf.push(2);

    float lf = buf.load_factor();
    KTEST_EXPECT_TRUE(lf > 0.39f);
    KTEST_EXPECT_TRUE(lf < 0.41f);
    buf.push(3);
    buf.push(4);
    buf.push(5);

    KTEST_REQUIRE_TRUE(buf.full());
    KTEST_EXPECT_TRUE(buf.load_factor() == 1.0f);
}

#endif  // CONFIG_KERNEL_TESTING