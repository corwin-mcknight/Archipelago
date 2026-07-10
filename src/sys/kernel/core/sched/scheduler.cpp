// src/sys/kernel/core/sched/scheduler.cpp
#include <kernel/arch.h>
#include <kernel/assert.h>
#include <kernel/config.h>
#include <kernel/log.h>
#include <kernel/mm/pmm.h>
#include <kernel/panic.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/wait_queue.h>
#include <kernel/time.h>

#include <ktl/deque>
#include <ktl/stack>
#include <ktl/vector>

extern uintptr_t g_hhdm_offset;

// Stack tripwire floor for the running thread, consumed by the arch trap entries. Published on
// every switch. Zero disables the check (x86 idle; pre-scheduler riscv boot writes a real floor
// from trap_init).
extern "C" uintptr_t g_kstack_floor = 0;

namespace kernel::sched {

struct cpu_sched {
    ktl::deque<ktl::ref<Thread>> run_queue;
    ktl::ref<Thread> current;
    ktl::ref<Thread> idle;
    // Outgoing thread of the in-flight switch; dropped by sched_finish_switch() on the incoming
    // thread's stack so a dead thread's final ref never dies on its own stack.
    ktl::ref<Thread> previous;
};

// All scheduler state below is guarded by interrupts-off on the single scheduling core.
// ponytail: g_boot_core indexing, not a per-CPU pointer; GS/tp per-CPU state when APs schedule.
cpu_sched g_cpus[CONFIG_MAX_CORES];
uint32_t g_boot_core = 0;

cpu_sched& cur_cpu() { return g_cpus[g_boot_core]; }

namespace {

bool g_started = false;

// Dead threads awaiting the reaper, and recycled stacks. Stacks come from alloc_contiguous,
// which has no contiguous free -- the cache recycles them among threads instead; the pool is
// bounded by the peak live thread count.
ktl::deque<ktl::ref<Thread>> g_zombies;
ktl::stack<kernel::mm::vm_paddr_t> g_stack_cache;
wait_queue g_reaper_wq;

struct sleeper {
    ktime_t wake_at = 0;
    ktl::ref<Thread> thread;
};
ktl::vector<sleeper> g_sleepers;

trace_ring<CONFIG_SCHED_TRACE_EVENTS> g_trace;
global_stats g_stats;
uint64_t g_last_switch_ts = 0;
bool g_lifecycle_log      = false;

void trace_push(trace_kind kind, switch_reason reason, uint64_t from, uint64_t to) {
    trace_record r;
    r.timestamp = kernel::arch::timestamp();
    r.kind      = kind;
    r.reason    = reason;
    r.from_id   = from;
    r.to_id     = to;
    g_trace.push(r);
}

// Interrupts must be disabled. Returns when this thread is next switched in.
void switch_to(ktl::ref<Thread> next, switch_reason reason) {
    auto& c      = cur_cpu();
    uint64_t now = kernel::arch::timestamp();
    c.current->stats().cpu_cycles += now - g_last_switch_ts;
    g_last_switch_ts = now;

    next->stats().scheduled += 1;
    if (next->ready_ts() != 0) {
        uint64_t lat = now - next->ready_ts();
        next->stats().lat_total_cycles += lat;
        if (lat > next->stats().lat_max_cycles) { next->stats().lat_max_cycles = lat; }
        next->set_ready_ts(0);
    }

    g_stats.switches += 1;
    switch (reason) {
        case switch_reason::PREEMPT: g_stats.preempts += 1; break;
        case switch_reason::YIELD: g_stats.yields += 1; break;
        case switch_reason::BLOCK: g_stats.block_switches += 1; break;
        case switch_reason::SLEEP: g_stats.sleep_switches += 1; break;
        case switch_reason::EXIT: g_stats.exit_switches += 1; break;
        case switch_reason::NONE:
        default: break;
    }
    trace_push(trace_kind::SWITCH, reason, c.current->id(), next->id());

    next->set_state(thread_state::RUNNING);
    next->reset_slice();
    Thread* outgoing = c.current.get();
    assert(c.previous.get() == nullptr, "switch_to: unfinished previous switch");
    c.previous     = ktl::move(c.current);
    c.current      = ktl::move(next);
    g_kstack_floor = c.current->kstack_floor();
    arch_context_switch(outgoing->saved_sp_slot(), c.current->saved_sp());
    sched_finish_switch();
}

// Returns a stack to the cache; shared by reap() and spawn()'s failure paths.
void release_stack(kernel::mm::vm_paddr_t phys) {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    bool ok        = g_stack_cache.push(phys);
    kernel::arch::restore_interrupts(flags);
    if (!ok) { g_log.warn("sched: stack cache full; leaking a thread stack"); }
}

void reap(ktl::ref<Thread> zombie) {
    if (lifecycle_log_enabled()) { g_log.debug("sched: reap id={0}", zombie->id()); }
    kernel_task()->remove_thread(zombie->id());
    if (zombie->kstack_phys() != 0) { release_stack(zombie->kstack_phys()); }
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    g_stats.reaped += 1;
    kernel::arch::restore_interrupts(flags);
}

[[noreturn]] void reaper_main(void*) {
    while (true) {
        ktl::maybe<ktl::ref<Thread>> zombie;
        uint64_t flags = kernel::arch::save_and_disable_interrupts();
        zombie         = g_zombies.pop_front();
        kernel::arch::restore_interrupts(flags);
        if (!zombie.has_value()) {
            g_reaper_wq.block_if(0, [](void*) { return g_zombies.size() == 0; }, nullptr);
            continue;
        }
        reap(ktl::move(*zombie));
    }
}

}  // namespace

bool started() { return g_started; }

ktl::ref<Thread> current() {
    uint64_t flags     = kernel::arch::save_and_disable_interrupts();
    ktl::ref<Thread> t = cur_cpu().current;
    kernel::arch::restore_interrupts(flags);
    return t;
}

bool current_is_idle() {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    auto& c        = cur_cpu();
    bool idle      = c.current.get() == c.idle.get();
    kernel::arch::restore_interrupts(flags);
    return idle;
}

void make_ready(ktl::ref<Thread> thread) {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    assert(thread->state() == thread_state::BLOCKED, "make_ready: thread is not blocked");
    thread->set_state(thread_state::READY);
    thread->set_ready_ts(kernel::arch::timestamp());
    thread->stats().wakes += 1;
    g_stats.wakes += 1;
    trace_push(trace_kind::WAKE, switch_reason::NONE, cur_cpu().current ? cur_cpu().current->id() : 0, thread->id());
    bool ok = cur_cpu().run_queue.push_back(ktl::move(thread));
    assert(ok, "make_ready: run queue allocation failed");
    kernel::arch::restore_interrupts(flags);
}

void schedule_out(switch_reason reason) {
    auto& c   = cur_cpu();
    auto next = c.run_queue.pop_front();
    switch_to(next.has_value() ? ktl::move(*next) : c.idle, reason);
}

void yield() {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    auto& c        = cur_cpu();
    auto next      = c.run_queue.pop_front();
    if (!next.has_value() && c.current.get() != c.idle.get()) {
        // Queue empty: hand off to idle. The boot context runs as the idle thread and is never
        // re-queued after a tick preemption, so a cooperatively-yielding thread would otherwise
        // starve it forever.
        // ponytail: this is a double-switch per empty-queue yield (caller -> idle, then idle's
        // own yield back out); revisit if this path ever gets hot.
        next = c.idle;
    }
    if (next.has_value()) {
        if (c.current.get() != c.idle.get()) {
            c.current->stats().yields += 1;
            c.current->set_state(thread_state::READY);
            c.current->set_ready_ts(kernel::arch::timestamp());
            bool ok = c.run_queue.push_back(c.current);
            assert(ok, "yield: run queue allocation failed");
        }
        switch_to(ktl::move(*next), switch_reason::YIELD);
    }
    kernel::arch::restore_interrupts(flags);
}

void sleep_ticks(uint64_t ticks) {
    if (ticks == 0) {
        yield();
        return;
    }
    if (lifecycle_log_enabled()) { g_log.debug("sched: sleep id={0} ticks={1}", current()->id(), ticks); }
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

void on_tick() {
    if (!g_started) { return; }
    auto& c = cur_cpu();
    // Wake due sleepers first so a woken thread can be this tick's switch target.
    // ponytail: linear scan; a deadline-ordered list when thread counts grow.
    for (size_t i = 0; i < g_sleepers.size();) {
        if (g_sleepers[i].wake_at <= kernel::time::now()) {
            ktl::ref<Thread> t = ktl::move(g_sleepers[i].thread);
            g_sleepers[i]      = ktl::move(g_sleepers[g_sleepers.size() - 1]);
            auto discard       = g_sleepers.pop_back();
            make_ready(ktl::move(t));
        } else {
            ++i;
        }
    }
    if (c.run_queue.size() == 0) { return; }
    bool idle_running = (c.current.get() == c.idle.get());
    if (!idle_running && c.current->decrement_slice() > 0) { return; }
    auto next = c.run_queue.pop_front();
    if (!idle_running) {
        c.current->stats().preemptions += 1;
        c.current->set_state(thread_state::READY);
        c.current->set_ready_ts(kernel::arch::timestamp());
        bool ok = c.run_queue.push_back(c.current);
        assert(ok, "on_tick: run queue allocation failed");
    }
    switch_to(ktl::move(*next), switch_reason::PREEMPT);
}

ktl::result<ktl::ref<Thread>> spawn(const char* name, thread_entry_fn entry, void* arg) {
    constexpr size_t STACK_PAGES            = CONFIG_KERNEL_STACK_SIZE / KERNEL_MINIMUM_PAGE_SIZE;

    uint64_t flags                          = kernel::arch::save_and_disable_interrupts();
    ktl::maybe<kernel::mm::vm_paddr_t> phys = g_stack_cache.pop();
    kernel::arch::restore_interrupts(flags);
    if (!phys.has_value()) { phys = kernel::mm::g_page_frame_allocator.alloc_contiguous(STACK_PAGES); }
    if (!phys.has_value()) { return ktl::err(ktl::errc::oom); }

    uintptr_t virt_base = g_hhdm_offset + *phys;
    auto thread         = ktl::make_ref<Thread>(*phys, virt_base);
    if (!thread) {
        release_stack(*phys);
        return ktl::err(ktl::errc::oom);
    }
    thread->set_name(name);
    thread->set_saved_sp(kernel::arch::prepare_thread_stack(virt_base + CONFIG_KERNEL_STACK_SIZE, entry, arg));

    auto added = kernel_task()->add_thread(thread);
    if (added.is_err()) {
        release_stack(*phys);
        return ktl::err(added.unwrap_err());
    }

    flags = kernel::arch::save_and_disable_interrupts();
    thread->set_ready_ts(kernel::arch::timestamp());
    g_stats.spawned += 1;
    trace_push(trace_kind::SPAWN, switch_reason::NONE, cur_cpu().current ? cur_cpu().current->id() : 0, thread->id());
    bool ok = cur_cpu().run_queue.push_back(thread);
    kernel::arch::restore_interrupts(flags);
    if (!ok) {
        kernel_task()->remove_thread(thread->id());
        release_stack(*phys);
        return ktl::err(ktl::errc::oom);
    }
    if (lifecycle_log_enabled()) { g_log.debug("sched: spawn '{0}' id={1}", name, thread->id()); }
    return ktl::result<ktl::ref<Thread>>::ok(thread);
}

[[noreturn]] void exit_current() {
    if (lifecycle_log_enabled()) { g_log.debug("sched: exit id={0}", current()->id()); }
    kernel::arch::disable_interrupts();
    auto& c = cur_cpu();
    assert(c.current.get() != c.idle.get(), "exit_current: idle thread cannot exit");
    c.current->set_state(thread_state::DEAD);
    c.current->signal_set(Thread::SIGNAL_TERMINATED);
    trace_push(trace_kind::EXIT, switch_reason::NONE, c.current->id(), 0);
    bool ok = g_zombies.push_back(c.current);
    assert(ok, "exit_current: zombie list allocation failed");
    g_reaper_wq.wake_one();
    schedule_out(switch_reason::EXIT);
    panic("exit_current: switched back into a dead thread");
}

void init(uint32_t boot_core_index) {
    g_boot_core = boot_core_index;
    auto idle   = ktl::make_ref<Thread>();
    assert(static_cast<bool>(idle), "sched: idle thread allocation failed");
    idle->set_name("idle0");
    idle->set_state(thread_state::RUNNING);
    // The boot stack keeps whatever tripwire the arch established (riscv trap_init; 0 on x86).
    idle->set_kstack_floor(g_kstack_floor);
    kernel_task()->add_thread(idle).expect("sched: task zero rejected the idle thread");
    auto& c          = cur_cpu();
    c.idle           = idle;
    c.current        = ktl::move(idle);
    g_last_switch_ts = kernel::arch::timestamp();
    g_stats.boot_ts  = g_last_switch_ts;
    g_started        = true;
    spawn("reaper", reaper_main, nullptr).expect("sched: reaper spawn failed");
    g_log.info("sched: online (core {0})", boot_core_index);
}

[[noreturn]] void idle_loop() {
    // Tick preemption (on_tick) switches threads in and out automatically, but idle itself is
    // never preempted into -- it has to cooperatively yield to hand the CPU to a newly-ready
    // thread. yield() is a fast no-op when the run queue is empty, so this still spends most of
    // its time halted in-between.
    while (true) {
        yield();
        kernel::arch::wait_for_interrupt();
    }
}

global_stats stats_snapshot() {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    global_stats s = g_stats;
    auto& c        = cur_cpu();
    s.runq_depth   = c.run_queue.size();
    s.sleepers     = g_sleepers.size();
    s.zombies      = g_zombies.size();
    // Charge the running thread's in-progress slice so idle/busy shares are current.
    uint64_t now   = kernel::arch::timestamp();
    c.current->stats().cpu_cycles += now - g_last_switch_ts;
    g_last_switch_ts = now;
    if (c.idle) { s.idle_cycles = c.idle->stats().cpu_cycles; }
    kernel::arch::restore_interrupts(flags);
    return s;
}

size_t trace_copy_newest(trace_record* out, size_t max) {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    size_t n       = g_trace.copy_newest(out, max);
    kernel::arch::restore_interrupts(flags);
    return n;
}

void trace_clear() {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    g_trace.clear();
    kernel::arch::restore_interrupts(flags);
}

void set_lifecycle_log(bool enabled) { g_lifecycle_log = enabled; }
bool lifecycle_log_enabled() { return g_lifecycle_log; }

}  // namespace kernel::sched

// Trampoline hooks. cur_cpu() is file-scope state above, so these live in this file.
extern "C" void sched_finish_switch() { kernel::sched::cur_cpu().previous.reset(); }
extern "C" [[noreturn]] void sched_thread_exit() { kernel::sched::exit_current(); }
