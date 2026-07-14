#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/handle_table.h>
#include <kernel/sched/task.h>

using namespace kernel::obj;

static void handle_insert_init() {
    kernel::sched::Task::register_type(g_type_registry).expect("task type registration failed");
}

KTEST_WITH_INIT(handle_insert_existing_object, "obj/handle_table", handle_insert_init) {
    HandleTable table;
    auto task = ktl::make_ref<kernel::sched::Task>();
    KTEST_REQUIRE_TRUE(static_cast<bool>(task));
    ktl::ref<Object> object = task;
    auto id                 = table.insert(object, RIGHT_READ);
    KTEST_REQUIRE_TRUE(id.is_ok());
    auto got = table.get<kernel::sched::Task>(id.unwrap());
    KTEST_REQUIRE_TRUE(got.is_ok());
    KTEST_EXPECT_TRUE(got.unwrap().get() == task.get());
    KTEST_EXPECT_TRUE(table.get<kernel::sched::Task>(id.unwrap(), RIGHT_WRITE).is_err());
}

KTEST_WITH_INIT(handle_table_clear_closes_all, "obj/handle_table", handle_insert_init) {
    HandleTable table;
    auto task               = ktl::make_ref<kernel::sched::Task>();
    ktl::ref<Object> object = task;
    auto a                  = table.insert(object, RIGHT_READ);
    auto b                  = table.insert(object, RIGHT_READ);
    KTEST_REQUIRE_TRUE(a.is_ok() && b.is_ok());
    KTEST_EXPECT_EQUAL(table.count(), 2u);
    table.clear();
    KTEST_EXPECT_EQUAL(table.count(), 0u);
    KTEST_EXPECT_TRUE(!table.is_valid(a.unwrap()));
}

#endif
