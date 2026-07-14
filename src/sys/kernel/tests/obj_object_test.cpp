#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/test_objects.h>

using namespace kernel::testing;
using namespace kernel::obj;

KTEST_MODULE_WITH_INIT("obj/object", obj_object_init);

static void obj_object_init() { register_all_test_types(); }

KTEST_CASE(obj_object_identity_and_metadata) {
    auto a = ktl::make_ref<TestObjA>();
    auto b = ktl::make_ref<TestObjA>();
    // Monotonic implies unique.
    KTEST_EXPECT_TRUE(b->id() > a->id());
    KTEST_EXPECT_TRUE(a->type_id() == TEST_TYPE_A);
    KTEST_EXPECT_TRUE(a->name() == nullptr);
    a->set_name("my_test_object");
    KTEST_EXPECT_TRUE(a->name() != nullptr);
}

KTEST_CASE(obj_object_signal_set_and_clear) {
    auto obj = ktl::make_ref<TestObjA>();
    KTEST_EXPECT_TRUE(obj->signals() == 0);
    obj->signal_set(0x01);
    obj->signal_set(0x10);
    KTEST_EXPECT_TRUE(obj->signals() == 0x11);
    obj->signal_clear(0x01);
    KTEST_EXPECT_TRUE(obj->signals() == 0x10);
}

KTEST_CASE(obj_object_refcount_and_destruction) {
    uint32_t live_before = g_type_registry.live_count(TEST_TYPE_A);
    bool destroyed       = false;
    auto r1              = ktl::make_ref<TestObjA>(&destroyed);
    KTEST_EXPECT_TRUE(g_type_registry.live_count(TEST_TYPE_A) == live_before + 1);
    {
        auto r2 = r1;
        KTEST_EXPECT_TRUE(r1.ref_count() == 2);
    }
    KTEST_EXPECT_ALL(r1.ref_count() == 1, !destroyed);
    r1.reset();
    KTEST_EXPECT_ALL(destroyed, g_type_registry.live_count(TEST_TYPE_A) == live_before);
}

// Boot-time registration: obj_init installs the built-in object types.
// Fork-per-test isolation makes the one-shot registration safe to run here.
KTEST(obj_init_registers_builtin_types, "obj/object") {
    kernel::obj::obj_init();
    KTEST_EXPECT_TRUE(g_type_registry.lookup(Event::TYPE_ID).has_value());
    KTEST_EXPECT_TRUE(g_type_registry.lookup(Counter::TYPE_ID).has_value());
}

#endif
