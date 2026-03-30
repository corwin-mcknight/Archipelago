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
    KTEST_EXPECT_TRUE(!r);
    KTEST_EXPECT_TRUE(r.get() == nullptr);
}

KTEST(ktl_ref_make_ref_creates_with_refcount_1, "ktl/ref") {
    bool destroyed = false;
    auto r         = ktl::make_ref<test_target>(&destroyed);
    KTEST_EXPECT_TRUE(static_cast<bool>(r));
    KTEST_EXPECT_TRUE(r.get() != nullptr);
    KTEST_EXPECT_TRUE(r.ref_count() == 1);
    KTEST_EXPECT_TRUE(!destroyed);
}

KTEST(ktl_ref_copy_increments_refcount, "ktl/ref") {
    bool destroyed = false;
    auto r1        = ktl::make_ref<test_target>(&destroyed);
    auto r2        = r1;
    KTEST_EXPECT_TRUE(r1.ref_count() == 2);
    KTEST_EXPECT_TRUE(r2.ref_count() == 2);
    KTEST_EXPECT_TRUE(r1.get() == r2.get());
    KTEST_EXPECT_TRUE(!destroyed);
}

KTEST(ktl_ref_move_transfers_ownership, "ktl/ref") {
    bool destroyed = false;
    auto r1        = ktl::make_ref<test_target>(&destroyed);
    auto r2        = ktl::move(r1);
    KTEST_EXPECT_TRUE(!r1);
    KTEST_EXPECT_TRUE(static_cast<bool>(r2));
    KTEST_EXPECT_TRUE(r2.ref_count() == 1);
    KTEST_EXPECT_TRUE(!destroyed);
}

KTEST(ktl_ref_destruction_releases, "ktl/ref") {
    bool destroyed = false;
    {
        auto r = ktl::make_ref<test_target>(&destroyed);
        KTEST_EXPECT_TRUE(!destroyed);
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
    KTEST_EXPECT_TRUE(r1.ref_count() == 1);
    KTEST_EXPECT_TRUE(!destroyed);
    r1.reset();
    KTEST_EXPECT_TRUE(destroyed);
}

KTEST(ktl_ref_reset_nulls_out, "ktl/ref") {
    bool destroyed = false;
    auto r         = ktl::make_ref<test_target>(&destroyed);
    r.reset();
    KTEST_EXPECT_TRUE(!r);
    KTEST_EXPECT_TRUE(r.get() == nullptr);
    KTEST_EXPECT_TRUE(destroyed);
}

KTEST(ktl_ref_copy_assignment, "ktl/ref") {
    bool d1 = false;
    bool d2 = false;
    auto r1 = ktl::make_ref<test_target>(&d1);
    auto r2 = ktl::make_ref<test_target>(&d2);
    r1      = r2;
    KTEST_EXPECT_TRUE(d1);
    KTEST_EXPECT_TRUE(!d2);
    KTEST_EXPECT_TRUE(r1.get() == r2.get());
    KTEST_EXPECT_TRUE(r1.ref_count() == 2);
}

KTEST(ktl_ref_self_assignment_safe, "ktl/ref") {
    bool destroyed = false;
    auto r         = ktl::make_ref<test_target>(&destroyed);
    auto* rp       = &r;
    *rp            = r;
    KTEST_EXPECT_TRUE(!destroyed);
    KTEST_EXPECT_TRUE(r.ref_count() == 1);
}

KTEST(ktl_ref_equality_same_object, "ktl/ref") {
    bool destroyed = false;
    auto r1        = ktl::make_ref<test_target>(&destroyed);
    auto r2        = r1;
    KTEST_EXPECT_TRUE(r1 == r2);
}

KTEST(ktl_ref_equality_different_objects, "ktl/ref") {
    bool d1 = false;
    bool d2 = false;
    auto r1 = ktl::make_ref<test_target>(&d1);
    auto r2 = ktl::make_ref<test_target>(&d2);
    KTEST_EXPECT_TRUE(!(r1 == r2));
}

KTEST(ktl_ref_arrow_and_deref, "ktl/ref") {
    bool destroyed = false;
    auto r         = ktl::make_ref<test_target>(&destroyed);
    KTEST_EXPECT_TRUE(r->destroyed == &destroyed);
    KTEST_EXPECT_TRUE((*r).destroyed == &destroyed);
}

#endif
