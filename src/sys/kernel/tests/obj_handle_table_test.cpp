#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/handle_table.h>
#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>

#include <ktl/ref>

using namespace kernel::testing;
using namespace kernel::obj;

namespace {

constexpr TypeId TEST_TYPE_A = 50;
constexpr TypeId TEST_TYPE_B = 51;

class ObjA : public Object {
   public:
    DECLARE_OBJECT_TYPE(ObjA, TEST_TYPE_A)
    explicit ObjA(bool* destroyed_flag = nullptr) : Object(TYPE_ID), m_destroyed(destroyed_flag) {}
    ~ObjA() override {
        if (m_destroyed) { *m_destroyed = true; }
    }

   private:
    bool* m_destroyed;
};

class ObjB : public Object {
   public:
    DECLARE_OBJECT_TYPE(ObjB, TEST_TYPE_B)
    ObjB() : Object(TYPE_ID) {}
};

}  // namespace

static void handle_table_init() {
    static bool registered = false;
    if (!registered) {
        g_type_registry.register_type(TEST_TYPE_A, "obj_a", RIGHTS_ALL, RIGHTS_ALL);
        g_type_registry.register_type(TEST_TYPE_B, "obj_b", RIGHTS_ALL, RIGHTS_ALL);
        registered = true;
    }
}

KTEST_WITH_INIT(obj_handle_table_emplace_valid, "obj/handle_table", handle_table_init) {
    HandleTable table;
    auto result = table.emplace<ObjA>(RIGHTS_ALL);
    KTEST_REQUIRE_TRUE(result.is_ok());
    KTEST_EXPECT_TRUE(result.unwrap().is_valid());
    KTEST_EXPECT_TRUE(table.count() == 1);
}

KTEST_WITH_INIT(obj_handle_table_get_returns_object, "obj/handle_table", handle_table_init) {
    HandleTable table;
    auto id  = table.emplace<ObjA>(RIGHTS_ALL).unwrap();
    auto got = table.get<ObjA>(id);
    KTEST_REQUIRE_TRUE(got.is_ok());
    KTEST_EXPECT_TRUE(got.unwrap()->type_id() == TEST_TYPE_A);
}

KTEST_WITH_INIT(obj_handle_table_close_invalidates, "obj/handle_table", handle_table_init) {
    HandleTable table;
    auto id = table.emplace<ObjA>(RIGHTS_ALL).unwrap();
    KTEST_REQUIRE_TRUE(table.is_valid(id));
    table.close(id);
    KTEST_EXPECT_TRUE(!table.is_valid(id));
    KTEST_EXPECT_TRUE(table.count() == 0);
}

KTEST_WITH_INIT(obj_handle_table_close_destroys_object, "obj/handle_table", handle_table_init) {
    bool destroyed = false;
    HandleTable table;
    auto id = table.emplace<ObjA>(RIGHTS_ALL, &destroyed).unwrap();
    KTEST_EXPECT_TRUE(!destroyed);
    table.close(id);
    KTEST_EXPECT_TRUE(destroyed);
}

KTEST_WITH_INIT(obj_handle_table_generation_counter, "obj/handle_table", handle_table_init) {
    HandleTable table;
    auto id1 = table.emplace<ObjA>(RIGHTS_ALL).unwrap();
    table.close(id1);
    auto id2 = table.emplace<ObjA>(RIGHTS_ALL).unwrap();
    // Same slot reused but different generation
    KTEST_EXPECT_TRUE(id1.index == id2.index);
    KTEST_EXPECT_TRUE(id1.generation != id2.generation);
    // Old handle no longer valid
    KTEST_EXPECT_TRUE(!table.is_valid(id1));
    KTEST_EXPECT_TRUE(table.is_valid(id2));
}

KTEST_WITH_INIT(obj_handle_table_info_returns_metadata, "obj/handle_table", handle_table_init) {
    HandleTable table;
    auto id   = table.emplace<ObjA>(RIGHT_READ | RIGHT_SIGNAL).unwrap();
    auto info = table.info(id);
    KTEST_REQUIRE_TRUE(info.has_value());
    KTEST_EXPECT_TRUE(info.value().rights == (RIGHT_READ | RIGHT_SIGNAL));
    KTEST_EXPECT_TRUE(info.value().type_id == TEST_TYPE_A);
}

KTEST_WITH_INIT(obj_handle_table_duplicate_ands_rights, "obj/handle_table", handle_table_init) {
    HandleTable table;
    auto src     = table.emplace<ObjA>(RIGHT_READ | RIGHT_WRITE).unwrap();
    auto dup_res = table.duplicate(src, RIGHT_READ);
    KTEST_REQUIRE_TRUE(dup_res.is_ok());
    auto dup_info = table.info(dup_res.unwrap());
    KTEST_REQUIRE_TRUE(dup_info.has_value());
    KTEST_EXPECT_TRUE(dup_info.value().rights == RIGHT_READ);
    KTEST_EXPECT_TRUE(table.count() == 2);
}

KTEST_WITH_INIT(obj_handle_table_get_wrong_type, "obj/handle_table", handle_table_init) {
    HandleTable table;
    auto id  = table.emplace<ObjA>(RIGHTS_ALL).unwrap();
    auto got = table.get<ObjB>(id);
    KTEST_EXPECT_TRUE(got.is_err());
    KTEST_EXPECT_TRUE(got.unwrap_err() == RESULT_WRONG_TYPE);
}

KTEST_WITH_INIT(obj_handle_table_get_insufficient_rights, "obj/handle_table", handle_table_init) {
    HandleTable table;
    auto id  = table.emplace<ObjA>(RIGHT_READ).unwrap();
    auto got = table.get<ObjA>(id, RIGHT_WRITE);
    KTEST_EXPECT_TRUE(got.is_err());
    KTEST_EXPECT_TRUE(got.unwrap_err() == RESULT_RIGHTS_VIOLATION);
}

KTEST_WITH_INIT(obj_handle_table_get_sufficient_rights, "obj/handle_table", handle_table_init) {
    HandleTable table;
    auto id  = table.emplace<ObjA>(RIGHT_READ | RIGHT_WRITE).unwrap();
    auto got = table.get<ObjA>(id, RIGHT_READ);
    KTEST_EXPECT_TRUE(got.is_ok());
}

KTEST_WITH_INIT(obj_handle_table_growth, "obj/handle_table", handle_table_init) {
    HandleTable table;
    HandleId first_id = HandleId::invalid();
    // Create more handles than GROW_BATCH (32)
    for (int i = 0; i < 64; i++) {
        auto result = table.emplace<ObjA>(RIGHTS_ALL);
        KTEST_REQUIRE_TRUE(result.is_ok());
        if (i == 0) { first_id = result.unwrap(); }
    }
    KTEST_EXPECT_TRUE(table.count() == 64);
    // First handle still valid after growth
    KTEST_EXPECT_TRUE(table.is_valid(first_id));
    auto got = table.get<ObjA>(first_id);
    KTEST_EXPECT_TRUE(got.is_ok());
}

KTEST_WITH_INIT(obj_handle_table_invalid_handle, "obj/handle_table", handle_table_init) {
    HandleTable table;
    KTEST_EXPECT_TRUE(!table.is_valid(HandleId::invalid()));
    auto got = table.get<ObjA>(HandleId::invalid());
    KTEST_EXPECT_TRUE(got.is_err());
    KTEST_EXPECT_TRUE(got.unwrap_err() == RESULT_HANDLE_INVALID);
}

KTEST_WITH_INIT(obj_handle_table_global_emplace, "obj/handle_table", handle_table_init) {
    size_t before = g_handle_table.count();
    auto result   = g_handle_table.emplace<ObjA>(RIGHTS_ALL);
    KTEST_REQUIRE_TRUE(result.is_ok());
    KTEST_EXPECT_TRUE(g_handle_table.count() == before + 1);
    g_handle_table.close(result.unwrap());
    KTEST_EXPECT_TRUE(g_handle_table.count() == before);
}

KTEST_WITH_INIT(obj_handle_table_emplace_and_get, "obj/handle_table", handle_table_init) {
    HandleTable table;
    auto result = table.emplace<ObjA>(RIGHTS_ALL);
    KTEST_REQUIRE_TRUE(result.is_ok());
    auto id = result.unwrap();
    KTEST_EXPECT_TRUE(id.is_valid());
    auto got = table.get<ObjA>(id);
    KTEST_REQUIRE_TRUE(got.is_ok());
    KTEST_EXPECT_TRUE(got.unwrap()->type_id() == TEST_TYPE_A);
    KTEST_EXPECT_TRUE(table.count() == 1);
}

KTEST_WITH_INIT(obj_handle_table_emplace_with_args, "obj/handle_table", handle_table_init) {
    bool destroyed = false;
    HandleTable table;
    auto result = table.emplace<ObjA>(RIGHTS_ALL, &destroyed);
    KTEST_REQUIRE_TRUE(result.is_ok());
    auto id = result.unwrap();
    KTEST_EXPECT_TRUE(table.get<ObjA>(id).is_ok());
    table.close(id);
    KTEST_EXPECT_TRUE(destroyed);
}

KTEST_WITH_INIT(obj_handle_table_destructor_closes_all, "obj/handle_table", handle_table_init) {
    bool d1 = false;
    bool d2 = false;
    {
        HandleTable table;
        table.emplace<ObjA>(RIGHTS_ALL, &d1);
        table.emplace<ObjA>(RIGHTS_ALL, &d2);
    }
    KTEST_EXPECT_TRUE(d1);
    KTEST_EXPECT_TRUE(d2);
}

#endif
