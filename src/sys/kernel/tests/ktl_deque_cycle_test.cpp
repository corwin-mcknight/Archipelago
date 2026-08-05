#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/deque>

KTEST_MODULE("ktl/deque_cycle");

// The run queue's exact regime: a bounded live population cycled through push_back/pop_front far
// past the block capacity, so blocks fill physically, drain, and rotate through compaction. This
// is the pattern that must never allocate after reserve() and must never lose or reorder an
// element -- the scheduler pushes from interrupt context on the strength of that promise.
KTEST_CASE(deque_cycle_through_block_rotation) {
    ktl::deque<uint64_t> queue;
    KTEST_REQUIRE_TRUE(queue.reserve(12));

    uint64_t next_in  = 0;
    uint64_t next_out = 0;
    for (size_t i = 0; i < 8; ++i) { KTEST_REQUIRE_TRUE(queue.push_back(next_in++)); }

    // FIFO invariant across 10k cycles: values come out exactly in the order they went in.
    for (size_t cycle = 0; cycle < 10000; ++cycle) {
        KTEST_REQUIRE_VALUE(out, queue.pop_front());
        KTEST_REQUIRE_EQUAL(out, next_out);
        next_out++;
        KTEST_REQUIRE_TRUE(queue.push_back(next_in++));
        KTEST_REQUIRE_EQUAL(queue.size(), 8u);
    }

    // Drain and refill through empty repeatedly -- the single-block reset path.
    for (size_t round = 0; round < 100; ++round) {
        while (queue.size() != 0) {
            KTEST_REQUIRE_VALUE(out, queue.pop_front());
            KTEST_REQUIRE_EQUAL(out, next_out);
            next_out++;
        }
        for (size_t i = 0; i < 11; ++i) { KTEST_REQUIRE_TRUE(queue.push_back(next_in++)); }
    }
    while (queue.size() != 0) {
        KTEST_REQUIRE_VALUE(out, queue.pop_front());
        KTEST_REQUIRE_EQUAL(out, next_out);
        next_out++;
    }
    KTEST_EXPECT_EQUAL(next_in, next_out);
}

// Same cycling with a growing-then-shrinking population, crossing several block boundaries in
// both directions.
KTEST_CASE(deque_cycle_population_waves) {
    ktl::deque<uint64_t> queue;
    KTEST_REQUIRE_TRUE(queue.reserve(64));

    uint64_t next_in  = 0;
    uint64_t next_out = 0;
    for (size_t wave = 1; wave <= 60; ++wave) {
        for (size_t i = 0; i < wave; ++i) { KTEST_REQUIRE_TRUE(queue.push_back(next_in++)); }
        size_t drain = wave > 1 ? wave - 1 : 1;
        for (size_t i = 0; i < drain && queue.size() != 0; ++i) {
            KTEST_REQUIRE_VALUE(out, queue.pop_front());
            KTEST_REQUIRE_EQUAL(out, next_out);
            next_out++;
        }
    }
    while (queue.size() != 0) {
        KTEST_REQUIRE_VALUE(out, queue.pop_front());
        KTEST_REQUIRE_EQUAL(out, next_out);
        next_out++;
    }
    KTEST_EXPECT_EQUAL(next_in, next_out);
}

#endif
