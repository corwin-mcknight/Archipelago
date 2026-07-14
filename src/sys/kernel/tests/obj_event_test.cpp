#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/testing/test_objects.h>

using namespace kernel::testing;
using namespace kernel::obj;

KTEST_MODULE_WITH_INIT("obj/event", obj_event_init);

static void obj_event_init() { register_all_test_types(); }

// One event handle through its whole life: the type is registered, emplace yields a valid
// handle, the typed get works while a cross-type get is rejected, signals set/clear, the
// object can be named, and closing the handle destroys the object.
KTEST_CASE(obj_event_handle_lifecycle) {
    KTEST_EXPECT_TRUE(Event::TYPE_ID != 0);
    KTEST_EXPECT_TRUE(g_type_registry.lookup(Event::TYPE_ID).has_value());

    HandleTable table;
    uint32_t before = g_type_registry.live_count(Event::TYPE_ID);
    KTEST_UNWRAP(id, table.emplace<Event>(RIGHT_READ | RIGHT_SIGNAL));
    KTEST_EXPECT_ALL(id.is_valid(), table.count() == 1, g_type_registry.live_count(Event::TYPE_ID) == before + 1);

    {
        KTEST_UNWRAP(evt, table.get<Event>(id));
        KTEST_EXPECT_TRUE(evt->type_id() == Event::TYPE_ID);

        auto wrong = table.get<Counter>(id);
        KTEST_EXPECT_ALL(wrong.is_err(), wrong.unwrap_err() == ktl::errc::wrong_type);

        evt->signal_set(0x05);
        KTEST_EXPECT_TRUE(evt->signals() == 0x05);
        evt->signal_clear(0x01);
        KTEST_EXPECT_TRUE(evt->signals() == 0x04);

        evt->set_name("test_event");
        KTEST_EXPECT_TRUE(evt->name() != nullptr);
    }  // drop the get() ref so close holds the last reference

    KTEST_EXPECT_TRUE(table.close(id).is_ok());
    KTEST_EXPECT_TRUE(g_type_registry.live_count(Event::TYPE_ID) == before);
}

// A duplicated handle keeps the object alive after the original closes; the object dies
// only when the last handle goes.
KTEST_CASE(obj_event_duplicate_survives_close) {
    HandleTable table;
    uint32_t before = g_type_registry.live_count(Event::TYPE_ID);
    KTEST_UNWRAP(id1, table.emplace<Event>(RIGHT_READ | RIGHT_SIGNAL | RIGHT_DUPLICATE));
    KTEST_UNWRAP(id2, table.duplicate(id1, RIGHTS_ALL));
    KTEST_EXPECT_TRUE(table.close(id1).is_ok());
    KTEST_EXPECT_TRUE(g_type_registry.live_count(Event::TYPE_ID) == before + 1);
    KTEST_EXPECT_TRUE(table.close(id2).is_ok());
    KTEST_EXPECT_TRUE(g_type_registry.live_count(Event::TYPE_ID) == before);
}

#endif
