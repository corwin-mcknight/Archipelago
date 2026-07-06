#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/json_escape.h>
#include <kernel/log.h>
#include <kernel/log_ring.h>

#include <ktl/string_view>

using namespace kernel::testing;
using kernel::log_level;
using kernel::log_message;
using kernel::log_ring;

// A reserved-then-published message drains in sequence order.
KTEST(log_ring_reserve_publish_drain_order, "kernel/log_ring") {
    log_ring<int, 4> ring;
    for (int i = 0; i < 3; i++) {
        uint64_t seq;
        int* p = ring.reserve(seq);
        KTEST_REQUIRE_TRUE(p != nullptr);
        *p = i * 10;
        ring.publish(seq);
    }

    int out[4];
    int n = 0;
    ring.drain([&](const int& v) { out[n++] = v; });
    KTEST_EXPECT_ALL(n == 3, out[0] == 0, out[1] == 10, out[2] == 20);
}

// A full ring fails to log without consuming a sequence, and counts the drop.
KTEST(log_ring_fail_to_log_when_full, "kernel/log_ring") {
    log_ring<int, 4> ring;
    for (int i = 0; i < 4; i++) {
        uint64_t s;
        int* p = ring.reserve(s);
        KTEST_REQUIRE_TRUE(p != nullptr);
        *p = i;
        ring.publish(s);
    }

    uint64_t s;
    int* p = ring.reserve(s);  // ring full
    KTEST_EXPECT_ALL(p == nullptr, ring.dropped() == 1, ring.size() == 4);
}

// Draining frees slots so reservation succeeds again; the earlier drop still counts.
KTEST(log_ring_reuse_after_drain, "kernel/log_ring") {
    log_ring<int, 2> ring;
    uint64_t s;
    int* p = ring.reserve(s);
    *p     = 1;
    ring.publish(s);
    p  = ring.reserve(s);
    *p = 2;
    ring.publish(s);
    KTEST_REQUIRE_TRUE(ring.reserve(s) == nullptr);  // full

    int sum = 0;
    ring.drain([&](const int& v) { sum += v; });
    KTEST_REQUIRE_TRUE(sum == 3);

    p = ring.reserve(s);  // space again
    KTEST_REQUIRE_TRUE(p != nullptr);
    KTEST_EXPECT_EQUAL((int)ring.dropped(), 1);
    *p = 9;
    ring.publish(s);

    int last = -1;
    ring.drain([&](const int& v) { last = v; });
    KTEST_EXPECT_EQUAL(last, 9);
}

// The ordered drain stops at an in-progress (reserved-but-unpublished) slot and resumes once
// it is published.
KTEST(log_ring_drain_stops_at_in_progress, "kernel/log_ring") {
    log_ring<int, 4> ring;
    uint64_t s0, s1;
    int* p0 = ring.reserve(s0);
    *p0     = 100;  // reserved, NOT published -> WRITING
    int* p1 = ring.reserve(s1);
    *p1     = 200;
    ring.publish(s1);  // published, but behind the hole at s0

    int n = 0;
    ring.drain([&](const int&) { n++; });
    KTEST_EXPECT_EQUAL(n, 0);  // ordered: cannot emit past the in-progress slot

    ring.publish(s0);
    int out[4];
    int k = 0;
    ring.drain([&](const int& v) { out[k++] = v; });
    KTEST_EXPECT_ALL(k == 2, out[0] == 100, out[1] == 200);
}

// After wrap, the history scan returns only the retained window, in order, with no stale entries.
KTEST(log_ring_for_each_history_window, "kernel/log_ring") {
    log_ring<int, 4> ring;
    for (int i = 0; i < 6; i++) {
        uint64_t s;
        int* p = ring.reserve(s);
        if (p == nullptr) {
            ring.drain([](const int&) {});  // make room
            p = ring.reserve(s);
        }
        KTEST_REQUIRE_TRUE(p != nullptr);
        *p = i;
        ring.publish(s);
    }

    int vals[8];
    int n = 0;
    ring.for_each(0, [&](const int& v) { vals[n++] = v; });
    KTEST_REQUIRE_TRUE(n >= 1 && n <= 4);
    for (int i = 1; i < n; i++) { KTEST_REQUIRE_TRUE(vals[i] > vals[i - 1]); }  // strictly increasing
    KTEST_EXPECT_EQUAL(vals[n - 1], 5);                                         // newest message retained
}

// The crash scan emits committed slots and flags in-progress ones.
KTEST(log_ring_crash_scan_flags_in_progress, "kernel/log_ring") {
    log_ring<int, 4> ring;
    uint64_t s0, s1;
    int* p0 = ring.reserve(s0);
    *p0     = 7;
    ring.publish(s0);
    int* p1    = ring.reserve(s1);
    *p1        = 8;  // WRITING, not published

    int ready  = 0;
    int inprog = 0;
    ring.crash_scan([&](const int&, bool in_progress) {
        if (in_progress) {
            inprog++;
        } else {
            ready++;
        }
    });
    KTEST_EXPECT_ALL(ready == 1, inprog == 1);
}

// Headline migration property: flushed slots are RETAINED as history, not discarded. A drained
// (all-FLUSHED) ring must still replay the last Capacity messages in order via for_each.
KTEST(log_ring_retains_flushed_history, "kernel/log_ring") {
    log_ring<int, 4> ring;
    for (int i = 0; i < 4; i++) {
        uint64_t s;
        int* p = ring.reserve(s);
        *p     = i + 1;
        ring.publish(s);
    }
    int drained = 0;
    ring.drain([&](const int&) { drained++; });
    KTEST_REQUIRE_TRUE(drained == 4 && ring.size() == 0);  // everything FLUSHED, nothing pending

    int vals[8];
    int n = 0;
    ring.for_each(0, [&](const int& v) { vals[n++] = v; });
    KTEST_EXPECT_ALL(n == 4, vals[0] == 1, vals[1] == 2, vals[2] == 3, vals[3] == 4);
}

// for_each(min_seq) filters below min_seq and returns the one-past-highest cursor.
KTEST(log_ring_for_each_min_seq_and_cursor, "kernel/log_ring") {
    log_ring<int, 8> ring;
    for (int i = 0; i < 5; i++) {
        uint64_t s;
        int* p = ring.reserve(s);
        *p     = i;
        ring.publish(s);
    }

    int vals[8];
    int n           = 0;
    uint64_t cursor = ring.for_each(2, [&](const int& v) { vals[n++] = v; });
    KTEST_EXPECT_ALL(n == 3, vals[0] == 2, vals[1] == 3, vals[2] == 4, cursor == 5);

    int n2           = 0;
    uint64_t cursor2 = ring.for_each(cursor, [&](const int&) { n2++; });  // nothing new
    KTEST_EXPECT_ALL(n2 == 0, cursor2 == 5);

    uint64_t c3 = ring.for_each(100, [&](const int&) {});  // past the end
    KTEST_EXPECT_EQUAL((int)c3, 100);
}

// dropped() accumulates across multiple full-ring reservation failures.
KTEST(log_ring_dropped_accumulates, "kernel/log_ring") {
    log_ring<int, 2> ring;
    uint64_t s;
    int* p = ring.reserve(s);
    *p     = 1;
    ring.publish(s);
    p  = ring.reserve(s);
    *p = 2;
    ring.publish(s);  // full
    for (int i = 0; i < 3; i++) { KTEST_REQUIRE_TRUE(ring.reserve(s) == nullptr); }
    KTEST_EXPECT_EQUAL((int)ring.dropped(), 3);
}

// crash_scan reconstructs sequence order across a wrap and flags the in-progress slot.
KTEST(log_ring_crash_scan_after_wrap, "kernel/log_ring") {
    log_ring<int, 4> ring;
    for (int i = 0; i < 6; i++) {
        uint64_t s;
        int* p = ring.reserve(s);
        if (p == nullptr) {
            ring.drain([](const int&) {});
            p = ring.reserve(s);
        }
        *p = i;
        ring.publish(s);
    }
    uint64_t sw;
    int* pw       = ring.reserve(sw);  // WRITING, not published
    *pw           = 99;

    int committed = 0;
    int inprog    = 0;
    int last      = -1;
    ring.crash_scan([&](const int& v, bool in_progress) {
        if (in_progress) {
            inprog++;
        } else {
            committed++;
            last = v;
        }
    });
    KTEST_EXPECT_ALL(inprog == 1, committed >= 1, last == 5);
}

// The history scan skips an in-progress slot but still shows committed neighbors in order.
KTEST(log_ring_for_each_skips_in_progress, "kernel/log_ring") {
    log_ring<int, 4> ring;
    uint64_t s0, s1, s2;
    int* p0 = ring.reserve(s0);
    *p0     = 10;
    ring.publish(s0);
    int* p1 = ring.reserve(s1);
    *p1     = 20;  // WRITING (in-progress)
    int* p2 = ring.reserve(s2);
    *p2     = 30;
    ring.publish(s2);

    int vals[8];
    int n = 0;
    ring.for_each(0, [&](const int& v) { vals[n++] = v; });
    KTEST_EXPECT_ALL(n == 2, vals[0] == 10, vals[1] == 30);
}

// log_message packs sequence (low 60 bits) and level (top 4) and round-trips, including a
// sequence right at the 60-bit boundary that must not bleed into the level nibble.
KTEST(log_message_seq_level_packing, "kernel/log") {
    log_message a(123, log_level::warn, 0xABCDEF, "x");
    KTEST_EXPECT_ALL(a.sequence() == 0xABCDEF, a.level() == log_level::warn);

    uint64_t big = (static_cast<uint64_t>(1) << 60) - 1;
    log_message b(0, log_level::error, big, "y");
    KTEST_EXPECT_ALL(b.sequence() == big, b.level() == log_level::error);
}

// Copy/move construction and assignment preserve the packed level/sequence
// word and the text.
KTEST(log_message_copy_and_move, "kernel/log") {
    log_message src(42, log_level::info, 7, ktl::string_view("hello"));
    log_message from_fixed(43, log_level::debug, 8, src.text);

    log_message copied(src);
    KTEST_EXPECT_ALL(copied.sequence() == 7, copied.timestamp == 42, ktl::string_view(copied.text) == "hello");

    log_message moved(ktl::move(copied));
    KTEST_EXPECT_ALL(moved.sequence() == 7, ktl::string_view(moved.text) == "hello");

    log_message assigned;
    assigned = src;
    KTEST_EXPECT_ALL(assigned.sequence() == 7, ktl::string_view(assigned.text) == "hello");

    log_message move_assigned;
    move_assigned = ktl::move(assigned);
    KTEST_EXPECT_ALL(move_assigned.sequence() == 7, ktl::string_view(move_assigned.text) == "hello",
                     from_fixed.sequence() == 8);
}

// The default write_string falls back to byte-at-a-time output.
KTEST(logging_device_write_string_default, "kernel/log") {
    struct capture_device : kernel::driver::logging_device {
        char buf[16] = {};
        int n        = 0;
        const char* name() const override { return "capture"; }
        void init() override {}
        void write_byte(char c) override { buf[n++] = c; }
    } dev;
    dev.write_string("abc");
    KTEST_EXPECT_ALL(dev.n == 3, ktl::string_view(dev.buf) == "abc");
}

// A fresh log has dropped nothing.
KTEST(system_log_dropped_starts_zero, "kernel/log") {
    static kernel::system_log log;
    KTEST_EXPECT_EQUAL(log.dropped(), static_cast<uint64_t>(0));
}

// JSON escaping covers quote, backslash, the common control chars, and \u00XX for other controls.
KTEST(json_escape_specials, "kernel/json_escape") {
    char buf[64];
    int n = 0;
    kernel::write_json_escaped([&](char c) { buf[n++] = c; }, "a\"b\\c\nd\te\x01");
    buf[n] = '\0';
    KTEST_EXPECT_TRUE(ktl::string_view(buf) == "a\\\"b\\\\c\\nd\\te\\u0001");
}

#endif  // CONFIG_KERNEL_TESTING
