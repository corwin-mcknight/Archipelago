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
constexpr kernel::obj::TypeId TEST_TYPE_A                  = 50;
constexpr kernel::obj::TypeId TEST_TYPE_B                  = 51;
constexpr kernel::obj::TypeId TEST_TYPE_RESTRICTED         = 52;
constexpr kernel::obj::TypeId TEST_TYPE_UNREGISTERED       = 53;

// Rights contract for TestObjRestricted (see register_all_test_types).
constexpr kernel::obj::Rights TEST_RESTRICTED_VALID_RIGHTS = kernel::obj::RIGHT_READ | kernel::obj::RIGHT_DUPLICATE;

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

// Type registered with a restricted valid_rights contract, for rights-validation checks.
class TestObjRestricted : public kernel::obj::Object {
   public:
    DECLARE_OBJECT_TYPE(TestObjRestricted, TEST_TYPE_RESTRICTED)
    TestObjRestricted() : Object(TYPE_ID) {}
};

// Type deliberately never registered with the type registry.
class TestObjUnregistered : public kernel::obj::Object {
   public:
    DECLARE_OBJECT_TYPE(TestObjUnregistered, TEST_TYPE_UNREGISTERED)
    TestObjUnregistered() : Object(TYPE_ID) {}
};

// already_registered is expected here: obj_init() registers Event/Counter at boot, and tests
// re-enter this function freely. Any other registration failure is a real test-environment bug.
inline void expect_registered(ktl::result<void> result, const char* msg) {
    if (result.is_err() && result.unwrap_err() == ktl::errc::already_registered) { return; }
    result.expect(msg);
}

// One-shot init that registers TestObjA/TestObjB plus Event/Counter.
inline void register_all_test_types() {
    static bool done = false;
    if (done) return;
    using namespace kernel::obj;
    expect_registered(g_type_registry.register_type(TEST_TYPE_A, "test_obj_a", RIGHTS_ALL, RIGHTS_ALL),
                      "test type A registration failed");
    expect_registered(g_type_registry.register_type(TEST_TYPE_B, "test_obj_b", RIGHTS_ALL, RIGHTS_ALL),
                      "test type B registration failed");
    expect_registered(g_type_registry.register_type(TEST_TYPE_RESTRICTED, "test_obj_restricted",
                                                    TEST_RESTRICTED_VALID_RIGHTS, RIGHT_READ),
                      "restricted test type registration failed");
    expect_registered(Event::register_type(g_type_registry), "Event test registration failed");
    expect_registered(Counter::register_type(g_type_registry), "Counter test registration failed");
    done = true;
}

}  // namespace kernel::testing

#endif  // CONFIG_KERNEL_TESTING
