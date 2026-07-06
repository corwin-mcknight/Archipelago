// libFuzzer target for ktl::rb_tree (host fuzz lane).
//
// Each input byte is one operation over a fixed pool of N keyed nodes: the low bits pick a node, the
// top bits pick insert / erase / find. A bool array is the oracle model. After every mutation the tree
// must (1) pass validate() -- RB invariants and BST order -- and (2) iterate in exactly the sorted set
// of currently-present keys. find() must agree with the model. Any divergence traps for libFuzzer.
//
// Freestanding like the code under test: no libc containers, only the intrusive tree and a stack array.

#include <stddef.h>
#include <stdint.h>

#include <ktl/rb_tree>

namespace {

constexpr size_t kNodes = 64;

struct item {
    int key = 0;
    ktl::rb_node hook;
};

struct item_less {
    bool operator()(const item& a, const item& b) const { return a.key < b.key; }
    bool operator()(const item& a, int b) const { return a.key < b; }
    bool operator()(int a, const item& b) const { return a < b.key; }
};

using tree = ktl::rb_tree<item, &item::hook, item_less>;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    item nodes[kNodes];
    for (size_t i = 0; i < kNodes; ++i) { nodes[i].key = static_cast<int>(i); }
    bool present[kNodes] = {};

    tree t;

    for (size_t i = 0; i < size; ++i) {
        uint8_t byte = data[i];
        size_t idx   = byte % kNodes;
        int op       = (byte >> 6) & 0x3;

        if (op == 0) {  // insert
            bool inserted = t.insert(nodes[idx]);
            if (inserted == present[idx]) { __builtin_trap(); }  // insert result must equal "was absent"
            present[idx] = true;
        } else if (op == 1) {  // erase (only if present; erasing an absent node is undefined for the tree)
            if (present[idx]) {
                t.erase(nodes[idx]);
                present[idx] = false;
            }
        } else {  // find -- must agree with the model, no mutation
            bool found = t.find(static_cast<int>(idx)) != t.end();
            if (found != present[idx]) { __builtin_trap(); }
            continue;  // no mutation, skip the post-mutation checks
        }

        if (!t.validate()) { __builtin_trap(); }

        // In-order traversal must equal the sorted set of present keys.
        size_t expected = 0;
        while (expected < kNodes && !present[expected]) { ++expected; }
        for (auto& e : t) {
            if (expected >= kNodes || e.key != static_cast<int>(expected)) { __builtin_trap(); }
            ++expected;
            while (expected < kNodes && !present[expected]) { ++expected; }
        }
        if (expected != kNodes) { __builtin_trap(); }  // model had keys the tree did not yield
    }

    return 0;
}
