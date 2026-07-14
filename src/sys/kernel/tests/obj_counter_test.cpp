#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/test_objects.h>

using namespace kernel::testing;
using namespace kernel::obj;

KTEST_MODULE_WITH_INIT("obj/counter", obj_counter_init);

static void obj_counter_init() { register_all_test_types(); }

// One counter through its value operations: emplace honors the initial value, increment
// returns the previous value and adds the delta, reset zeroes.
KTEST_CASE(obj_counter_value_operations) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<Counter>(RIGHT_READ | RIGHT_WRITE, static_cast<uint64_t>(42)));
    KTEST_UNWRAP(ctr, table.get<Counter>(id));
    KTEST_EXPECT_TRUE(ctr->value() == 42);
    uint64_t prev = ctr->increment(5);
    KTEST_EXPECT_ALL(prev == 42, ctr->value() == 47);
    ctr->reset();
    KTEST_EXPECT_TRUE(ctr->value() == 0);
}

KTEST_CASE(obj_counter_rights_enforcement) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<Counter>(RIGHT_READ, static_cast<uint64_t>(0)));
    auto got = table.get<Counter>(id, RIGHT_WRITE);
    KTEST_EXPECT_ALL(got.is_err(), got.unwrap_err() == ktl::errc::rights_violation);
}

// Counter and Event are distinct registered types that coexist in one table: cross-type
// gets are rejected and each type's live count moves independently.
KTEST_CASE(obj_counter_and_event_coexist) {
    KTEST_EXPECT_TRUE(Counter::TYPE_ID != Event::TYPE_ID);
    KTEST_EXPECT_TRUE(g_type_registry.lookup(Counter::TYPE_ID).has_value());

    HandleTable table;
    uint32_t evt_before = g_type_registry.live_count(Event::TYPE_ID);
    uint32_t ctr_before = g_type_registry.live_count(Counter::TYPE_ID);
    KTEST_UNWRAP(eid, table.emplace<Event>(RIGHT_READ | RIGHT_SIGNAL | RIGHT_DUPLICATE));
    KTEST_UNWRAP(cid, table.emplace<Counter>(RIGHT_READ | RIGHT_WRITE, static_cast<uint64_t>(0)));
    KTEST_EXPECT_ALL(table.get<Event>(eid).is_ok(), table.get<Counter>(cid).is_ok(), table.get<Counter>(eid).is_err(),
                     table.get<Event>(cid).is_err(), table.count() == 2);
    KTEST_EXPECT_ALL(g_type_registry.live_count(Event::TYPE_ID) == evt_before + 1,
                     g_type_registry.live_count(Counter::TYPE_ID) == ctr_before + 1);
}

#endif
