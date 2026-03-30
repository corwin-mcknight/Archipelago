#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/type_registry.h>

using namespace kernel::testing;
using namespace kernel::obj;

KTEST(obj_type_registry_register_returns_id, "obj/type_registry") {
    TypeRegistry registry;
    auto result = registry.register_type(10, "test_type", RIGHTS_ALL, RIGHT_READ);
    KTEST_REQUIRE_TRUE(result.is_ok());
    KTEST_EXPECT_TRUE(result.unwrap() == 10);
}

KTEST(obj_type_registry_lookup_by_id, "obj/type_registry") {
    TypeRegistry registry;
    registry.register_type(10, "test_type", RIGHTS_ALL, RIGHT_READ);
    auto found = registry.lookup(10);
    KTEST_REQUIRE_TRUE(found.has_value());
    KTEST_EXPECT_TRUE(found.value()->id == 10);
    KTEST_EXPECT_TRUE(found.value()->valid_rights == RIGHTS_ALL);
    KTEST_EXPECT_TRUE(found.value()->default_rights == RIGHT_READ);
}

KTEST(obj_type_registry_lookup_by_name, "obj/type_registry") {
    TypeRegistry registry;
    registry.register_type(10, "test_type", RIGHTS_ALL, RIGHT_READ);
    auto found = registry.lookup_by_name("test_type");
    KTEST_REQUIRE_TRUE(found.has_value());
    KTEST_EXPECT_TRUE(found.value()->id == 10);
}

KTEST(obj_type_registry_duplicate_id_fails, "obj/type_registry") {
    TypeRegistry registry;
    registry.register_type(10, "type_a", RIGHTS_ALL, RIGHT_READ);
    auto result = registry.register_type(10, "type_b", RIGHTS_ALL, RIGHT_READ);
    KTEST_EXPECT_TRUE(result.is_err());
    KTEST_EXPECT_TRUE(result.unwrap_err() == RESULT_ALREADY_REGISTERED);
}

KTEST(obj_type_registry_duplicate_name_fails, "obj/type_registry") {
    TypeRegistry registry;
    registry.register_type(10, "same_name", RIGHTS_ALL, RIGHT_READ);
    auto result = registry.register_type(11, "same_name", RIGHTS_ALL, RIGHT_READ);
    KTEST_EXPECT_TRUE(result.is_err());
    KTEST_EXPECT_TRUE(result.unwrap_err() == RESULT_ALREADY_REGISTERED);
}

KTEST(obj_type_registry_multiple_types, "obj/type_registry") {
    TypeRegistry registry;
    KTEST_REQUIRE_TRUE(registry.register_type(1, "alpha", RIGHT_READ, RIGHT_READ).is_ok());
    KTEST_REQUIRE_TRUE(registry.register_type(2, "beta", RIGHT_WRITE, RIGHT_WRITE).is_ok());
    KTEST_REQUIRE_TRUE(registry.register_type(3, "gamma", RIGHT_SIGNAL, RIGHT_SIGNAL).is_ok());
    KTEST_EXPECT_TRUE(registry.count() == 3);
}

KTEST(obj_type_registry_lookup_missing_returns_nothing, "obj/type_registry") {
    TypeRegistry registry;
    auto found = registry.lookup(999);
    KTEST_EXPECT_TRUE(!found.has_value());
}

KTEST(obj_type_registry_lookup_name_missing_returns_nothing, "obj/type_registry") {
    TypeRegistry registry;
    auto found = registry.lookup_by_name("nonexistent");
    KTEST_EXPECT_TRUE(!found.has_value());
}

KTEST(obj_type_registry_live_count_zero_initially, "obj/type_registry") {
    TypeRegistry registry;
    registry.register_type(10, "test_type", RIGHTS_ALL, RIGHT_READ);
    KTEST_EXPECT_TRUE(registry.live_count(10) == 0);
}

KTEST(obj_type_registry_live_count_tracks, "obj/type_registry") {
    TypeRegistry registry;
    registry.register_type(10, "test_type", RIGHTS_ALL, RIGHT_READ);
    registry.on_object_created(10);
    KTEST_EXPECT_TRUE(registry.live_count(10) == 1);
    registry.on_object_created(10);
    KTEST_EXPECT_TRUE(registry.live_count(10) == 2);
    registry.on_object_destroyed(10);
    KTEST_EXPECT_TRUE(registry.live_count(10) == 1);
}

#endif