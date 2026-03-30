#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/counter.h>
#include <kernel/obj/event.h>
#include <kernel/obj/handle_table.h>
#include <kernel/obj/type_registry.h>

using namespace kernel::testing;
using namespace kernel::obj;

static void obj_counter_init() {
    static bool registered = false;
    if (!registered) {
        Event::register_type(g_type_registry);
        Counter::register_type(g_type_registry);
        registered = true;
    }
}

KTEST_WITH_INIT(obj_counter_register_type, "obj/counter", obj_counter_init) {
    KTEST_EXPECT_TRUE(Counter::TYPE_ID != Event::TYPE_ID);
    auto desc = g_type_registry.lookup(Counter::TYPE_ID);
    KTEST_EXPECT_TRUE(desc.has_value());
}

KTEST_WITH_INIT(obj_counter_emplace_with_initial, "obj/counter", obj_counter_init) {
    HandleTable table;
    auto id  = table.emplace<Counter>(RIGHTS_ALL, static_cast<uint64_t>(42)).unwrap();
    auto ctr = table.get<Counter>(id).unwrap();
    KTEST_EXPECT_TRUE(ctr->value() == 42);
}

KTEST_WITH_INIT(obj_counter_increment, "obj/counter", obj_counter_init) {
    HandleTable table;
    auto id       = table.emplace<Counter>(RIGHTS_ALL, static_cast<uint64_t>(10)).unwrap();
    auto ctr      = table.get<Counter>(id).unwrap();
    uint64_t prev = ctr->increment(5);
    KTEST_EXPECT_TRUE(prev == 10);
    KTEST_EXPECT_TRUE(ctr->value() == 15);
}

KTEST_WITH_INIT(obj_counter_reset, "obj/counter", obj_counter_init) {
    HandleTable table;
    auto id  = table.emplace<Counter>(RIGHTS_ALL, static_cast<uint64_t>(99)).unwrap();
    auto ctr = table.get<Counter>(id).unwrap();
    ctr->reset();
    KTEST_EXPECT_TRUE(ctr->value() == 0);
}

KTEST_WITH_INIT(obj_counter_rights_enforcement, "obj/counter", obj_counter_init) {
    HandleTable table;
    auto id  = table.emplace<Counter>(RIGHT_READ, static_cast<uint64_t>(0)).unwrap();
    auto got = table.get<Counter>(id, RIGHT_WRITE);
    KTEST_EXPECT_TRUE(got.is_err());
    KTEST_EXPECT_TRUE(got.unwrap_err() == RESULT_RIGHTS_VIOLATION);
}

KTEST_WITH_INIT(obj_counter_and_event_coexist, "obj/counter", obj_counter_init) {
    HandleTable table;
    auto eid = table.emplace<Event>(RIGHTS_ALL).unwrap();
    auto cid = table.emplace<Counter>(RIGHTS_ALL, static_cast<uint64_t>(0)).unwrap();

    KTEST_EXPECT_TRUE(table.get<Event>(eid).is_ok());
    KTEST_EXPECT_TRUE(table.get<Counter>(cid).is_ok());
    KTEST_EXPECT_TRUE(table.get<Counter>(eid).is_err());
    KTEST_EXPECT_TRUE(table.get<Event>(cid).is_err());
    KTEST_EXPECT_TRUE(table.count() == 2);
}

KTEST_WITH_INIT(obj_counter_live_count_independent, "obj/counter", obj_counter_init) {
    HandleTable table;
    uint32_t evt_before = g_type_registry.live_count(Event::TYPE_ID);
    uint32_t ctr_before = g_type_registry.live_count(Counter::TYPE_ID);

    table.emplace<Event>(RIGHTS_ALL);
    table.emplace<Counter>(RIGHTS_ALL, static_cast<uint64_t>(0));

    KTEST_EXPECT_TRUE(g_type_registry.live_count(Event::TYPE_ID) == evt_before + 1);
    KTEST_EXPECT_TRUE(g_type_registry.live_count(Counter::TYPE_ID) == ctr_before + 1);
}

#endif
