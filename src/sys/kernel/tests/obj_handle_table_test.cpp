#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/sched/task.h>
#include <kernel/testing/test_objects.h>

using namespace kernel::testing;
using namespace kernel::obj;

KTEST_MODULE_WITH_INIT("obj/handle_table", handle_table_init);

static void handle_table_init() {
    register_all_test_types();
    kernel::sched::Task::register_type(g_type_registry).expect("task type registration failed");
}

KTEST_CASE(obj_handle_table_emplace_and_get) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_EXPECT_ALL(id.is_valid(), table.count() == 1);
    KTEST_UNWRAP(got, table.get<TestObjA>(id));
    KTEST_EXPECT_TRUE(got->type_id() == TEST_TYPE_A);
}

KTEST_CASE(obj_handle_table_close_invalidates) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_REQUIRE_TRUE(table.is_valid(id));
    KTEST_EXPECT_TRUE(table.close(id).is_ok());
    KTEST_EXPECT_ALL(!table.is_valid(id), table.count() == 0);
}

KTEST_CASE(obj_handle_table_close_destroys_object) {
    bool destroyed = false;
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL, &destroyed));
    KTEST_EXPECT_TRUE(table.get<TestObjA>(id).is_ok());
    KTEST_EXPECT_FALSE(destroyed);
    KTEST_EXPECT_TRUE(table.close(id).is_ok());
    KTEST_EXPECT_TRUE(destroyed);
}

// take() is close() that hands the entry's reference over instead of dropping it: the handle dies,
// the object survives exactly as long as the returned reference, and the rights ride along
// unchanged -- the move half of handle transfer.
KTEST_CASE(obj_handle_table_take_moves_reference) {
    bool destroyed = false;
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHT_READ | RIGHT_SIGNAL, &destroyed));

    KTEST_UNWRAP(taken, table.take(id));
    KTEST_EXPECT_ALL(!table.is_valid(id), table.count() == 0, !destroyed);
    KTEST_EXPECT_ALL(taken.rights == (RIGHT_READ | RIGHT_SIGNAL), taken.object->type_id() == TEST_TYPE_A);

    // The slot is recycled like a closed one, and re-taking the dead handle is refused.
    auto again = table.take(id);
    KTEST_EXPECT_ALL(again.is_err(), again.unwrap_err() == ktl::errc::handle_invalid);

    taken.object.reset();
    KTEST_EXPECT_TRUE(destroyed);
}

KTEST_CASE(obj_handle_table_generation_counter) {
    HandleTable table;
    KTEST_UNWRAP(id1, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_EXPECT_TRUE(table.close(id1).is_ok());
    KTEST_UNWRAP(id2, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_EXPECT_ALL(id1.index == id2.index, id1.generation != id2.generation, !table.is_valid(id1),
                     table.is_valid(id2));
}

KTEST_CASE(obj_handle_table_info_returns_metadata) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHT_READ | RIGHT_SIGNAL));
    KTEST_REQUIRE_VALUE(info, table.info(id));
    KTEST_EXPECT_ALL(info.rights == (RIGHT_READ | RIGHT_SIGNAL), info.type_id == TEST_TYPE_A);
}

KTEST_CASE(obj_handle_table_duplicate_ands_rights) {
    HandleTable table;
    KTEST_UNWRAP(src, table.emplace<TestObjA>(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE));
    KTEST_UNWRAP(dup, table.duplicate(src, RIGHT_READ));
    KTEST_REQUIRE_VALUE(info, table.info(dup));
    KTEST_EXPECT_ALL(info.rights == RIGHT_READ, table.count() == 2);
}

// Duplication is itself a capability enforced by the table, not just by syscall dispatch: a
// source handle without RIGHT_DUPLICATE is refused no matter which kernel path asks.
KTEST_CASE(obj_handle_table_duplicate_requires_right) {
    HandleTable table;
    KTEST_UNWRAP(src, table.emplace<TestObjA>(RIGHT_READ | RIGHT_WRITE));
    auto dup = table.duplicate(src, RIGHT_READ);
    KTEST_EXPECT_ALL(dup.is_err(), dup.unwrap_err() == ktl::errc::rights_violation, table.count() == 1);
}

// One rights-enforcement story for get(): a cross-type get is rejected with wrong_type,
// requesting a right the handle lacks fails with rights_violation, and requesting a
// subset of the held rights succeeds.
KTEST_CASE(obj_handle_table_get_enforces_type_and_rights) {
    HandleTable table;
    KTEST_UNWRAP(ro, table.emplace<TestObjA>(RIGHT_READ));
    KTEST_UNWRAP(rw, table.emplace<TestObjA>(RIGHT_READ | RIGHT_WRITE));
    auto wrong = table.get<TestObjB>(ro);
    KTEST_EXPECT_ALL(wrong.is_err(), wrong.unwrap_err() == ktl::errc::wrong_type);
    auto denied = table.get<TestObjA>(ro, RIGHT_WRITE);
    KTEST_EXPECT_ALL(denied.is_err(), denied.unwrap_err() == ktl::errc::rights_violation);
    KTEST_EXPECT_TRUE(table.get<TestObjA>(rw, RIGHT_READ).is_ok());
}

KTEST_CASE(obj_handle_table_growth) {
    HandleTable table;
    HandleId first_id = HandleId::invalid();
    for (int i = 0; i < 64; i++) {
        KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL));
        if (i == 0) { first_id = id; }
    }
    KTEST_EXPECT_ALL(table.count() == 64, table.is_valid(first_id));
    KTEST_EXPECT_TRUE(table.get<TestObjA>(first_id).is_ok());
}

KTEST_CASE(obj_handle_table_invalid_handle) {
    HandleTable table;
    KTEST_EXPECT_FALSE(table.is_valid(HandleId::invalid()));
    auto got = table.get<TestObjA>(HandleId::invalid());
    KTEST_EXPECT_ALL(got.is_err(), got.unwrap_err() == ktl::errc::handle_invalid);
}

KTEST_CASE(obj_handle_table_global_emplace) {
    auto& handles = kernel::sched::kernel_task()->handles();
    size_t before = handles.count();
    KTEST_UNWRAP(id, handles.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_EXPECT_TRUE(handles.count() == before + 1);
    KTEST_EXPECT_TRUE(handles.close(id).is_ok());
    KTEST_EXPECT_TRUE(handles.count() == before);
}

// Rights outside the type's registered valid_rights contract are rejected, not clamped --
// whether entirely out-of-contract or a mix of in-contract and out-of-contract bits -- while
// rights within the contract still work, including duplicate (whose rights are a masked subset).
KTEST_CASE(obj_handle_table_enforces_rights_contract) {
    HandleTable table;
    auto bad = table.emplace<TestObjRestricted>(RIGHT_WRITE);
    KTEST_EXPECT_ALL(bad.is_err(), bad.unwrap_err() == ktl::errc::rights_violation, table.count() == 0);
    auto mixed = table.emplace<TestObjRestricted>(RIGHT_READ | RIGHT_WRITE);
    KTEST_EXPECT_ALL(mixed.is_err(), mixed.unwrap_err() == ktl::errc::rights_violation, table.count() == 0);
    KTEST_UNWRAP(id, table.emplace<TestObjRestricted>(TEST_RESTRICTED_VALID_RIGHTS));
    KTEST_REQUIRE_VALUE(info, table.info(id));
    KTEST_EXPECT_TRUE(info.rights == TEST_RESTRICTED_VALID_RIGHTS);
    KTEST_UNWRAP(dup, table.duplicate(id, RIGHT_READ));
    KTEST_REQUIRE_VALUE(dup_info, table.info(dup));
    KTEST_EXPECT_ALL(dup_info.rights == RIGHT_READ, table.count() == 2);
}

// Objects whose type was never registered cannot be given handles at all.
KTEST_CASE(obj_handle_table_rejects_unregistered_type) {
    HandleTable table;
    auto bad = table.emplace<TestObjUnregistered>(RIGHT_READ);
    KTEST_EXPECT_ALL(bad.is_err(), bad.unwrap_err() == ktl::errc::wrong_type, table.count() == 0);
}

// A slot whose generation counter saturates is retired on close instead of being recycled,
// so a stale HandleId can never revalidate after the counter would have wrapped.
KTEST_CASE(obj_handle_table_generation_wrap_retires_slot) {
    HandleTable table;
    KTEST_UNWRAP(initial, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_REQUIRE_VALUE(id, table.testing_set_generation(initial, 0x7FFFFFFFu));
    KTEST_REQUIRE_TRUE(table.is_valid(id));
    KTEST_EXPECT_TRUE(table.close(id).is_ok());
    KTEST_EXPECT_ALL(!table.is_valid(id), table.count() == 0);

    // The retired slot must not be handed out again; the next emplace gets a different index.
    KTEST_UNWRAP(next, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_EXPECT_ALL(next.index != id.index, table.is_valid(next), !table.is_valid(id), table.count() == 1);

    // A handle forged with generation 0 against the retired slot must not validate either.
    KTEST_EXPECT_FALSE(table.is_valid(HandleId{id.index, 0}));
}

KTEST_CASE(obj_handle_table_destructor_closes_all) {
    bool d1 = false, d2 = false;
    {
        HandleTable table;
        KTEST_EXPECT_TRUE(table.emplace<TestObjA>(RIGHTS_ALL, &d1).is_ok());
        KTEST_EXPECT_TRUE(table.emplace<TestObjA>(RIGHTS_ALL, &d2).is_ok());
    }
    KTEST_EXPECT_ALL(d1, d2);
}

namespace {

// Closes another handle in the same table from its destructor, the way a dying Task closes
// handles it holds in a parent's table. Piggybacks on TestObjA's registered type: the
// registry only ever sees the stored type id.
class CloseOnDeath : public Object {
   public:
    DECLARE_OBJECT_TYPE(CloseOnDeath, TEST_TYPE_A)
    CloseOnDeath(HandleTable* table, const HandleId* victim) : Object(TYPE_ID), m_table(table), m_victim(victim) {}
    ~CloseOnDeath() override { (void)m_table->close(*m_victim); }

   private:
    HandleTable* m_table;
    const HandleId* m_victim;
};

}  // namespace

// The destructor runs against the table's non-recursive mutex, so the last reference must be
// dropped only after close() releases the lock.
KTEST_CASE(obj_handle_table_close_reentrant_destructor) {
    HandleTable table;
    HandleId victim = HandleId::invalid();
    KTEST_UNWRAP(closer, table.emplace<CloseOnDeath>(RIGHTS_ALL, &table, &victim));
    KTEST_UNWRAP(v, table.emplace<TestObjA>(RIGHTS_ALL));
    victim = v;
    KTEST_EXPECT_TRUE(table.close(closer).is_ok());
    KTEST_EXPECT_ALL(!table.is_valid(victim), table.count() == 0);
}

KTEST_CASE(obj_handle_table_clear_reentrant_destructor) {
    HandleTable table;
    HandleId victim = HandleId::invalid();
    KTEST_REQUIRE_TRUE(table.emplace<CloseOnDeath>(RIGHTS_ALL, &table, &victim).is_ok());
    KTEST_UNWRAP(v, table.emplace<TestObjA>(RIGHTS_ALL));
    victim = v;
    table.clear();
    KTEST_EXPECT_ALL(table.count() == 0, !table.is_valid(victim));
}

#endif
