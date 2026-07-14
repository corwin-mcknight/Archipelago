#pragma once

#include <kernel/arch.h>
#include <kernel/config.h>
#include <kernel/panic.h>
#include <stddef.h>
#include <stdint.h>

namespace kernel::synchronization {

struct held_lock {
    const void* address = nullptr;
    uint32_t identity   = 0;
    const char* file    = nullptr;
    uint32_t line       = 0;
};

struct execution_context {
    uint32_t preempt_depth   = 0;
    uint32_t irq_depth       = 0;
    uint32_t interrupt_depth = 0;
    uint32_t fault_depth     = 0;
    uint32_t syscall_depth   = 0;
    bool preempt_pending     = false;
    size_t cpu_index         = 0;
    uint64_t thread_id       = 0;
#ifndef NDEBUG
    held_lock held[CONFIG_LOCKDEP_MAX_HELD] = {};
    size_t held_count                       = 0;
#endif
};

using deferred_preempt_hook = void (*)();

void init_execution_context(size_t cpu_index);
execution_context& current_execution_context();
void set_current_thread_id(uint64_t thread_id);
void set_deferred_preempt_hook(deferred_preempt_hook hook);

void preempt_disable();
void preempt_enable();
bool preemption_disabled();
bool blocking_allowed();
void request_preemption();

void interrupt_enter();
void interrupt_exit();
void fault_enter();
void fault_exit();
void syscall_enter();
void syscall_exit();

void assert_thread_context(const char* operation);
void assert_blocking_allowed(const char* operation);
void assert_no_locks_held(const char* operation);

class critical_section {
   public:
    critical_section() { preempt_disable(); }
    ~critical_section() { preempt_enable(); }
    critical_section(const critical_section&)            = delete;
    critical_section& operator=(const critical_section&) = delete;
};

class critical_irq_section {
   public:
    critical_irq_section() : m_flags(kernel::arch::save_and_disable_interrupts()) {
        auto& context = current_execution_context();
        ++context.irq_depth;
        preempt_disable();
    }
    ~critical_irq_section() {
        auto& context = current_execution_context();
        if (context.irq_depth == 0) { panic("critical_irq_section: unbalanced exit"); }
        --context.irq_depth;
        preempt_enable();
        kernel::arch::restore_interrupts(m_flags);
    }
    critical_irq_section(const critical_irq_section&)            = delete;
    critical_irq_section& operator=(const critical_irq_section&) = delete;

   private:
    uint64_t m_flags;
};

}  // namespace kernel::synchronization
