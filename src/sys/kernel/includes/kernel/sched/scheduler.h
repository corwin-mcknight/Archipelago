// src/sys/kernel/includes/kernel/sched/scheduler.h
#pragma once

#include <kernel/sched/thread.h>

#include <ktl/ref>
#include <ktl/result>

namespace kernel::sched {

// Bring the scheduler online on the boot core: the calling context becomes the idle thread and
// the reaper is spawned. Requires obj_init() and a ticking timer.
void init(uint32_t boot_core_index);
bool started();

ktl::ref<Thread> current();

// Create a kernel thread under task zero and make it runnable. name must be a string literal.
ktl::result<ktl::ref<Thread>> spawn(const char* name, thread_entry_fn entry, void* arg);

void yield();
// Block the current thread until at least `ticks` kernel ticks have elapsed. The idle thread
// must never sleep.
void sleep_ticks(uint64_t ticks);
// Timer-tick hook: slice accounting and preemption. Interrupt context only.
void on_tick();
[[noreturn]] void exit_current();
[[noreturn]] void idle_loop();

// wait_queue/scheduler internals. schedule_out() requires interrupts disabled and the current
// thread already parked (BLOCKED/DEAD, or re-queued by the caller).
void schedule_out();
void make_ready(ktl::ref<Thread> thread);

}  // namespace kernel::sched

// Called by the arch entry trampolines.
extern "C" void sched_finish_switch();
extern "C" [[noreturn]] void sched_thread_exit();
