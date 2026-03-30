#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>

#include <ktl/ref>

using namespace kernel::testing;
using namespace kernel::obj;

namespace {

constexpr TypeId TEST_TYPE_ID = 99;

class TestObject : public Object {
   public:
    DECLARE_OBJECT_TYPE(TestObject, TEST_TYPE_ID)

    explicit TestObject(bool* destroyed_flag = nullptr) : Object(TYPE_ID), m_destroyed(destroyed_flag) {}
    ~TestObject() override {
        if (m_destroyed) { *m_destroyed = true; }
    }

   private:
    bool* m_destroyed;
};

}  // namespace

static void obj_object_init() {
    static bool registered = false;
    if (!registered) {
        g_type_registry.register_type(TEST_TYPE_ID, "test_object", RIGHTS_ALL, RIGHTS_ALL);
        registered = true;
    }
}

KTEST_WITH_INIT(obj_object_unique_ids, "obj/object", obj_object_init) {
    auto a = ktl::make_ref<TestObject>();
    auto b = ktl::make_ref<TestObject>();
    KTEST_EXPECT_TRUE(a->id() != b->id());
}

KTEST_WITH_INIT(obj_object_monotonic_ids, "obj/object", obj_object_init) {
    auto a = ktl::make_ref<TestObject>();
    auto b = ktl::make_ref<TestObject>();
    KTEST_EXPECT_TRUE(b->id() > a->id());
}

KTEST_WITH_INIT(obj_object_type_id, "obj/object", obj_object_init) {
    auto obj = ktl::make_ref<TestObject>();
    KTEST_EXPECT_TRUE(obj->type_id() == TEST_TYPE_ID);
}

KTEST_WITH_INIT(obj_object_signals_default_zero, "obj/object", obj_object_init) {
    auto obj = ktl::make_ref<TestObject>();
    KTEST_EXPECT_TRUE(obj->signals() == 0);
}

KTEST_WITH_INIT(obj_object_signal_set, "obj/object", obj_object_init) {
    auto obj = ktl::make_ref<TestObject>();
    obj->signal_set(0x05);
    KTEST_EXPECT_TRUE(obj->signals() == 0x05);
}

KTEST_WITH_INIT(obj_object_signal_clear, "obj/object", obj_object_init) {
    auto obj = ktl::make_ref<TestObject>();
    obj->signal_set(0xFF);
    obj->signal_clear(0x0F);
    KTEST_EXPECT_TRUE(obj->signals() == 0xF0);
}

KTEST_WITH_INIT(obj_object_signals_independent, "obj/object", obj_object_init) {
    auto obj = ktl::make_ref<TestObject>();
    obj->signal_set(0x01);
    obj->signal_set(0x10);
    KTEST_EXPECT_TRUE(obj->signals() == 0x11);
    obj->signal_clear(0x01);
    KTEST_EXPECT_TRUE(obj->signals() == 0x10);
}

KTEST_WITH_INIT(obj_object_destructor_runs, "obj/object", obj_object_init) {
    bool destroyed = false;
    {
        auto obj = ktl::make_ref<TestObject>(&destroyed);
        KTEST_EXPECT_TRUE(!destroyed);
    }
    KTEST_EXPECT_TRUE(destroyed);
}

KTEST_WITH_INIT(obj_object_ref_last_drop_destroys, "obj/object", obj_object_init) {
    bool destroyed = false;
    auto r1        = ktl::make_ref<TestObject>(&destroyed);
    {
        auto r2 = r1;
        KTEST_EXPECT_TRUE(r1.ref_count() == 2);
    }
    KTEST_EXPECT_TRUE(!destroyed);
    r1.reset();
    KTEST_EXPECT_TRUE(destroyed);
}

KTEST_WITH_INIT(obj_object_live_count, "obj/object", obj_object_init) {
    uint32_t before = g_type_registry.live_count(TEST_TYPE_ID);
    auto obj        = ktl::make_ref<TestObject>();
    KTEST_EXPECT_TRUE(g_type_registry.live_count(TEST_TYPE_ID) == before + 1);
    obj.reset();
    KTEST_EXPECT_TRUE(g_type_registry.live_count(TEST_TYPE_ID) == before);
}

KTEST_WITH_INIT(obj_object_name_default_null, "obj/object", obj_object_init) {
    auto obj = ktl::make_ref<TestObject>();
    KTEST_EXPECT_TRUE(obj->name() == nullptr);
}

KTEST_WITH_INIT(obj_object_set_name, "obj/object", obj_object_init) {
    auto obj = ktl::make_ref<TestObject>();
    obj->set_name("my_test_object");
    KTEST_EXPECT_TRUE(obj->name() != nullptr);
}

#endif
