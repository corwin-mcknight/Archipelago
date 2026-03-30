#pragma once

#include <kernel/config.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/counter.h>
#include <kernel/obj/event.h>
#include <kernel/obj/handle_table.h>
#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>

#include <ktl/ref>

namespace kernel::testing {

// Shared type IDs for test-only Object subclasses.
constexpr kernel::obj::TypeId TEST_TYPE_A = 50;
constexpr kernel::obj::TypeId TEST_TYPE_B = 51;

// Simple Object subclass with an optional destruction callback.
class TestObjA : public kernel::obj::Object {
   public:
    DECLARE_OBJECT_TYPE(TestObjA, TEST_TYPE_A)
    explicit TestObjA(bool* destroyed_flag = nullptr) : Object(TYPE_ID), m_destroyed(destroyed_flag) {}
    ~TestObjA() override {
        if (m_destroyed) { *m_destroyed = true; }
    }

   private:
    bool* m_destroyed;
};

// Second type used for wrong-type checks.
class TestObjB : public kernel::obj::Object {
   public:
    DECLARE_OBJECT_TYPE(TestObjB, TEST_TYPE_B)
    TestObjB() : Object(TYPE_ID) {}
};

// One-shot init that registers TestObjA/TestObjB plus Event/Counter.
inline void register_all_test_types() {
    static bool done = false;
    if (done) return;
    using namespace kernel::obj;
    g_type_registry.register_type(TEST_TYPE_A, "test_obj_a", RIGHTS_ALL, RIGHTS_ALL);
    g_type_registry.register_type(TEST_TYPE_B, "test_obj_b", RIGHTS_ALL, RIGHTS_ALL);
    Event::register_type(g_type_registry);
    Counter::register_type(g_type_registry);
    done = true;
}

}  // namespace kernel::testing

#endif  // CONFIG_KERNEL_TESTING
