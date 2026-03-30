#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/counter.h>
#include <kernel/obj/event.h>
#include <kernel/obj/handle_table.h>
#include <kernel/obj/type_registry.h>

using namespace kernel::testing;
using namespace kernel::obj;

static void obj_event_init() {
    static bool registered = false;
    if (!registered) {
        Event::register_type(g_type_registry);
        Counter::register_type(g_type_registry);
        registered = true;
    }
}

KTEST_WITH_INIT(obj_event_register_type, "obj/event", obj_event_init) {
    KTEST_EXPECT_TRUE(Event::TYPE_ID != 0);
    auto desc = g_type_registry.lookup(Event::TYPE_ID);
    KTEST_EXPECT_TRUE(desc.has_value());
}

KTEST_WITH_INIT(obj_event_emplace, "obj/event", obj_event_init) {
    HandleTable table;
    auto result = table.emplace<Event>(RIGHT_READ | RIGHT_SIGNAL);
    KTEST_REQUIRE_TRUE(result.is_ok());
    KTEST_EXPECT_TRUE(table.count() == 1);
}

KTEST_WITH_INIT(obj_event_get, "obj/event", obj_event_init) {
    HandleTable table;
    auto id  = table.emplace<Event>(RIGHT_READ | RIGHT_SIGNAL).unwrap();
    auto got = table.get<Event>(id);
    KTEST_REQUIRE_TRUE(got.is_ok());
    KTEST_EXPECT_TRUE(got.unwrap()->type_id() == Event::TYPE_ID);
}

KTEST_WITH_INIT(obj_event_wrong_type, "obj/event", obj_event_init) {
    HandleTable table;
    auto id  = table.emplace<Event>(RIGHTS_ALL).unwrap();
    auto got = table.get<Counter>(id);
    KTEST_EXPECT_TRUE(got.is_err());
    KTEST_EXPECT_TRUE(got.unwrap_err() == RESULT_WRONG_TYPE);
}

KTEST_WITH_INIT(obj_event_signals, "obj/event", obj_event_init) {
    HandleTable table;
    auto id  = table.emplace<Event>(RIGHTS_ALL).unwrap();
    auto evt = table.get<Event>(id).unwrap();
    evt->signal_set(0x05);
    KTEST_EXPECT_TRUE(evt->signals() == 0x05);
    evt->signal_clear(0x01);
    KTEST_EXPECT_TRUE(evt->signals() == 0x04);
}

KTEST_WITH_INIT(obj_event_close_destroys, "obj/event", obj_event_init) {
    HandleTable table;
    uint32_t before = g_type_registry.live_count(Event::TYPE_ID);
    auto id         = table.emplace<Event>(RIGHTS_ALL).unwrap();
    KTEST_EXPECT_TRUE(g_type_registry.live_count(Event::TYPE_ID) == before + 1);
    table.close(id);
    KTEST_EXPECT_TRUE(g_type_registry.live_count(Event::TYPE_ID) == before);
}

KTEST_WITH_INIT(obj_event_duplicate_survives_close, "obj/event", obj_event_init) {
    HandleTable table;
    uint32_t before = g_type_registry.live_count(Event::TYPE_ID);
    auto id1        = table.emplace<Event>(RIGHTS_ALL).unwrap();
    auto id2        = table.duplicate(id1, RIGHTS_ALL).unwrap();
    table.close(id1);
    // Object still alive via second handle
    KTEST_EXPECT_TRUE(g_type_registry.live_count(Event::TYPE_ID) == before + 1);
    table.close(id2);
    KTEST_EXPECT_TRUE(g_type_registry.live_count(Event::TYPE_ID) == before);
}

KTEST_WITH_INIT(obj_event_set_name, "obj/event", obj_event_init) {
    HandleTable table;
    auto id  = table.emplace<Event>(RIGHTS_ALL).unwrap();
    auto evt = table.get<Event>(id).unwrap();
    evt->set_name("test_event");
    KTEST_EXPECT_TRUE(evt->name() != nullptr);
}

#endif
