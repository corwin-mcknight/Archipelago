#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/tracking_value.h>

#include <ktl/circular_buffer>
#include <ktl/maybe>

using namespace kernel::testing;

KTEST(ktl_circular_buffer_push_pop, "ktl/circular_buffer") {
    ktl::circular_buffer<int, 4> buf;
    KTEST_EXPECT_ALL(buf.empty(), !buf.full());

    buf.push(1);
    buf.push(2);
    buf.push(3);
    buf.push(4);
    KTEST_EXPECT_ALL(buf.full(), buf.size() == 4);

    KTEST_EXPECT_VALUE(buf.pop(), 1);
    KTEST_EXPECT_VALUE(buf.pop(), 2);
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
    KTEST_EXPECT_VALUE(buf.pop(), 2);
}

KTEST(ktl_circular_buffer_emplace_moves, "ktl/circular_buffer") {
    ktl::circular_buffer<tracking_value, 2> buf;

    tracking_value source{7};
    buf.emplace(ktl::move(source));
    KTEST_EXPECT_ALL(source.value == -1, source.move_observed);

    KTEST_REQUIRE_VALUE(v, buf.peek());
    KTEST_EXPECT_ALL(v.value == 7, v.move_observed);
}

KTEST(ktl_circular_buffer_copy_last_limits, "ktl/circular_buffer") {
    ktl::circular_buffer<int, 5> buf;
    for (int i = 0; i < 5; ++i) { buf.push(i); }

    int dest[3]   = {-1, -1, -1};
    size_t copied = buf.copy_last(dest, 3);
    KTEST_REQUIRE_TRUE(copied == 3);
    KTEST_EXPECT_EQUAL(dest[0], 2);
    KTEST_EXPECT_EQUAL(dest[1], 3);
    KTEST_EXPECT_EQUAL(dest[2], 4);
}

KTEST(ktl_circular_buffer_copy_last_shortens, "ktl/circular_buffer") {
    ktl::circular_buffer<int, 4> buf;
    buf.push(99);

    int dest[4]   = {-1, -1, -1, -1};
    size_t copied = buf.copy_last(dest, 4);
    KTEST_REQUIRE_TRUE(copied == 1);
    KTEST_EXPECT_EQUAL(dest[0], 99);
    KTEST_EXPECT_EQUAL(dest[1], -1);
}

KTEST(ktl_circular_buffer_pop_empty, "ktl/circular_buffer") {
    ktl::circular_buffer<int, 2> buf;
    KTEST_EXPECT_ALL(!buf.pop().has_value(), !buf.peek().has_value());
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
    KTEST_EXPECT_ALL(lf > 0.39f, lf < 0.41f);
    buf.push(3);
    buf.push(4);
    buf.push(5);
    KTEST_EXPECT_ALL(buf.full(), buf.load_factor() == 1.0f);
}

#endif  // CONFIG_KERNEL_TESTING
