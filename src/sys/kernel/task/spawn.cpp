// src/sys/kernel/task/spawn.cpp
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

ktl::result<ktl::ref<Thread>> thread_create_in(ktl::ref<Task> task, const char* name, thread_entry_fn entry,
                                               void* arg) {
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
    // A fresh user thread is switched in -- and its FP state restored -- before it ever saves, so
    // the area must hold the entry-state image now: the zero-initialized area plus the arch's
    // nonzero entry fields. Kernel threads' areas are never touched, so seeding them is harmless.
    kernel::arch::fpu_init(thread->fpu_area());

    // A thread gets an IPC buffer exactly when its task has an address space to map one into.
    // Kernel threads have neither, and make no syscalls.
    if (task->aspace() != nullptr) {
        auto slot = task->acquire_ipc_slot();
        if (!slot.has_value()) {
            stack_pool_release(*phys);
            return ktl::err(ktl::errc::capacity_exhausted);
        }
        auto buffer = ipc_buffer::create(*task->aspace(), IPC_BUFFER_DEFAULT_PAGES, *slot);
        if (buffer.is_err()) {
            // Nothing was mapped, so the bit is all there is to give back.
            task->release_ipc_slot(*slot);
            stack_pool_release(*phys);
            return ktl::err(buffer.unwrap_err());
        }
        thread->set_ipc(buffer.unwrap());
    }

    auto added = task->add_thread(thread);
    if (added.is_err()) {
        // The buffer is mapped by now, so this unmaps as well as freeing the slot -- otherwise the
        // next thread handed this slot would fail to map over it.
        release_thread_ipc(*task, thread->ipc());
        stack_pool_release(*phys);
        return ktl::err(added.unwrap_err());
    }

    auto reserved = ensure_tick_capacity();
    if (reserved.is_err()) {
        task->remove_thread(thread->id());
        release_thread_ipc(*task, thread->ipc());
        stack_pool_release(*phys);
        return ktl::err(reserved.unwrap_err());
    }

    return ktl::result<ktl::ref<Thread>>::ok(thread);
}

void thread_discard(ktl::ref<Thread> thread) {
    auto task = ktl::static_ref_cast<Task>(thread->owner());
    note_thread_reaped();
    task->remove_thread(thread->id());
    release_thread_ipc(*task, thread->ipc());
    stack_pool_release(thread->kstack_phys());
}

ktl::result<void> thread_enqueue(ktl::ref<Thread> thread) {
    bool ok;
    {
        sched_guard guard(g_sched_lock);
        thread->set_ready_ts(kernel::arch::timestamp());
        g_stats.spawned += 1;
        auto& c = cur_cpu();
        trace_push(trace_kind::SPAWN, switch_reason::NONE, c.current ? c.current->id() : 0, thread->id());
        ok = push_runnable_locked(thread);
    }
    if (!ok) {
        thread_discard(thread);
        return ktl::err(ktl::errc::oom);
    }
    if (lifecycle_log_enabled()) { g_log.debug("sched: spawn '{0}' id={1}", thread->name(), thread->id()); }
    return ktl::result<void>::ok();
}

ktl::result<ktl::ref<Thread>> spawn(const char* name, thread_entry_fn entry, void* arg) {
    auto created = thread_create_in(kernel_task(), name, entry, arg);
    if (created.is_err()) { return ktl::err(created.unwrap_err()); }
    auto thread = created.unwrap();
    auto queued = thread_enqueue(thread);
    if (queued.is_err()) { return ktl::err(queued.unwrap_err()); }
    return ktl::result<ktl::ref<Thread>>::ok(ktl::move(thread));
}

}  // namespace kernel::sched
