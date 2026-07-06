#include <kernel/testing/testing.h>
#include <stddef.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/rb_tree>

using namespace kernel::testing;

namespace {

struct item {
    int key = 0;
    ktl::rb_node hook;
};

// Heterogeneous "less": orders items by key and also answers item-vs-key probes so find/lower_bound/
// upper_bound can be driven by a bare int.
struct item_less {
    bool operator()(const item& a, const item& b) const { return a.key < b.key; }
    bool operator()(const item& a, int b) const { return a.key < b; }
    bool operator()(int a, const item& b) const { return a < b.key; }
};

using tree = ktl::rb_tree<item, &item::hook, item_less>;

// Fixed pool of nodes -- the tree never allocates, membership just references these.
template <size_t N> struct pool {
    item items[N];
    item& operator[](size_t i) { return items[i]; }
    void set_keys_identity() {
        for (size_t i = 0; i < N; ++i) { items[i].key = static_cast<int>(i); }
    }
};

}  // namespace

KTEST(ktl_rb_tree_empty, "ktl/rb_tree") {
    tree t;
    KTEST_EXPECT_TRUE(t.empty());
    KTEST_EXPECT_EQUAL(t.size(), 0u);
    KTEST_EXPECT_TRUE(t.begin() == t.end());
    KTEST_EXPECT_TRUE(t.validate());
    KTEST_EXPECT_TRUE(t.find(42) == t.end());
}

KTEST(ktl_rb_tree_ordered_iteration, "ktl/rb_tree") {
    pool<16> nodes;
    tree t;
    // Insert in a scrambled order; iteration must come out sorted.
    const int order[16] = {8, 3, 12, 1, 15, 7, 4, 0, 11, 9, 2, 14, 6, 5, 13, 10};
    for (int i : order) {
        nodes[static_cast<size_t>(i)].key = i;
        KTEST_REQUIRE_TRUE(t.insert(nodes[static_cast<size_t>(i)]));
        KTEST_REQUIRE_TRUE(t.validate());
    }
    KTEST_EXPECT_EQUAL(t.size(), 16u);

    int expected = 0;
    for (auto it = t.begin(); it != t.end(); ++it) {
        KTEST_EXPECT_EQUAL(it->key, expected);
        ++expected;
    }
    KTEST_EXPECT_EQUAL(expected, 16);
}

KTEST(ktl_rb_tree_duplicate_rejected, "ktl/rb_tree") {
    pool<2> nodes;
    nodes[0].key = 5;
    nodes[1].key = 5;
    tree t;
    KTEST_REQUIRE_TRUE(t.insert(nodes[0]));
    KTEST_EXPECT_FALSE(t.insert(nodes[1]));  // equal key rejected
    KTEST_EXPECT_EQUAL(t.size(), 1u);
    KTEST_EXPECT_TRUE(t.validate());
}

KTEST(ktl_rb_tree_find_and_bounds, "ktl/rb_tree") {
    pool<10> nodes;
    tree t;
    // Keys 0,10,20,...,90.
    for (size_t i = 0; i < 10; ++i) {
        nodes[i].key = static_cast<int>(i) * 10;
        KTEST_REQUIRE_TRUE(t.insert(nodes[i]));
    }

    KTEST_EXPECT_TRUE(t.find(30) != t.end());
    KTEST_EXPECT_EQUAL(t.find(30)->key, 30);
    KTEST_EXPECT_TRUE(t.find(35) == t.end());

    // lower_bound: first key >= probe.
    KTEST_EXPECT_EQUAL(t.lower_bound(30)->key, 30);
    KTEST_EXPECT_EQUAL(t.lower_bound(31)->key, 40);
    KTEST_EXPECT_EQUAL(t.lower_bound(0)->key, 0);
    KTEST_EXPECT_TRUE(t.lower_bound(90) != t.end());
    KTEST_EXPECT_TRUE(t.lower_bound(91) == t.end());

    // upper_bound: first key > probe.
    KTEST_EXPECT_EQUAL(t.upper_bound(30)->key, 40);
    KTEST_EXPECT_EQUAL(t.upper_bound(29)->key, 30);
    KTEST_EXPECT_TRUE(t.upper_bound(90) == t.end());

    // find_le: last key <= probe.
    KTEST_EXPECT_EQUAL(t.find_le(30)->key, 30);
    KTEST_EXPECT_EQUAL(t.find_le(35)->key, 30);
    KTEST_EXPECT_EQUAL(t.find_le(95)->key, 90);
    KTEST_EXPECT_TRUE(t.find_le(-1) == t.end());
}

KTEST(ktl_rb_tree_erase_and_reinsert, "ktl/rb_tree") {
    pool<32> nodes;
    nodes.set_keys_identity();
    tree t;
    for (size_t i = 0; i < 32; ++i) { KTEST_REQUIRE_TRUE(t.insert(nodes[i])); }
    KTEST_REQUIRE_TRUE(t.validate());

    // Erase every third node.
    for (size_t i = 0; i < 32; i += 3) {
        t.erase(nodes[i]);
        KTEST_REQUIRE_TRUE(t.validate());
    }
    for (size_t i = 0; i < 32; ++i) {
        bool should_be_present = (i % 3) != 0;
        KTEST_EXPECT_EQUAL(t.find(static_cast<int>(i)) != t.end(), should_be_present);
    }

    // Reinsert them; the tree returns to full and stays valid.
    for (size_t i = 0; i < 32; i += 3) {
        KTEST_REQUIRE_TRUE(t.insert(nodes[i]));
        KTEST_REQUIRE_TRUE(t.validate());
    }
    KTEST_EXPECT_EQUAL(t.size(), 32u);

    int expected = 0;
    for (auto& e : t) {
        KTEST_EXPECT_EQUAL(e.key, expected);
        ++expected;
    }
    KTEST_EXPECT_EQUAL(expected, 32);
}

KTEST(ktl_rb_tree_erase_root_and_all, "ktl/rb_tree") {
    pool<7> nodes;
    nodes.set_keys_identity();
    tree t;
    for (size_t i = 0; i < 7; ++i) { KTEST_REQUIRE_TRUE(t.insert(nodes[i])); }

    // Repeatedly erase the smallest remaining element until empty.
    for (int expected = 0; expected < 7; ++expected) {
        KTEST_REQUIRE_TRUE(t.begin() != t.end());
        KTEST_EXPECT_EQUAL(t.begin()->key, expected);
        t.erase(*t.begin());
        KTEST_REQUIRE_TRUE(t.validate());
    }
    KTEST_EXPECT_TRUE(t.empty());
    KTEST_EXPECT_TRUE(t.begin() == t.end());
}

KTEST(ktl_rb_tree_mixed_workload, "ktl/rb_tree") {
    // Deterministic pseudo-random churn validated after every mutation, mirroring the fuzz oracle at a
    // fixed seed so the host tier alone exercises the balance paths.
    constexpr size_t kMax = 128;
    pool<kMax> nodes;
    for (size_t i = 0; i < kMax; ++i) { nodes[i].key = static_cast<int>(i); }

    bool present[kMax] = {};
    tree t;
    uint32_t state = 0x1234567u;
    for (int step = 0; step < 4000; ++step) {
        state       = state * 1664525u + 1013904223u;
        size_t idx  = (state >> 8) % kMax;
        bool insert = ((state >> 1) & 1u) != 0;

        if (insert && !present[idx]) {
            KTEST_REQUIRE_TRUE(t.insert(nodes[idx]));
            present[idx] = true;
        } else if (!insert && present[idx]) {
            t.erase(nodes[idx]);
            present[idx] = false;
        } else if (present[idx]) {
            // Redundant insert must be rejected; erase-of-absent is simply skipped.
            KTEST_EXPECT_FALSE(t.insert(nodes[idx]));
        }
        KTEST_REQUIRE_TRUE(t.validate());
    }

    // Final contents match the model set, in order.
    size_t model_count = 0;
    for (size_t i = 0; i < kMax; ++i) { model_count += present[i] ? 1 : 0; }
    KTEST_EXPECT_EQUAL(t.size(), model_count);

    int prev = -1;
    for (auto& e : t) {
        KTEST_EXPECT_TRUE(e.key > prev);
        KTEST_EXPECT_TRUE(present[static_cast<size_t>(e.key)]);
        prev = e.key;
    }
}

#endif  // CONFIG_KERNEL_TESTING
