// src/sys/kernel/core/sched/sleep.cpp
#include <kernel/arch.h>
#include <kernel/assert.h>
#include <kernel/log.h>
#include <kernel/sched/internal.h>
#include <kernel/sched/scheduler.h>
#include <kernel/synchronization/execution_context.h>
#include <kernel/time.h>

#include <ktl/vector>

namespace kernel::sched {

namespace {

struct sleeper {
    ktime_t wake_at = 0;
    ktl::ref<Thread> thread;
};
ktl::vector<sleeper> g_sleepers;

}  // namespace

void sleep_ticks(uint64_t ticks) {
    kernel::synchronization::assert_blocking_allowed("sleep_ticks: scheduling is forbidden in this context");
    kernel::synchronization::assert_no_locks_held("sleep_ticks: scheduling while holding a lock");
    if (ticks == 0) {
        yield();
        return;
    }
    if (lifecycle_log_verbose_enabled()) { g_log.debug("sched: sleep id={0} ticks={1}", current()->id(), ticks); }
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    auto& c        = cur_cpu();
    assert(c.current.get() != c.idle.get(), "sleep_ticks: idle thread cannot sleep");
    c.current->stats().sleeps += 1;
    c.current->set_state(thread_state::BLOCKED);
    bool ok = g_sleepers.push_back(sleeper{kernel::time::now() + ticks, c.current});
    assert(ok, "sleep_ticks: sleeper allocation failed");
    schedule_out(switch_reason::SLEEP);
    kernel::arch::restore_interrupts(flags);
}

void wake_due_sleepers() {
    for (size_t i = 0; i < g_sleepers.size();) {
        if (g_sleepers[i].wake_at <= kernel::time::now()) {
            ktl::ref<Thread> t = ktl::move(g_sleepers[i].thread);
            g_sleepers.swap_remove(i);
            make_ready(ktl::move(t));
        } else {
            ++i;
        }
    }
}

size_t sleeper_count() { return g_sleepers.size(); }

}  // namespace kernel::sched
