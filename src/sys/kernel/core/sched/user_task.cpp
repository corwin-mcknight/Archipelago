#include <kernel/arch.h>
#include <kernel/config.h>
#include <kernel/log.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/mm/vmo.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/user_task.h>
#include <std/new.h>
#include <string.h>

extern uintptr_t g_hhdm_offset;
extern "C" const char user_payload_start[];
extern "C" const char user_payload_end[];

namespace kernel::sched {

namespace {

constexpr uintptr_t USER_CODE_VADDR = 0x400000;
constexpr uintptr_t USER_STACK_BASE = 0x800000;
constexpr size_t USER_STACK_PAGES   = 4;
constexpr uintptr_t USER_STACK_TOP  = USER_STACK_BASE + USER_STACK_PAGES * KERNEL_MINIMUM_PAGE_SIZE;

[[noreturn]] void user_thread_entry(void*) {
    // The temporary ref from current() must die before enter_user: the kernel stack is
    // abandoned on exit, so a ref still live here would never run its destructor and
    // would pin the Thread (and its owner Task) forever.
    uintptr_t kstack_top = current()->kstack_top();
    kernel::arch::enter_user(USER_CODE_VADDR, USER_STACK_TOP, kstack_top);
}

ktl::result<ktl::ref<kernel::mm::vmo>> build_code_vmo(size_t& pages_out) {
    size_t bytes = static_cast<size_t>(user_payload_end - user_payload_start);
    size_t pages = (bytes + KERNEL_MINIMUM_PAGE_SIZE - 1) / KERNEL_MINIMUM_PAGE_SIZE;
    auto code    = kernel::mm::create_anonymous_vmo(pages);
    if (!code) { return ktl::err(ktl::errc::oom); }
    auto committed = code->commit(0, pages);
    if (committed.is_err()) { return ktl::err(committed.unwrap_err()); }
    for (size_t page = 0; page < pages; ++page) {
        auto frame = code->resident_frame(page);
        if (!frame.has_value()) { return ktl::err(ktl::errc::oom); }
        size_t offset = page * KERNEL_MINIMUM_PAGE_SIZE;
        size_t chunk  = bytes - offset < KERNEL_MINIMUM_PAGE_SIZE ? bytes - offset : KERNEL_MINIMUM_PAGE_SIZE;
        memcpy(reinterpret_cast<void*>(frame.value() + g_hhdm_offset), user_payload_start + offset, chunk);
    }
    pages_out = pages;
    return ktl::result<ktl::ref<kernel::mm::vmo>>::ok(ktl::move(code));
}

}  // namespace

ktl::result<ktl::ref<Task>> create_user_task(const char* name) {
    using namespace kernel::mm;
    using namespace kernel::obj;

    auto task = ktl::make_ref<Task>();
    if (!task) { return ktl::err(ktl::errc::oom); }
    task->set_name(name);

    auto* aspace = new (std::nothrow) vm_aspace();
    if (aspace == nullptr || !aspace->init()) {
        delete aspace;
        return ktl::err(ktl::errc::oom);
    }
    task->set_aspace(aspace);

    auto fail = [&](ktl::errc error) -> ktl::result<ktl::ref<Task>> {
        task->set_aspace(nullptr);
        delete aspace;
        return ktl::err(error);
    };

    size_t code_pages = 0;
    auto code         = build_code_vmo(code_pages);
    if (code.is_err()) { return fail(code.unwrap_err()); }
    auto mapped = aspace->root().map(USER_CODE_VADDR, code_pages * KERNEL_MINIMUM_PAGE_SIZE, code.unwrap(), 0,
                                     vm_prot::USER | vm_prot::READ | vm_prot::EXECUTE);
    if (mapped.is_err()) { return fail(mapped.unwrap_err()); }

    auto stack = create_anonymous_vmo(USER_STACK_PAGES);
    if (!stack) { return fail(ktl::errc::oom); }
    auto stack_mapped = aspace->root().map(USER_STACK_BASE, USER_STACK_PAGES * KERNEL_MINIMUM_PAGE_SIZE, stack, 0,
                                           vm_prot::USER | vm_prot::READ | vm_prot::WRITE);
    if (stack_mapped.is_err()) { return fail(stack_mapped.unwrap_err()); }

    ktl::ref<kernel::obj::Object> task_object = task;
    auto owner = kernel_task()->handles().insert(task_object, RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE);
    if (owner.is_err()) { return fail(owner.unwrap_err()); }
    task->set_owner_handle(owner.unwrap());

    register_task(task);
    task->set_state(task_state::RUNNING);
    // Interrupts stay off from spawn through self-handle insertion so the payload cannot
    // run, exit, and be torn down before its handles exist -- a late insert into the
    // cleared table would recreate the Task->HandleTable->Task cycle teardown breaks.
    // ponytail: single-core interrupts-off; SMP needs a creation lock or a paused-spawn API.
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    auto thread    = spawn_into(task, name, user_thread_entry, nullptr);
    if (thread.is_err()) {
        kernel::arch::restore_interrupts(flags);
        unregister_task(task->id());
        (void)kernel_task()->handles().close(task->owner_handle());
        return fail(thread.unwrap_err());
    }

    ktl::ref<kernel::obj::Object> self_task_object   = task;
    ktl::ref<kernel::obj::Object> self_thread_object = thread.unwrap();
    auto self_task   = task->handles().insert(ktl::move(self_task_object), RIGHT_READ | RIGHT_WRITE);
    auto self_thread = task->handles().insert(ktl::move(self_thread_object), RIGHT_READ | RIGHT_WAIT);
    kernel::arch::restore_interrupts(flags);
    if (self_task.is_err() || self_thread.is_err()) {
        g_log.warn("task: '{0}' created without full self-handles", name);
    }

    if (lifecycle_log_enabled()) { g_log.debug("task: created '{0}' id={1}", name, task->id()); }
    return ktl::result<ktl::ref<Task>>::ok(ktl::move(task));
}

void teardown_user_task(ktl::ref<Task> task) {
    task->handles().clear();
    auto* aspace = task->aspace();
    if (aspace != nullptr) {
        if (kernel::mm::vm_aspace::active() == aspace) { kernel::mm::kernel_aspace().activate(); }
        task->set_aspace(nullptr);
        delete aspace;
    }
    unregister_task(task->id());
    auto owner = task->owner_handle();
    if (owner.is_valid()) { (void)kernel_task()->handles().close(owner); }
    // TERMINATED is the completion signal observers poll for, so it must be the last
    // teardown step; publishing it earlier exposes a half-torn-down task.
    task->set_state(task_state::TERMINATED);
    if (lifecycle_log_enabled()) { g_log.debug("task: torn down id={0}", task->id()); }
}

}  // namespace kernel::sched
