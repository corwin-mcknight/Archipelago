#include <kernel/panic.h>
#include <kernel/synchronization/execution_context.h>

namespace kernel::synchronization {

namespace {
execution_context g_contexts[CONFIG_MAX_CORES];
deferred_preempt_hook g_preempt_hook = nullptr;

bool deferred_preemption_eligible(const execution_context& context) {
    return context.preempt_depth == 0 && context.interrupt_depth == 0 && context.fault_depth == 0;
}

void service_deferred_preemption(execution_context& context) {
    if (!context.preempt_pending || !deferred_preemption_eligible(context) || g_preempt_hook == nullptr) { return; }
    context.preempt_pending = false;
    g_preempt_hook();
}
}  // namespace

void init_execution_context(size_t cpu_index) {
    if (cpu_index >= CONFIG_MAX_CORES) { panic("execution context: CPU index out of range"); }
    g_contexts[cpu_index]           = {};
    g_contexts[cpu_index].cpu_index = cpu_index;
}

execution_context& current_execution_context() {
#if defined(ARCH_X86_64) || defined(ARCH_RISCV64)
    size_t index = kernel::arch::current_core_index();
#else
    size_t index = 0;
#endif
    if (index >= CONFIG_MAX_CORES) { panic("execution context: current CPU index out of range"); }
    return g_contexts[index];
}

void set_current_thread_id(uint64_t thread_id) { current_execution_context().thread_id = thread_id; }
void set_deferred_preempt_hook(deferred_preempt_hook hook) { g_preempt_hook = hook; }

void preempt_disable() { ++current_execution_context().preempt_depth; }

void preempt_enable() {
    auto& context = current_execution_context();
    if (context.preempt_depth == 0) { panic("preempt_enable: unbalanced enable"); }
    --context.preempt_depth;
    service_deferred_preemption(context);
}

bool preemption_disabled() { return current_execution_context().preempt_depth != 0; }

bool blocking_allowed() {
    const auto& context = current_execution_context();
    return context.preempt_depth == 0 && context.interrupt_depth == 0 && context.fault_depth == 0;
}

void request_preemption() { current_execution_context().preempt_pending = true; }

void interrupt_enter() { ++current_execution_context().interrupt_depth; }
void interrupt_exit() {
    auto& context = current_execution_context();
    if (context.interrupt_depth == 0) { panic("interrupt_exit: unbalanced exit"); }
    --context.interrupt_depth;
    service_deferred_preemption(context);
}
void fault_enter() { ++current_execution_context().fault_depth; }
void fault_exit() {
    auto& context = current_execution_context();
    if (context.fault_depth == 0) { panic("fault_exit: unbalanced exit"); }
    --context.fault_depth;
    service_deferred_preemption(context);
}
void syscall_enter() { ++current_execution_context().syscall_depth; }
void syscall_exit() {
    auto& context = current_execution_context();
    if (context.syscall_depth == 0) { panic("syscall_exit: unbalanced exit"); }
    --context.syscall_depth;
    service_deferred_preemption(context);
}

void assert_thread_context(const char* operation) {
    const auto& context = current_execution_context();
    if (context.interrupt_depth != 0 || context.fault_depth != 0) { panic(operation); }
}
void assert_blocking_allowed(const char* operation) {
    if (!blocking_allowed()) { panic(operation); }
}
void assert_no_locks_held(const char* operation) {
#ifndef NDEBUG
    if (current_execution_context().held_count != 0) { panic(operation); }
#else
    (void)operation;
#endif
}

}  // namespace kernel::synchronization
