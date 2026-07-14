// src/sys/kernel/core/sched/spawn.cpp
#include <kernel/arch.h>
#include <kernel/assert.h>
#include <kernel/config.h>
#include <kernel/log.h>
#include <kernel/mm/pmm.h>
#include <kernel/sched/internal.h>
#include <kernel/sched/reaper.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>

extern uintptr_t g_hhdm_offset;

namespace kernel::sched {

ktl::result<ktl::ref<Thread>> spawn_into(ktl::ref<Task> task, const char* name, thread_entry_fn entry, void* arg) {
    constexpr size_t STACK_PAGES            = CONFIG_KERNEL_STACK_SIZE / KERNEL_MINIMUM_PAGE_SIZE;

    ktl::maybe<kernel::mm::vm_paddr_t> phys = stack_pool_acquire();
    if (!phys.has_value()) { phys = kernel::mm::g_page_frame_allocator.alloc_contiguous(STACK_PAGES); }
    if (!phys.has_value()) { return ktl::err(ktl::errc::oom); }

    uintptr_t virt_base = g_hhdm_offset + *phys;
    auto thread         = ktl::make_ref<Thread>(name, task, *phys, virt_base);
    if (!thread) {
        stack_pool_release(*phys);
        return ktl::err(ktl::errc::oom);
    }
    thread->set_saved_sp(kernel::arch::prepare_thread_stack(virt_base + CONFIG_KERNEL_STACK_SIZE, entry, arg));

    auto added = task->add_thread(thread);
    if (added.is_err()) {
        stack_pool_release(*phys);
        return ktl::err(added.unwrap_err());
    }

    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    thread->set_ready_ts(kernel::arch::timestamp());
    g_stats.spawned += 1;
    trace_push(trace_kind::SPAWN, switch_reason::NONE, cur_cpu().current ? cur_cpu().current->id() : 0, thread->id());
    bool ok = cur_cpu().run_queue.push_back(thread);
    kernel::arch::restore_interrupts(flags);
    if (!ok) {
        task->remove_thread(thread->id());
        stack_pool_release(*phys);
        return ktl::err(ktl::errc::oom);
    }
    if (lifecycle_log_enabled()) { g_log.debug("sched: spawn '{0}' id={1}", name, thread->id()); }
    return ktl::result<ktl::ref<Thread>>::ok(thread);
}

ktl::result<ktl::ref<Thread>> spawn(const char* name, thread_entry_fn entry, void* arg) {
    return spawn_into(kernel_task(), name, entry, arg);
}

}  // namespace kernel::sched
