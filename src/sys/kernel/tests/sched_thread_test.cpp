// src/sys/kernel/tests/sched_thread_test.cpp
#include <kernel/obj/type_registry.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>
#include <kernel/testing/testing.h>

#include <ktl/string_view>

using namespace kernel::sched;
using kernel::obj::g_type_registry;

KTEST_MODULE_WITH_INIT("sched/thread", sched_thread_init);

static void sched_thread_init() {
    // Fork-per-test isolation gives each test a fresh registry; direct registration is safe.
    Task::register_type(g_type_registry).expect("task type registration failed");
    Thread::register_type(g_type_registry).expect("thread type registration failed");
}

// Stackless, spawned, and adopting threads all keep their parent task.
KTEST_CASE(sched_thread_construction_variants) {
    auto task = ktl::make_ref<Task>();
    Thread d(task);
    KTEST_EXPECT_TRUE(d.state() == thread_state::READY);
    KTEST_EXPECT_ALL(d.saved_sp() == 0u, d.kstack_phys() == 0u, d.kstack_floor() == 0u);
    KTEST_EXPECT_TRUE(d.type_id() == kernel::obj::type_ids::THREAD);

    // Spawned: owns a fresh stack with a tripwire floor; queued, not running,
    // until the scheduler switches to it.
    Thread s("spawned", task, 0x40000, 0xFFFF800000040000u);
    KTEST_EXPECT_EQUAL(s.kstack_phys(), 0x40000u);
    KTEST_EXPECT_EQUAL(s.kstack_floor(), 0xFFFF800000040000u + CONFIG_KERNEL_STACK_TRIPWIRE_MARGIN);
    KTEST_EXPECT_EQUAL(s.kstack_top(), 0xFFFF800000040000u + CONFIG_KERNEL_STACK_SIZE);
    KTEST_EXPECT_TRUE(s.state() == thread_state::READY);
    KTEST_EXPECT_TRUE(ktl::string_view(s.name()) == "spawned");

    // Adopting: the context is already executing on its own stack, so it owns no stack
    // of its own and must not be reported as merely READY.
    Thread a("idle0", task, 0xFFFF800000090000u);
    KTEST_EXPECT_TRUE(a.state() == thread_state::RUNNING);
    KTEST_EXPECT_EQUAL(a.kstack_floor(), 0xFFFF800000090000u);
    KTEST_EXPECT_ALL(a.kstack_phys() == 0u, a.kstack_top() == 0u);
    KTEST_EXPECT_ALL(d.owner() == task, s.owner() == task, a.owner() == task);
}

// Runtime state over one thread: state transitions, timeslice saturation, termination signal.
KTEST_CASE(sched_thread_runtime_state) {
    Thread t(kernel_task());
    t.set_state(thread_state::RUNNING);
    KTEST_EXPECT_TRUE(t.state() == thread_state::RUNNING);

    t.reset_slice();
    for (uint32_t i = 0; i < CONFIG_SCHED_TIMESLICE_TICKS - 1; ++i) { KTEST_EXPECT_TRUE(t.decrement_slice() > 0); }
    KTEST_EXPECT_EQUAL(t.decrement_slice(), 0u);
    KTEST_EXPECT_EQUAL(t.decrement_slice(), 0u);  // saturates, no wrap

    t.signal_set(Thread::SIGNAL_TERMINATED);
    KTEST_EXPECT_TRUE((t.signals() & Thread::SIGNAL_TERMINATED) != 0);
    t.set_state(thread_state::DEAD);
    KTEST_EXPECT_TRUE(t.state() == thread_state::DEAD);
}

static_assert(!__is_constructible(Thread));
static_assert(!__is_constructible(Thread, Thread&&));

KTEST_CASE(sched_thread_keeps_parent_alive) {
    auto task = ktl::make_ref<Task>();
    KTEST_REQUIRE_TRUE(static_cast<bool>(task));
    auto id     = task->id();
    auto thread = ktl::make_ref<Thread>(task);
    KTEST_REQUIRE_TRUE(static_cast<bool>(thread));
    task.reset();
    KTEST_EXPECT_EQUAL(thread->owner()->id(), id);
    KTEST_EXPECT_EQUAL(g_type_registry.live_count(Task::TYPE_ID), 1u);
    thread.reset();
    KTEST_EXPECT_EQUAL(g_type_registry.live_count(Task::TYPE_ID), 0u);
}

KTEST_CASE_CRASH(sched_thread_rejects_null_parent) { Thread thread(ktl::ref<Task>{}); }
KTEST_CASE_CRASH(sched_thread_adopting_rejects_null_parent) { Thread thread("idle", {}, uintptr_t{0}); }
KTEST_CASE_CRASH(sched_thread_spawned_rejects_null_parent) { Thread thread("spawned", {}, 0x40000, 0x80000); }
