#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/ref>

using namespace kernel::testing;

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

KTEST(ktl_ref_default_is_null, "ktl/ref") {
    ktl::ref<test_target> r;
    KTEST_EXPECT_ALL(!r, r.get() == nullptr);
}

KTEST(ktl_ref_make_ref_creates_with_refcount_1, "ktl/ref") {
    bool destroyed = false;
    auto r         = ktl::make_ref<test_target>(&destroyed);
    KTEST_EXPECT_ALL(static_cast<bool>(r), r.get() != nullptr, r.ref_count() == 1, !destroyed);
}

KTEST(ktl_ref_copy_increments_refcount, "ktl/ref") {
    bool destroyed = false;
    auto r1        = ktl::make_ref<test_target>(&destroyed);
    auto r2        = r1;
    KTEST_EXPECT_ALL(r1.ref_count() == 2, r2.ref_count() == 2, r1.get() == r2.get(), !destroyed);
}

KTEST(ktl_ref_move_transfers_ownership, "ktl/ref") {
    bool destroyed = false;
    auto r1        = ktl::make_ref<test_target>(&destroyed);
    auto r2        = ktl::move(r1);
    KTEST_EXPECT_ALL(!r1, static_cast<bool>(r2), r2.ref_count() == 1, !destroyed);
}

KTEST(ktl_ref_destruction_releases, "ktl/ref") {
    bool destroyed = false;
    {
        auto r = ktl::make_ref<test_target>(&destroyed);
        KTEST_EXPECT_FALSE(destroyed);
    }
    KTEST_EXPECT_TRUE(destroyed);
}

KTEST(ktl_ref_last_copy_destroys, "ktl/ref") {
    bool destroyed = false;
    auto r1        = ktl::make_ref<test_target>(&destroyed);
    {
        auto r2 = r1;
        KTEST_EXPECT_TRUE(r1.ref_count() == 2);
    }
    KTEST_EXPECT_ALL(r1.ref_count() == 1, !destroyed);
    r1.reset();
    KTEST_EXPECT_TRUE(destroyed);
}

KTEST(ktl_ref_reset_nulls_out, "ktl/ref") {
    bool destroyed = false;
    auto r         = ktl::make_ref<test_target>(&destroyed);
    r.reset();
    KTEST_EXPECT_ALL(!r, r.get() == nullptr, destroyed);
}

KTEST(ktl_ref_copy_assignment, "ktl/ref") {
    bool d1 = false, d2 = false;
    auto r1 = ktl::make_ref<test_target>(&d1);
    auto r2 = ktl::make_ref<test_target>(&d2);
    r1      = r2;
    KTEST_EXPECT_ALL(d1, !d2, r1.get() == r2.get(), r1.ref_count() == 2);
}

KTEST(ktl_ref_self_assignment_safe, "ktl/ref") {
    bool destroyed = false;
    auto r         = ktl::make_ref<test_target>(&destroyed);
    auto* rp       = &r;
    *rp            = r;
    KTEST_EXPECT_ALL(!destroyed, r.ref_count() == 1);
}

KTEST(ktl_ref_equality_same_object, "ktl/ref") {
    bool destroyed = false;
    auto r1        = ktl::make_ref<test_target>(&destroyed);
    auto r2        = r1;
    KTEST_EXPECT_TRUE(r1 == r2);
}

KTEST(ktl_ref_equality_different_objects, "ktl/ref") {
    bool d1 = false, d2 = false;
    auto r1 = ktl::make_ref<test_target>(&d1);
    auto r2 = ktl::make_ref<test_target>(&d2);
    KTEST_EXPECT_TRUE(!(r1 == r2));
}

KTEST(ktl_ref_arrow_and_deref, "ktl/ref") {
    bool destroyed = false;
    auto r         = ktl::make_ref<test_target>(&destroyed);
    KTEST_EXPECT_ALL(r->destroyed == &destroyed, (*r).destroyed == &destroyed);
}

#endif
