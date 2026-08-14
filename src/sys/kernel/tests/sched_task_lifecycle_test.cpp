#include <kernel/sched/task.h>
#include <kernel/testing/testing.h>

using namespace kernel::sched;

KTEST_MODULE("sched/task");

KTEST_CASE(task_registry_snapshot) {
    ktl::vector<ktl::ref<Task>> before;
    KTEST_REQUIRE_TRUE(snapshot_tasks(before));
    auto task = ktl::make_ref<Task>();
    register_task(task);
    ktl::vector<ktl::ref<Task>> during;
    KTEST_REQUIRE_TRUE(snapshot_tasks(during));
    KTEST_EXPECT_EQUAL(during.size(), before.size() + 1);
    unregister_task(task->id());
    ktl::vector<ktl::ref<Task>> after;
    KTEST_REQUIRE_TRUE(snapshot_tasks(after));
    KTEST_EXPECT_EQUAL(after.size(), before.size());
}
