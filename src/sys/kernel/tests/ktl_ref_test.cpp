#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/ref>

using namespace kernel::testing;

KTEST_MODULE("ktl/ref");

namespace {

struct test_target {
    bool* destroyed;
    explicit test_target(bool* flag) : destroyed(flag) {}
    ~test_target() {
        if (destroyed) { *destroyed = true; }
    }
    test_target(const test_target&)            = delete;
    test_target& operator=(const test_target&) = delete;
};

}  // namespace

KTEST_CASE(ktl_ref_construction_and_access) {
    // Default-constructed ref is null.
    ktl::ref<test_target> null_ref;
    KTEST_EXPECT_ALL(!null_ref, null_ref.get() == nullptr);

    // make_ref creates with refcount 1; -> and * reach the object.
    bool destroyed = false;
    auto r         = ktl::make_ref<test_target>(&destroyed);
    KTEST_EXPECT_ALL(static_cast<bool>(r), r.get() != nullptr, r.ref_count() == 1, !destroyed);
    KTEST_EXPECT_ALL(r->destroyed == &destroyed, (*r).destroyed == &destroyed);
}

KTEST_CASE(ktl_ref_copy_lifecycle) {
    bool destroyed = false;
    auto r1        = ktl::make_ref<test_target>(&destroyed);
    {
        auto r2 = r1;
        KTEST_EXPECT_ALL(r1.ref_count() == 2, r2.ref_count() == 2, r1.get() == r2.get(), !destroyed);
        KTEST_EXPECT_TRUE(r1 == r2);
    }
    // Dropping one copy leaves the object alive; releasing the last reference destroys it.
    KTEST_EXPECT_ALL(r1.ref_count() == 1, !destroyed);
    r1.reset();
    KTEST_EXPECT_TRUE(destroyed);
}

KTEST_CASE(ktl_ref_move_transfers_ownership) {
    bool destroyed = false;
    auto r1        = ktl::make_ref<test_target>(&destroyed);
    auto r2        = ktl::move(r1);
    KTEST_EXPECT_ALL(!r1, static_cast<bool>(r2), r2.ref_count() == 1, !destroyed);
}

KTEST_CASE(ktl_ref_release_destroys) {
    // Scope exit drops the last reference.
    bool destroyed = false;
    {
        auto scoped = ktl::make_ref<test_target>(&destroyed);
        KTEST_EXPECT_FALSE(destroyed);
    }
    KTEST_EXPECT_TRUE(destroyed);

    // reset() drops it explicitly and nulls the handle.
    destroyed = false;
    auto r    = ktl::make_ref<test_target>(&destroyed);
    r.reset();
    KTEST_EXPECT_ALL(!r, r.get() == nullptr, destroyed);
}

KTEST_CASE(ktl_ref_copy_assignment) {
    bool d1 = false, d2 = false;
    auto r1 = ktl::make_ref<test_target>(&d1);
    auto r2 = ktl::make_ref<test_target>(&d2);
    KTEST_EXPECT_TRUE(!(r1 == r2));  // distinct objects compare unequal
    r1 = r2;
    KTEST_EXPECT_ALL(d1, !d2, r1.get() == r2.get(), r1.ref_count() == 2);
}

KTEST_CASE(ktl_ref_self_assignment_safe) {
    bool destroyed = false;
    auto r         = ktl::make_ref<test_target>(&destroyed);
    auto* rp       = &r;
    *rp            = r;
    KTEST_EXPECT_ALL(!destroyed, r.ref_count() == 1);
}

#endif
