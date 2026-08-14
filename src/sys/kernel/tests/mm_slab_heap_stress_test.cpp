#include <kernel/mm/slab_heap.h>
#include <kernel/testing/testing.h>

#include <ktl/vector>

using namespace kernel::mm;

KTEST_MODULE("mm/slab_heap_stress");

namespace {

// Deterministic xorshift so failures replay; seeded per case.
struct rng {
    uint64_t state;
    uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

struct live_alloc {
    void* ptr;
    size_t size;
    uint8_t tag;
};

void fill(const live_alloc& a) { __builtin_memset(a.ptr, a.tag, a.size); }

// The oracle: every byte of an allocation must still hold its tag when it is freed, no matter
// what the allocator did for neighbours in between. Full scan below one page, strided above so
// the case stays fast; first and last byte always checked.
bool intact(const live_alloc& a) {
    const uint8_t* bytes = static_cast<const uint8_t*>(a.ptr);
    if (bytes[0] != a.tag || bytes[a.size - 1] != a.tag) { return false; }
    size_t stride = a.size <= 4096 ? 1 : 251;
    for (size_t i = 0; i < a.size; i += stride) {
        if (bytes[i] != a.tag) { return false; }
    }
    return true;
}

}  // namespace

// Randomized churn across every class and the large path, with content verification on every
// free. ASan supervises the whole run, so any slot overlap, header clobber, or stale-freelist
// reuse surfaces as a hard failure rather than silent corruption.
KTEST_CASE(slab_heap_stress_randomized_oracle) {
    rng random{0x9E3779B97F4A7C15ull};
    ktl::vector<live_alloc> live;
    constexpr size_t OPS      = 40000;
    constexpr size_t MAX_LIVE = 1500;

    for (size_t op = 0; op < OPS; ++op) {
        bool do_alloc = live.size() < 16 || (live.size() < MAX_LIVE && random.next() % 100 < 55);
        if (do_alloc) {
            size_t shape = random.next() % 100;
            size_t size;
            if (shape < 70) {
                size = 1 + random.next() % 1024;  // every slab class
            } else if (shape < 90) {
                size = 1025 + random.next() % 3072;  // one-page large runs
            } else {
                size = 4097 + random.next() % (4 * 4096);  // multi-page large runs
            }
            void* p = g_slab_heap.alloc(size);
            KTEST_REQUIRE_TRUE(p != nullptr);
            live_alloc a{p, size, static_cast<uint8_t>(random.next() | 1)};
            fill(a);
            KTEST_REQUIRE_TRUE(live.push_back(a));
        } else {
            size_t index = random.next() % live.size();
            KTEST_REQUIRE_VALUE(victim, live.at(index));
            KTEST_REQUIRE_TRUE(intact(victim));
            g_slab_heap.free(victim.ptr);
            live.swap_remove(index);
        }
    }

    for (size_t i = 0; i < live.size(); ++i) {
        KTEST_REQUIRE_TRUE(intact(live[i]));
        g_slab_heap.free(live[i].ptr);
    }

    auto stats  = g_slab_heap.stats();
    size_t held = stats.large_allocs;
    for (const auto& cls : stats.classes) {
        held += cls.live_objects;
        KTEST_EXPECT_TRUE(cls.slabs <= 1);
    }
    KTEST_EXPECT_ALL(held == 0, stats.failures == 0, stats.alloc_calls == stats.free_calls);
}

// Class-boundary saturation: for every class, fill exactly to the slab edge, one past it, then
// free in an order chosen to exercise head/middle/tail unlinks of the partial list.
KTEST_CASE(slab_heap_stress_boundary_saturation) {
    constexpr size_t BOUNDARY_SIZES[] = {16, 17, 32, 33, 64, 65, 128, 129, 256, 257, 512, 513, 1023, 1024};
    for (size_t s = 0; s < sizeof(BOUNDARY_SIZES) / sizeof(BOUNDARY_SIZES[0]); ++s) {
        size_t size = BOUNDARY_SIZES[s];
        void* probe = g_slab_heap.alloc(size);
        KTEST_REQUIRE_TRUE(probe != nullptr);

        // Learn the class's capacity from stats, then push three slabs' worth of allocations.
        auto st         = g_slab_heap.stats();
        size_t per_slab = 0;
        for (const auto& cls : st.classes) {
            if (cls.live_objects != 0) { per_slab = cls.free_slots + cls.live_objects; }
        }
        KTEST_REQUIRE_TRUE(per_slab != 0);

        ktl::vector<void*> held;
        KTEST_REQUIRE_TRUE(held.push_back(probe));
        for (size_t i = 1; i < 3 * per_slab; ++i) {
            void* p = g_slab_heap.alloc(size);
            KTEST_REQUIRE_TRUE(p != nullptr);
            __builtin_memset(p, 0x5A, size);
            KTEST_REQUIRE_TRUE(held.push_back(p));
        }

        // Free every third, then every remaining even, then the rest: partial-list surgery from
        // full slabs, middle slabs, and empties, in mixed order.
        for (size_t i = 0; i < held.size(); i += 3) { g_slab_heap.free(held[i]); }
        for (size_t i = 0; i < held.size(); ++i) {
            if (i % 3 != 0 && i % 2 == 0) { g_slab_heap.free(held[i]); }
        }
        for (size_t i = 0; i < held.size(); ++i) {
            if (i % 3 != 0 && i % 2 != 0) { g_slab_heap.free(held[i]); }
        }
    }

    auto stats  = g_slab_heap.stats();
    size_t held = stats.large_allocs;
    for (const auto& cls : stats.classes) { held += cls.live_objects; }
    KTEST_EXPECT_TRUE(held == 0);
}

// Same-page slot reuse must never hand out overlapping storage: allocate a full slab of one
// class, free alternating slots, reallocate them, and verify no pointer appears twice and every
// slot's contents survive.
KTEST_CASE(slab_heap_stress_alternating_reuse) {
    constexpr size_t SIZE = 96;  // class 128
    ktl::vector<live_alloc> held;
    for (size_t i = 0; i < 31; ++i) {  // one slab of the 128 class holds 31 slots
        void* p = g_slab_heap.alloc(SIZE);
        KTEST_REQUIRE_TRUE(p != nullptr);
        live_alloc a{p, SIZE, static_cast<uint8_t>(i + 1)};
        fill(a);
        KTEST_REQUIRE_TRUE(held.push_back(a));
    }
    for (size_t i = 0; i < held.size(); i += 2) {
        KTEST_REQUIRE_TRUE(intact(held[i]));
        g_slab_heap.free(held[i].ptr);
    }
    for (size_t i = 0; i < held.size(); i += 2) {
        void* p = g_slab_heap.alloc(SIZE);
        KTEST_REQUIRE_TRUE(p != nullptr);
        for (size_t j = 1; j < held.size(); j += 2) { KTEST_REQUIRE_TRUE(p != held[j].ptr); }
        held[i] = {p, SIZE, static_cast<uint8_t>(0x80 + i)};
        fill(held[i]);
    }
    for (size_t i = 0; i < held.size(); ++i) {
        KTEST_REQUIRE_TRUE(intact(held[i]));
        g_slab_heap.free(held[i].ptr);
    }
}
