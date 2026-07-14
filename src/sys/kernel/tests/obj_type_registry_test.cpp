#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/type_registry.h>

using namespace kernel::testing;
using namespace kernel::obj;

KTEST_MODULE("obj/type_registry");

// Registration succeeds and the entry is retrievable by id and by name with its rights intact.
KTEST_CASE(obj_type_registry_register_and_lookup) {
    TypeRegistry registry;
    KTEST_REQUIRE_TRUE(registry.register_type(10, "test_type", RIGHTS_ALL, RIGHT_READ).is_ok());
    auto by_id = registry.lookup(10);
    KTEST_REQUIRE_TRUE(by_id.has_value());
    KTEST_EXPECT_ALL(by_id.value().id == 10, by_id.value().valid_rights == RIGHTS_ALL,
                     by_id.value().default_rights == RIGHT_READ);
    auto by_name = registry.lookup_by_name("test_type");
    KTEST_REQUIRE_TRUE(by_name.has_value());
    KTEST_EXPECT_TRUE(by_name.value().id == 10);
}

// Re-registering an existing id or an existing name both fail with already_registered.
KTEST_CASE(obj_type_registry_duplicate_registration_fails) {
    TypeRegistry registry;
    KTEST_REQUIRE_TRUE(registry.register_type(10, "type_a", RIGHTS_ALL, RIGHT_READ).is_ok());
    auto same_id = registry.register_type(10, "type_b", RIGHTS_ALL, RIGHT_READ);
    KTEST_EXPECT_ALL(same_id.is_err(), same_id.unwrap_err() == ktl::errc::already_registered);
    auto same_name = registry.register_type(11, "type_a", RIGHTS_ALL, RIGHT_READ);
    KTEST_EXPECT_ALL(same_name.is_err(), same_name.unwrap_err() == ktl::errc::already_registered);
}

// Multiple distinct types coexist; lookups for ids and names never registered return nothing.
KTEST_CASE(obj_type_registry_multiple_types_and_misses) {
    TypeRegistry registry;
    KTEST_REQUIRE_TRUE(registry.register_type(1, "alpha", RIGHT_READ, RIGHT_READ).is_ok());
    KTEST_REQUIRE_TRUE(registry.register_type(2, "beta", RIGHT_WRITE, RIGHT_WRITE).is_ok());
    KTEST_REQUIRE_TRUE(registry.register_type(3, "gamma", RIGHT_SIGNAL, RIGHT_SIGNAL).is_ok());
    KTEST_EXPECT_TRUE(registry.count() == 3);
    KTEST_EXPECT_ALL(!registry.lookup(999).has_value(), !registry.lookup_by_name("nonexistent").has_value());
}

// Live count starts at zero and tracks created/destroyed notifications.
KTEST_CASE(obj_type_registry_live_count_tracks) {
    TypeRegistry registry;
    KTEST_REQUIRE_TRUE(registry.register_type(10, "test_type", RIGHTS_ALL, RIGHT_READ).is_ok());
    KTEST_EXPECT_TRUE(registry.live_count(10) == 0);
    registry.on_object_created(10);
    KTEST_EXPECT_TRUE(registry.live_count(10) == 1);
    registry.on_object_created(10);
    KTEST_EXPECT_TRUE(registry.live_count(10) == 2);
    registry.on_object_destroyed(10);
    KTEST_EXPECT_TRUE(registry.live_count(10) == 1);
}

#endif
