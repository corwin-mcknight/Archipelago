#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/test_objects.h>

using namespace kernel::testing;
using namespace kernel::obj;

static void obj_event_init() { register_all_test_types(); }

KTEST_WITH_INIT(obj_event_register_type, "obj/event", obj_event_init) {
    KTEST_EXPECT_TRUE(Event::TYPE_ID != 0);
    KTEST_EXPECT_TRUE(g_type_registry.lookup(Event::TYPE_ID).has_value());
}

KTEST_WITH_INIT(obj_event_emplace, "obj/event", obj_event_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<Event>(RIGHT_READ | RIGHT_SIGNAL));
    KTEST_EXPECT_ALL(id.is_valid(), table.count() == 1);
}

KTEST_WITH_INIT(obj_event_get, "obj/event", obj_event_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<Event>(RIGHT_READ | RIGHT_SIGNAL));
    KTEST_UNWRAP(got, table.get<Event>(id));
    KTEST_EXPECT_TRUE(got->type_id() == Event::TYPE_ID);
}

KTEST_WITH_INIT(obj_event_wrong_type, "obj/event", obj_event_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<Event>(RIGHTS_ALL));
    auto got = table.get<Counter>(id);
    KTEST_EXPECT_ALL(got.is_err(), got.unwrap_err() == RESULT_WRONG_TYPE);
}

KTEST_WITH_INIT(obj_event_signals, "obj/event", obj_event_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<Event>(RIGHTS_ALL));
    KTEST_UNWRAP(evt, table.get<Event>(id));
    evt->signal_set(0x05);
    KTEST_EXPECT_TRUE(evt->signals() == 0x05);
    evt->signal_clear(0x01);
    KTEST_EXPECT_TRUE(evt->signals() == 0x04);
}

KTEST_WITH_INIT(obj_event_close_destroys, "obj/event", obj_event_init) {
    HandleTable table;
    uint32_t before = g_type_registry.live_count(Event::TYPE_ID);
    KTEST_UNWRAP(id, table.emplace<Event>(RIGHTS_ALL));
    KTEST_EXPECT_TRUE(g_type_registry.live_count(Event::TYPE_ID) == before + 1);
    table.close(id);
    KTEST_EXPECT_TRUE(g_type_registry.live_count(Event::TYPE_ID) == before);
}

KTEST_WITH_INIT(obj_event_duplicate_survives_close, "obj/event", obj_event_init) {
    HandleTable table;
    uint32_t before = g_type_registry.live_count(Event::TYPE_ID);
    KTEST_UNWRAP(id1, table.emplace<Event>(RIGHTS_ALL));
    KTEST_UNWRAP(id2, table.duplicate(id1, RIGHTS_ALL));
    table.close(id1);
    KTEST_EXPECT_TRUE(g_type_registry.live_count(Event::TYPE_ID) == before + 1);
    table.close(id2);
    KTEST_EXPECT_TRUE(g_type_registry.live_count(Event::TYPE_ID) == before);
}

KTEST_WITH_INIT(obj_event_set_name, "obj/event", obj_event_init) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<Event>(RIGHTS_ALL));
    KTEST_UNWRAP(evt, table.get<Event>(id));
    evt->set_name("test_event");
    KTEST_EXPECT_TRUE(evt->name() != nullptr);
}

#endif
