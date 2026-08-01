#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/boot.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/user_task.h>

#include <ktl/vector>

using namespace kernel::sched;

KTEST_MODULE("kernel/task");

// End to end over the real boot path: the init module is loaded from the boot image by the ELF
// loader, runs in its own address space, and exits. A missing module fails the test loudly rather
// than silently skipping, because an image without init is a broken image.
KTEST_CASE_INTEGRATION(user_task_lifecycle) {
    const auto* module = kernel::boot::find_module("init");
    KTEST_REQUIRE_TRUE(module != nullptr);
    KTEST_REQUIRE_TRUE(module->size > 0);

    auto created = create_user_task("utest", module->data, module->size);
    KTEST_REQUIRE_TRUE(created.is_ok());
    ktl::ref<Task> task = created.unwrap();
    KTEST_EXPECT_TRUE(task->state() == task_state::RUNNING);

    ktl::vector<ktl::ref<Thread>> threads;
    KTEST_REQUIRE_TRUE(task->snapshot_threads(threads));
    KTEST_REQUIRE_EQUAL(threads.size(), 1u);

    for (int i = 0; i < 2000 && task->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }

    KTEST_REQUIRE_TRUE(task->state() == task_state::TERMINATED);
    KTEST_EXPECT_EQUAL(task->thread_count(), 0u);
    KTEST_EXPECT_TRUE(task->aspace() == nullptr);
    KTEST_EXPECT_TRUE(threads[0]->state() == thread_state::DEAD);
    KTEST_EXPECT_TRUE(threads[0]->stats().yields >= 2);

    ktl::vector<ktl::ref<Task>> tasks;
    KTEST_REQUIRE_TRUE(snapshot_tasks(tasks));
    for (size_t i = 0; i < tasks.size(); ++i) { KTEST_EXPECT_TRUE(tasks[i]->id() != task->id()); }
}

#endif
