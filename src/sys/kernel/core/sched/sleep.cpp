// src/sys/kernel/core/sched/sleep.cpp
#include <kernel/arch.h>
#include <kernel/assert.h>
#include <kernel/log.h>
#include <kernel/sched/internal.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/wait_queue.h>
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

// Waiters parked on some object's wait queue with a deadline. Unlike sleepers, an entry does not
// own the thread -- the wait node on the waiter's stack does. The entry only records where to look:
// on expiry the scan asks the queue to claim() the node, which resolves the race against signal
// wakers under that queue's own lock. Entries leave the list on successful claim or when the
// waiter unregisters after waking; an entry whose claim fails stays until the waiter removes it,
// so the node pointer is valid for exactly as long as the entry exists.
struct timed_wait {
    ktime_t wake_at   = 0;
    wait_queue* queue = nullptr;
    wait_node* node   = nullptr;
};
ktl::vector<timed_wait> g_timed_waits;

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
    // A killed thread must not enter the sleeper list: its deadline could be arbitrarily far out,
    // and the kill scan that wakes sleepers early has already run. Checked with interrupts off so
    // a kill cannot slip between the check and the park; the thread exits at the syscall boundary.
    if (c.current->killed()) {
        kernel::arch::restore_interrupts(flags);
        return;
    }
    c.current->stats().sleeps += 1;
    c.current->set_state(thread_state::BLOCKED);
    bool ok = g_sleepers.push_back(sleeper{kernel::time::now() + ticks, c.current});
    ensure(ok, "sleep_ticks: sleeper push failed despite reservation");
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

bool wake_sleeper(Thread* thread) {
    for (size_t i = 0; i < g_sleepers.size(); i++) {
        if (g_sleepers[i].thread.get() != thread) { continue; }
        ktl::ref<Thread> woken = ktl::move(g_sleepers[i].thread);
        g_sleepers.swap_remove(i);
        make_ready(ktl::move(woken));
        return true;
    }
    return false;
}

bool sleepers_reserve(size_t capacity) {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    bool ok        = g_sleepers.reserve(capacity) && g_timed_waits.reserve(capacity);
    kernel::arch::restore_interrupts(flags);
    return ok;
}

void timed_wait_register(wait_queue* queue, wait_node* node, ktime_t wake_at) {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    bool ok        = g_timed_waits.push_back(timed_wait{wake_at, queue, node});
    ensure(ok, "timed_wait_register: push failed despite reservation");
    kernel::arch::restore_interrupts(flags);
}

void timed_wait_unregister(wait_node* node) {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    for (size_t i = 0; i < g_timed_waits.size(); i++) {
        if (g_timed_waits[i].node == node) {
            g_timed_waits.swap_remove(i);
            break;
        }
    }
    // Absent is normal: the expiry scan already removed the entry when it claimed the thread.
    kernel::arch::restore_interrupts(flags);
}

void wake_due_timed_waits() {
    for (size_t i = 0; i < g_timed_waits.size();) {
        if (g_timed_waits[i].wake_at > kernel::time::now()) {
            ++i;
            continue;
        }
        // claim() resolves the race against signal wakers under the queue's lock. A failed claim
        // means the waiter is not parked (not yet, or already woken); leave the entry -- the
        // waiter's unregister removes it, and its deadline predicate keeps it from re-parking.
        ktl::ref<Thread> woken = g_timed_waits[i].queue->claim(g_timed_waits[i].node);
        if (!woken) {
            ++i;
            continue;
        }
        g_timed_waits.swap_remove(i);
        make_ready(ktl::move(woken));
    }
}

}  // namespace kernel::sched
