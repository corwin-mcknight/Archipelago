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

ktl::result<ktl::ref<Thread>> thread_create_in(ktl::ref<Task> task, const char* name, thread_entry_fn entry, void* arg,
                                               size_t ipc_pages) {
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

    // A thread gets an IPC buffer exactly when its task has an address space to map one into.
    // Kernel threads have neither, and make no syscalls.
    if (task->aspace() != nullptr) {
        auto slot = task->acquire_ipc_slot();
        if (!slot.has_value()) {
            stack_pool_release(*phys);
            return ktl::err(ktl::errc::capacity_exhausted);
        }
        auto buffer = ipc_buffer::create(*task->aspace(), ipc_pages, *slot);
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
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    thread->set_ready_ts(kernel::arch::timestamp());
    g_stats.spawned += 1;
    trace_push(trace_kind::SPAWN, switch_reason::NONE, cur_cpu().current ? cur_cpu().current->id() : 0, thread->id());
    bool ok = cur_cpu().run_queue.push_back(thread);
    kernel::arch::restore_interrupts(flags);
    if (!ok) {
        thread_discard(thread);
        return ktl::err(ktl::errc::oom);
    }
    if (lifecycle_log_enabled()) { g_log.debug("sched: spawn '{0}' id={1}", thread->name(), thread->id()); }
    return ktl::result<void>::ok();
}

ktl::result<ktl::ref<Thread>> spawn_into(ktl::ref<Task> task, const char* name, thread_entry_fn entry, void* arg,
                                         size_t ipc_pages) {
    auto created = thread_create_in(ktl::move(task), name, entry, arg, ipc_pages);
    if (created.is_err()) { return created; }
    auto thread = created.unwrap();
    auto queued = thread_enqueue(thread);
    if (queued.is_err()) { return ktl::err(queued.unwrap_err()); }
    return ktl::result<ktl::ref<Thread>>::ok(ktl::move(thread));
}

ktl::result<ktl::ref<Thread>> spawn(const char* name, thread_entry_fn entry, void* arg) {
    return spawn_into(kernel_task(), name, entry, arg);
}

}  // namespace kernel::sched
