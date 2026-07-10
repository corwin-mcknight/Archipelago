// src/sys/kernel/tests/sched_trace_test.cpp
#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/sched/trace.h>

using namespace kernel::sched;

static trace_record rec(uint64_t ts) {
    trace_record r;
    r.timestamp = ts;
    r.from_id   = ts * 10;
    r.to_id     = ts * 10 + 1;
    r.kind      = trace_kind::SWITCH;
    r.reason    = switch_reason::YIELD;
    return r;
}

KTEST(sched_trace_ring_fills_and_orders, "sched/trace") {
    trace_ring<4> ring;
    KTEST_EXPECT_EQUAL(ring.size(), 0u);
    for (uint64_t i = 1; i <= 3; ++i) { ring.push(rec(i)); }
    trace_record out[4];
    size_t n = ring.copy_newest(out, 4);
    KTEST_REQUIRE_EQUAL(n, 3u);
    KTEST_EXPECT_EQUAL(out[0].timestamp, 3u);  // newest first
    KTEST_EXPECT_EQUAL(out[2].timestamp, 1u);
}

KTEST(sched_trace_ring_wraps_overwriting_oldest, "sched/trace") {
    trace_ring<4> ring;
    for (uint64_t i = 1; i <= 6; ++i) { ring.push(rec(i)); }
    KTEST_EXPECT_EQUAL(ring.size(), 4u);
    trace_record out[4];
    size_t n = ring.copy_newest(out, 4);
    KTEST_REQUIRE_EQUAL(n, 4u);
    KTEST_EXPECT_EQUAL(out[0].timestamp, 6u);
    KTEST_EXPECT_EQUAL(out[3].timestamp, 3u);  // 1 and 2 overwritten
}

KTEST(sched_trace_ring_copy_caps_and_clear, "sched/trace") {
    trace_ring<4> ring;
    for (uint64_t i = 1; i <= 4; ++i) { ring.push(rec(i)); }
    trace_record out[2];
    KTEST_EXPECT_EQUAL(ring.copy_newest(out, 2), 2u);
    KTEST_EXPECT_EQUAL(out[1].timestamp, 3u);
    ring.clear();
    KTEST_EXPECT_EQUAL(ring.size(), 0u);
    KTEST_EXPECT_EQUAL(ring.copy_newest(out, 2), 0u);
}

KTEST(sched_cycles_to_human_units, "sched/trace") {
    // hz=0: uncalibrated fallback, raw cycles
    KTEST_EXPECT_TRUE(cycles_to_human(1234, 0).unit[0] == 'c');
    KTEST_EXPECT_EQUAL(cycles_to_human(1234, 0).whole, 1234u);
    // 1 MHz clock: 1 cycle = 1 us
    auto us = cycles_to_human(999, 1'000'000);
    KTEST_EXPECT_EQUAL(us.whole, 999u);
    KTEST_EXPECT_TRUE(us.unit[0] == 'u');
    auto ms = cycles_to_human(1500, 1'000'000);  // 1500 us = 1.50 ms
    KTEST_EXPECT_EQUAL(ms.whole, 1u);
    KTEST_EXPECT_EQUAL(ms.hundredths, 50u);
    KTEST_EXPECT_TRUE(ms.unit[0] == 'm');
    auto s = cycles_to_human(2'340'000, 1'000'000);  // 2.34 s
    KTEST_EXPECT_EQUAL(s.whole, 2u);
    KTEST_EXPECT_EQUAL(s.hundredths, 34u);
    KTEST_EXPECT_TRUE(s.unit[0] == 's');
    // large-cycle path must not overflow: 10^13 cycles at 5 GHz = 2000 s
    auto big = cycles_to_human(10'000'000'000'000ull, 5'000'000'000ull);
    KTEST_EXPECT_EQUAL(big.whole, 2000u);
}

#endif
