#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/type_registry.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>
#include <kernel/sched/wait_queue.h>
#include <kernel/time.h>

using namespace kernel::sched;

namespace {
volatile int g_flag;
void set_flag_thread(void*) { g_flag = 1; }
void exit_immediately_thread(void*) {}
volatile int g_pings;
void ping_thread(void*) {
    for (int i = 0; i < 5; ++i) {
        g_pings = g_pings + 1;
        kernel::sched::yield();
    }
}
}  // namespace

KTEST(sched_current_is_kshell_thread, "kernel/sched") {
    auto self = current();
    KTEST_REQUIRE_TRUE(static_cast<bool>(self));
    KTEST_EXPECT_TRUE(self->state() == thread_state::RUNNING);
}

KTEST(sched_spawn_runs, "kernel/sched") {
    g_flag = 0;
    KTEST_UNWRAP(t, spawn("flagger", set_flag_thread, nullptr));
    for (int i = 0; i < 100000 && g_flag == 0; ++i) { yield(); }
    KTEST_EXPECT_EQUAL(g_flag, 1);
}

KTEST(sched_yield_interleaves, "kernel/sched") {
    g_pings = 0;
    KTEST_UNWRAP(t, spawn("pinger", ping_thread, nullptr));
    for (int i = 0; i < 100000 && g_pings < 5; ++i) { yield(); }
    KTEST_EXPECT_EQUAL(g_pings, 5);
}

KTEST(sched_exit_reaps_thread, "kernel/sched") {
    using kernel::obj::g_type_registry;
    uint32_t before = g_type_registry.live_count(Thread::TYPE_ID);
    {
        KTEST_UNWRAP(t, spawn("exiter", exit_immediately_thread, nullptr));
        // t drops at scope end; the zombie + reaper path must release the rest.
    }
    for (int i = 0; i < 100000 && g_type_registry.live_count(Thread::TYPE_ID) > before; ++i) { yield(); }
    KTEST_EXPECT_EQUAL(g_type_registry.live_count(Thread::TYPE_ID), before);
}

namespace {
volatile uint64_t g_spin_count;
volatile bool g_spin_stop;
void spinner_thread(void*) {
    while (!g_spin_stop) { g_spin_count = g_spin_count + 1; }
}
}  // namespace

KTEST(sched_preempts_spinner, "kernel/sched") {
    g_spin_count = 0;
    g_spin_stop  = false;
    KTEST_UNWRAP(t, spawn("spinner", spinner_thread, nullptr));
    // Busy-wait WITHOUT yielding: only preemption can give the spinner CPU time.
    ktime_t start = kernel::time::now();
    while (kernel::time::now() < start + 3 * CONFIG_SCHED_TIMESLICE_TICKS) {}
    uint64_t observed = g_spin_count;
    g_spin_stop       = true;
    KTEST_EXPECT_TRUE(observed > 0);
    for (int i = 0; i < 100000 && t->state() != thread_state::DEAD; ++i) { yield(); }
    KTEST_EXPECT_TRUE(t->state() == thread_state::DEAD);
}

namespace {
kernel::sched::wait_queue g_test_wq;
volatile int g_blocked_phase;
void blocker_thread(void*) {
    g_blocked_phase = 1;
    g_test_wq.block();
    g_blocked_phase = 2;
}
}  // namespace

KTEST(sched_sleep_advances_time, "kernel/sched") {
    ktime_t before = kernel::time::now();
    sleep_ticks(5);
    KTEST_EXPECT_TRUE(kernel::time::now() >= before + 5);
}

KTEST(sched_block_and_wake, "kernel/sched") {
    g_blocked_phase = 0;
    KTEST_UNWRAP(t, spawn("blocker", blocker_thread, nullptr));
    for (int i = 0; i < 100000 && g_blocked_phase == 0; ++i) { yield(); }
    KTEST_REQUIRE_EQUAL(g_blocked_phase, 1);
    sleep_ticks(3);  // give it slices; it must stay blocked, not spin
    KTEST_EXPECT_EQUAL(g_blocked_phase, 1);
    KTEST_EXPECT_TRUE(t->state() == thread_state::BLOCKED);
    g_test_wq.wake_one();
    for (int i = 0; i < 100000 && g_blocked_phase != 2; ++i) { yield(); }
    KTEST_EXPECT_EQUAL(g_blocked_phase, 2);
}

#endif
