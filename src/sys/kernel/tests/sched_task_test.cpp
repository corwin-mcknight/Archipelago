#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/event.h>
#include <kernel/obj/type_registry.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>

#include <ktl/ref>
#include <ktl/vector>

using namespace kernel::sched;
using namespace kernel::obj;

static void sched_task_init() {
    Thread::register_type(g_type_registry).expect("thread type registration failed");
    Task::register_type(g_type_registry).expect("task type registration failed");
    Event::register_type(g_type_registry).expect("event type registration failed");
}

KTEST_WITH_INIT(sched_task_kernel_task_is_singleton, "sched/task", sched_task_init) {
    auto a = kernel_task();
    auto b = kernel_task();
    KTEST_REQUIRE_TRUE(static_cast<bool>(a));
    KTEST_EXPECT_TRUE(a == b);
    KTEST_EXPECT_TRUE(a->type_id() == type_ids::TASK);
}

KTEST_WITH_INIT(sched_task_thread_list, "sched/task", sched_task_init) {
    Task task;
    auto t1 = ktl::make_ref<Thread>();
    auto t2 = ktl::make_ref<Thread>();
    KTEST_EXPECT_TRUE(task.add_thread(t1).is_ok());
    KTEST_EXPECT_TRUE(task.add_thread(t2).is_ok());
    KTEST_EXPECT_EQUAL(task.thread_count(), 2u);
    task.remove_thread(t1->id());
    KTEST_EXPECT_EQUAL(task.thread_count(), 1u);
    task.remove_thread(t1->id());  // removing an absent id is a no-op
    KTEST_EXPECT_EQUAL(task.thread_count(), 1u);
}

KTEST_WITH_INIT(sched_task_owns_handles, "sched/task", sched_task_init) {
    auto kt       = kernel_task();
    size_t before = kt->handles().count();
    KTEST_UNWRAP(id, kt->handles().emplace<Event>(RIGHT_READ | RIGHT_SIGNAL));
    KTEST_EXPECT_EQUAL(kt->handles().count(), before + 1);
    KTEST_EXPECT_TRUE(kt->handles().close(id).is_ok());
}

KTEST_WITH_INIT(sched_task_snapshot_threads, "sched/task", sched_task_init) {
    Task task;
    auto t1 = ktl::make_ref<Thread>();
    auto t2 = ktl::make_ref<Thread>();
    KTEST_REQUIRE_TRUE(task.add_thread(t1).is_ok());
    KTEST_REQUIRE_TRUE(task.add_thread(t2).is_ok());
    ktl::vector<ktl::ref<Thread>> snap;
    KTEST_REQUIRE_TRUE(task.snapshot_threads(snap));
    KTEST_EXPECT_EQUAL(snap.size(), 2u);
    task.remove_thread(t1->id());
    KTEST_EXPECT_EQUAL(snap.size(), 2u);  // snapshot is a copy, not a view
}

#endif
