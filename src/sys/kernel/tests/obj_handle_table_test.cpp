#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/test_objects.h>

using namespace kernel::testing;
using namespace kernel::obj;

static void handle_table_init() { register_all_test_types(); }

// Subsumes the old emplace_valid test -- emplace + get + verify type in one.
KTEST_WITH_INIT(obj_handle_table_emplace_and_get, "obj/handle_table", handle_table_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_EXPECT_ALL(id.is_valid(), table.count() == 1);
    KTEST_UNWRAP(got, table.get<TestObjA>(id));
    KTEST_EXPECT_TRUE(got->type_id() == TEST_TYPE_A);
}

KTEST_WITH_INIT(obj_handle_table_get_returns_object, "obj/handle_table", handle_table_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_UNWRAP(got, table.get<TestObjA>(id));
    KTEST_EXPECT_TRUE(got->type_id() == TEST_TYPE_A);
}

KTEST_WITH_INIT(obj_handle_table_close_invalidates, "obj/handle_table", handle_table_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_REQUIRE_TRUE(table.is_valid(id));
    table.close(id);
    KTEST_EXPECT_ALL(!table.is_valid(id), table.count() == 0);
}

// Subsumes old close_destroys_object + emplace_with_args -- both tested destroyed flag.
KTEST_WITH_INIT(obj_handle_table_close_destroys_object, "obj/handle_table", handle_table_init) {
    bool destroyed = false;
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL, &destroyed));
    KTEST_EXPECT_TRUE(table.get<TestObjA>(id).is_ok());
    KTEST_EXPECT_FALSE(destroyed);
    table.close(id);
    KTEST_EXPECT_TRUE(destroyed);
}

KTEST_WITH_INIT(obj_handle_table_generation_counter, "obj/handle_table", handle_table_init) {
    HandleTable table;
    KTEST_UNWRAP(id1, table.emplace<TestObjA>(RIGHTS_ALL));
    table.close(id1);
    KTEST_UNWRAP(id2, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_EXPECT_ALL(id1.index == id2.index, id1.generation != id2.generation, !table.is_valid(id1),
                     table.is_valid(id2));
}

KTEST_WITH_INIT(obj_handle_table_info_returns_metadata, "obj/handle_table", handle_table_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHT_READ | RIGHT_SIGNAL));
    KTEST_REQUIRE_VALUE(info, table.info(id));
    KTEST_EXPECT_ALL(info.rights == (RIGHT_READ | RIGHT_SIGNAL), info.type_id == TEST_TYPE_A);
}

KTEST_WITH_INIT(obj_handle_table_duplicate_ands_rights, "obj/handle_table", handle_table_init) {
    HandleTable table;
    KTEST_UNWRAP(src, table.emplace<TestObjA>(RIGHT_READ | RIGHT_WRITE));
    KTEST_UNWRAP(dup, table.duplicate(src, RIGHT_READ));
    KTEST_REQUIRE_VALUE(info, table.info(dup));
    KTEST_EXPECT_ALL(info.rights == RIGHT_READ, table.count() == 2);
}

KTEST_WITH_INIT(obj_handle_table_get_wrong_type, "obj/handle_table", handle_table_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL));
    auto got = table.get<TestObjB>(id);
    KTEST_EXPECT_ALL(got.is_err(), got.unwrap_err() == RESULT_WRONG_TYPE);
}

KTEST_WITH_INIT(obj_handle_table_get_insufficient_rights, "obj/handle_table", handle_table_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHT_READ));
    auto got = table.get<TestObjA>(id, RIGHT_WRITE);
    KTEST_EXPECT_ALL(got.is_err(), got.unwrap_err() == RESULT_RIGHTS_VIOLATION);
}

KTEST_WITH_INIT(obj_handle_table_get_sufficient_rights, "obj/handle_table", handle_table_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHT_READ | RIGHT_WRITE));
    KTEST_EXPECT_TRUE(table.get<TestObjA>(id, RIGHT_READ).is_ok());
}

KTEST_WITH_INIT(obj_handle_table_growth, "obj/handle_table", handle_table_init) {
    HandleTable table;
    HandleId first_id = HandleId::invalid();
    for (int i = 0; i < 64; i++) {
        KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL));
        if (i == 0) { first_id = id; }
    }
    KTEST_EXPECT_ALL(table.count() == 64, table.is_valid(first_id));
    KTEST_EXPECT_TRUE(table.get<TestObjA>(first_id).is_ok());
}

KTEST_WITH_INIT(obj_handle_table_invalid_handle, "obj/handle_table", handle_table_init) {
    HandleTable table;
    KTEST_EXPECT_FALSE(table.is_valid(HandleId::invalid()));
    auto got = table.get<TestObjA>(HandleId::invalid());
    KTEST_EXPECT_ALL(got.is_err(), got.unwrap_err() == RESULT_HANDLE_INVALID);
}

KTEST_WITH_INIT(obj_handle_table_global_emplace, "obj/handle_table", handle_table_init) {
    size_t before = g_handle_table.count();
    KTEST_UNWRAP(id, g_handle_table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_EXPECT_TRUE(g_handle_table.count() == before + 1);
    g_handle_table.close(id);
    KTEST_EXPECT_TRUE(g_handle_table.count() == before);
}

// F033: rights outside the type's registered valid_rights contract are rejected, not clamped.
KTEST_WITH_INIT(obj_handle_table_rejects_out_of_contract_rights, "obj/handle_table", handle_table_init) {
    HandleTable table;
    auto bad = table.emplace<TestObjRestricted>(RIGHT_WRITE);
    KTEST_EXPECT_ALL(bad.is_err(), bad.unwrap_err() == RESULT_RIGHTS_VIOLATION, table.count() == 0);
}

// F033: a mix of in-contract and out-of-contract bits is rejected outright (no silent clamping).
KTEST_WITH_INIT(obj_handle_table_rejects_mixed_rights, "obj/handle_table", handle_table_init) {
    HandleTable table;
    auto bad = table.emplace<TestObjRestricted>(RIGHT_READ | RIGHT_WRITE);
    KTEST_EXPECT_ALL(bad.is_err(), bad.unwrap_err() == RESULT_RIGHTS_VIOLATION, table.count() == 0);
}

// F033: rights within the contract still work, including duplicate (whose rights are a masked subset).
KTEST_WITH_INIT(obj_handle_table_accepts_in_contract_rights, "obj/handle_table", handle_table_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjRestricted>(TEST_RESTRICTED_VALID_RIGHTS));
    KTEST_REQUIRE_VALUE(info, table.info(id));
    KTEST_EXPECT_TRUE(info.rights == TEST_RESTRICTED_VALID_RIGHTS);
    KTEST_UNWRAP(dup, table.duplicate(id, RIGHT_READ));
    KTEST_REQUIRE_VALUE(dup_info, table.info(dup));
    KTEST_EXPECT_ALL(dup_info.rights == RIGHT_READ, table.count() == 2);
}

// F033: objects whose type was never registered cannot be given handles at all.
KTEST_WITH_INIT(obj_handle_table_rejects_unregistered_type, "obj/handle_table", handle_table_init) {
    HandleTable table;
    auto bad = table.emplace<TestObjUnregistered>(RIGHT_READ);
    KTEST_EXPECT_ALL(bad.is_err(), bad.unwrap_err() == RESULT_WRONG_TYPE, table.count() == 0);
}

// F021: a slot whose generation counter saturates is retired on close instead of being recycled,
// so a stale HandleId can never revalidate after the counter would have wrapped.
KTEST_WITH_INIT(obj_handle_table_generation_wrap_retires_slot, "obj/handle_table", handle_table_init) {
    HandleTable table;
    KTEST_UNWRAP(initial, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_REQUIRE_VALUE(id, table.testing_set_generation(initial, 0xFFFFFFFFu));
    KTEST_REQUIRE_TRUE(table.is_valid(id));
    KTEST_EXPECT_TRUE(table.close(id).is_ok());
    KTEST_EXPECT_ALL(!table.is_valid(id), table.count() == 0);

    // The retired slot must not be handed out again; the next emplace gets a different index.
    KTEST_UNWRAP(next, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_EXPECT_ALL(next.index != id.index, table.is_valid(next), !table.is_valid(id), table.count() == 1);

    // A handle forged with generation 0 against the retired slot must not validate either.
    KTEST_EXPECT_FALSE(table.is_valid(HandleId{id.index, 0}));
}

KTEST_WITH_INIT(obj_handle_table_destructor_closes_all, "obj/handle_table", handle_table_init) {
    bool d1 = false, d2 = false;
    {
        HandleTable table;
        table.emplace<TestObjA>(RIGHTS_ALL, &d1);
        table.emplace<TestObjA>(RIGHTS_ALL, &d2);
    }
    KTEST_EXPECT_ALL(d1, d2);
}

#endif
