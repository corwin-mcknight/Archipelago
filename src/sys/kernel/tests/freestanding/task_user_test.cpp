#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/boot.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/user_task.h>
#include <kernel/syscall.h>

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

    // The self-handle ABI: a fresh table whose first-generation slots 0 and 1 hold the task and
    // thread, in that order, exactly as SELF_TASK_HANDLE / SELF_THREAD_HANDLE promise. The window
    // is safe: init sleeps before exiting, so the table cannot have been torn down yet.
    {
        using namespace kernel::obj;
        KTEST_REQUIRE_VALUE(self_task, task->handles().info(HandleId{0, 0}));
        KTEST_REQUIRE_VALUE(self_thread, task->handles().info(HandleId{1, 0}));
        KTEST_EXPECT_ALL(self_task.type_id == type_ids::TASK, self_task.rights == (RIGHT_READ | RIGHT_WRITE));
        KTEST_EXPECT_ALL(self_thread.type_id == type_ids::THREAD, self_thread.rights == (RIGHT_READ | RIGHT_WAIT));
    }

    // Drive the dispatch pipeline through the real syscall entry from kernel context: this thread
    // belongs to task zero, so operations land on the kernel table, where the new task's owner
    // handle carries DUPLICATE -- the success path the self-handles deliberately cannot reach.
    {
        using namespace kernel::obj;
        auto owner      = task->owner_handle();
        uint64_t packed = static_cast<uint64_t>(owner.index) | (static_cast<uint64_t>(owner.generation) << 32);

        uint64_t info   = syscall_dispatch(kernel::syscall::SYS_OBJ_INFO, packed, 0, 0, 0, 0, 0);
        KTEST_EXPECT_ALL((info & 0xFFFFFFFF) == type_ids::TASK,
                         (info >> 32) == (RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE));

        uint64_t dup = syscall_dispatch(kernel::syscall::SYS_HANDLE_DUPLICATE, packed, RIGHT_READ, 0, 0, 0, 0);
        KTEST_REQUIRE_TRUE(static_cast<int64_t>(dup) >= 0);
        uint64_t dup_info = syscall_dispatch(kernel::syscall::SYS_OBJ_INFO, dup, 0, 0, 0, 0, 0);
        KTEST_EXPECT_TRUE((dup_info >> 32) == RIGHT_READ);
        KTEST_EXPECT_TRUE(syscall_dispatch(kernel::syscall::SYS_HANDLE_CLOSE, dup, 0, 0, 0, 0, 0) == 0);
    }

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
