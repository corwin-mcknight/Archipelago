#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/test_objects.h>

using namespace kernel::testing;
using namespace kernel::obj;

static void obj_counter_init() { register_all_test_types(); }

KTEST_WITH_INIT(obj_counter_register_type, "obj/counter", obj_counter_init) {
    KTEST_EXPECT_TRUE(Counter::TYPE_ID != Event::TYPE_ID);
    KTEST_EXPECT_TRUE(g_type_registry.lookup(Counter::TYPE_ID).has_value());
}

KTEST_WITH_INIT(obj_counter_emplace_with_initial, "obj/counter", obj_counter_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<Counter>(RIGHTS_ALL, static_cast<uint64_t>(42)));
    KTEST_UNWRAP(ctr, table.get<Counter>(id));
    KTEST_EXPECT_TRUE(ctr->value() == 42);
}

KTEST_WITH_INIT(obj_counter_increment, "obj/counter", obj_counter_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<Counter>(RIGHTS_ALL, static_cast<uint64_t>(10)));
    KTEST_UNWRAP(ctr, table.get<Counter>(id));
    uint64_t prev = ctr->increment(5);
    KTEST_EXPECT_ALL(prev == 10, ctr->value() == 15);
}

KTEST_WITH_INIT(obj_counter_reset, "obj/counter", obj_counter_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<Counter>(RIGHTS_ALL, static_cast<uint64_t>(99)));
    KTEST_UNWRAP(ctr, table.get<Counter>(id));
    ctr->reset();
    KTEST_EXPECT_TRUE(ctr->value() == 0);
}

KTEST_WITH_INIT(obj_counter_rights_enforcement, "obj/counter", obj_counter_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<Counter>(RIGHT_READ, static_cast<uint64_t>(0)));
    auto got = table.get<Counter>(id, RIGHT_WRITE);
    KTEST_EXPECT_ALL(got.is_err(), got.unwrap_err() == RESULT_RIGHTS_VIOLATION);
}

KTEST_WITH_INIT(obj_counter_and_event_coexist, "obj/counter", obj_counter_init) {
    HandleTable table;
    KTEST_UNWRAP(eid, table.emplace<Event>(RIGHTS_ALL));
    KTEST_UNWRAP(cid, table.emplace<Counter>(RIGHTS_ALL, static_cast<uint64_t>(0)));
    KTEST_EXPECT_ALL(table.get<Event>(eid).is_ok(), table.get<Counter>(cid).is_ok(), table.get<Counter>(eid).is_err(),
                     table.get<Event>(cid).is_err(), table.count() == 2);
}

KTEST_WITH_INIT(obj_counter_live_count_independent, "obj/counter", obj_counter_init) {
    HandleTable table;
    uint32_t evt_before = g_type_registry.live_count(Event::TYPE_ID);
    uint32_t ctr_before = g_type_registry.live_count(Counter::TYPE_ID);
    table.emplace<Event>(RIGHTS_ALL);
    table.emplace<Counter>(RIGHTS_ALL, static_cast<uint64_t>(0));
    KTEST_EXPECT_ALL(g_type_registry.live_count(Event::TYPE_ID) == evt_before + 1,
                     g_type_registry.live_count(Counter::TYPE_ID) == ctr_before + 1);
}

#endif
