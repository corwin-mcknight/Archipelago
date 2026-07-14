#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/sched/task.h>

using namespace kernel::sched;

KTEST(task_lifecycle_states, "sched/task") {
    auto task = ktl::make_ref<Task>();
    KTEST_REQUIRE_TRUE(static_cast<bool>(task));
    KTEST_EXPECT_TRUE(task->state() == task_state::NEW);
    task->set_state(task_state::RUNNING);
    KTEST_EXPECT_TRUE(task->state() == task_state::RUNNING);
    task->set_state(task_state::TERMINATED);
    KTEST_EXPECT_TRUE(task->state() == task_state::TERMINATED);
    KTEST_EXPECT_TRUE(task->aspace() == nullptr);
}

KTEST(task_registry_snapshot, "sched/task") {
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

#endif
