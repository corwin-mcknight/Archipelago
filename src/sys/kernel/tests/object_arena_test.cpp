#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/mm/object_arena.h>

using namespace kernel::mm;

KTEST_MODULE("mm/object_arena");

namespace {

struct three_words {
    uint64_t a, b, c;
};

bool slot_is_zero(const void* p, size_t n) {
    const auto* bytes = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) {
        if (bytes[i] != 0) { return false; }
    }
    return true;
}

}  // namespace

// The arena's contract in one pass: slots come out zero, exact-size (a 24-byte object costs 24
// bytes, not a 32-byte class), and a freed slot is scrubbed before it can be observed again --
// the lowest-slot bitmap scan hands the same address back, so the scrub is directly visible.
KTEST_CASE(object_arena_zero_on_free_roundtrip) {
    object_arena arena("test-words", sizeof(three_words), alignof(three_words));

    auto s = arena.stats();
    KTEST_EXPECT_ALL(s.object_size == 24, s.slot_size == 24);

    auto* first  = static_cast<three_words*>(arena.alloc());
    // The anchor keeps the slab non-empty across the free below; an empty slab returns its page
    // immediately, which would make the same-address check meaningless.
    auto* anchor = static_cast<three_words*>(arena.alloc());
    KTEST_REQUIRE_TRUE(first != nullptr && anchor != nullptr);
    KTEST_EXPECT_TRUE(slot_is_zero(first, sizeof(three_words)));
    first->a = 0xDEAD;
    first->c = 0xBEEF;
    arena.free(first);

    auto* again = static_cast<three_words*>(arena.alloc());
    KTEST_EXPECT_TRUE(again == first);
    KTEST_EXPECT_TRUE(slot_is_zero(again, sizeof(three_words)));
    arena.free(again);
    arena.free(anchor);
}

// Slabs grow one page at a time as slots run out and pages return to the source when a slab
// empties; the registry sees the arena while it lives and not after.
KTEST_CASE(object_arena_slab_lifecycle) {
    constexpr size_t BIG = 1000;  // few slots per page, so two slabs are cheap to force
    void* held[16]       = {};
    size_t per_slab      = 0;
    {
        object_arena arena("test-big", BIG, 8);
        per_slab     = arena.stats().capacity;  // 0 until a slab exists

        size_t count = 0;
        while (count < 16) {
            held[count] = arena.alloc();
            KTEST_REQUIRE_TRUE(held[count] != nullptr);
            count++;
            if (arena.stats().slabs == 2) { break; }
        }
        auto grown = arena.stats();
        KTEST_EXPECT_ALL(grown.slabs == 2, grown.live == count, grown.capacity == 2 * (count - 1));
        per_slab = count - 1;  // the last alloc is what forced slab two
        KTEST_EXPECT_TRUE(per_slab >= 2);

        for (size_t i = 0; i < count; i++) { arena.free(held[i]); }
        auto drained = arena.stats();
        KTEST_EXPECT_ALL(drained.slabs == 0, drained.live == 0, drained.free_calls == count);

        bool registered = false;
        for (auto* a = object_arena::first_arena(); a != nullptr; a = a->next_arena()) {
            if (a == &arena) { registered = true; }
        }
        KTEST_EXPECT_TRUE(registered);
    }
    for (auto* a = object_arena::first_arena(); a != nullptr; a = a->next_arena()) {
        KTEST_EXPECT_TRUE(a->stats().object_size != BIG);
    }
}

#endif
