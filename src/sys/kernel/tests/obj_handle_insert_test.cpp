#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/handle_table.h>
#include <kernel/sched/task.h>

using namespace kernel::obj;

KTEST_MODULE_WITH_INIT("obj/handle_table", handle_insert_init);

static void handle_insert_init() {
    kernel::sched::Task::register_type(g_type_registry).expect("task type registration failed");
}

// insert() wraps an already-constructed object: the handle resolves back to the same
// instance and enforces the granted rights, and clear() closes every handle at once.
KTEST_CASE(handle_insert_existing_object_and_clear) {
    HandleTable table;
    auto task = ktl::make_ref<kernel::sched::Task>();
    KTEST_REQUIRE_TRUE(static_cast<bool>(task));
    ktl::ref<Object> object = task;
    KTEST_UNWRAP(a, table.insert(object, RIGHT_READ));
    KTEST_UNWRAP(got, table.get<kernel::sched::Task>(a));
    KTEST_EXPECT_TRUE(got.get() == task.get());
    KTEST_EXPECT_TRUE(table.get<kernel::sched::Task>(a, RIGHT_WRITE).is_err());

    KTEST_REQUIRE_TRUE(table.insert(object, RIGHT_READ).is_ok());
    KTEST_EXPECT_EQUAL(table.count(), 2u);
    table.clear();
    KTEST_EXPECT_EQUAL(table.count(), 0u);
    KTEST_EXPECT_TRUE(!table.is_valid(a));
}

#endif
