#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/type_registry.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>

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

#endif
