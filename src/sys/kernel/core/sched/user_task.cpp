#include <kernel/arch.h>
#include <kernel/assert.h>
#include <kernel/config.h>
#include <kernel/elf_loader.h>
#include <kernel/log.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/mm/vmo.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/user_task.h>
#include <std/new.h>

namespace kernel::sched {

namespace {

// The stack is the kernel's choice, not the image's: nothing in an ELF says where it goes, and
// first-fit virtual address search is still a VMM to-do. A segment that reached this far would
// collide here and be rejected by Region::map rather than silently overlapping.
constexpr uintptr_t USER_STACK_BASE = 0x800000;
constexpr size_t USER_STACK_PAGES   = 4;
constexpr uintptr_t USER_STACK_TOP  = USER_STACK_BASE + USER_STACK_PAGES * KERNEL_MINIMUM_PAGE_SIZE;

[[noreturn]] void user_thread_entry(void* entry) {
    // The temporary ref from current() must die before enter_user: the kernel stack is
    // abandoned on exit, so a ref still live here would never run its destructor and
    // would pin the Thread (and its owner Task) forever.
    uintptr_t kstack_top = 0;
    uintptr_t ipc_base   = 0;
    uintptr_t ipc_size   = 0;
    {
        auto self  = current();
        kstack_top = self->kstack_top();
        ipc_base   = self->ipc().user_base();
        ipc_size   = self->ipc().size_bytes();
    }
    kernel::arch::enter_user(reinterpret_cast<uintptr_t>(entry), USER_STACK_TOP, kstack_top, ipc_base, ipc_size);
}

// Escrow `object` into the kernel table and attach it to `message`, the same escrow a user-to-user
// handle transfer rides. False leaves the message unchanged; handles already attached stay owned
// by the message, whose destruction closes them.
bool escrow_into(kernel::obj::MessageBuffer& message, ktl::ref<kernel::obj::Object> object,
                 kernel::obj::Rights rights) {
    auto escrowed = kernel_task()->handles().insert(ktl::move(object), rights);
    if (escrowed.is_err()) { return false; }
    if (!message.attach_handle(escrowed.unwrap())) {
        (void)kernel_task()->handles().close(escrowed.unwrap());
        return false;
    }
    return true;
}

}  // namespace

ktl::result<ktl::ref<Task>> create_user_task(const char* name, const void* elf, size_t elf_size,
                                             ktl::span<const bootstrap_extra> extras) {
    using namespace kernel::mm;
    using namespace kernel::obj;

    if (extras.size() > MessageBuffer::MAX_HANDLES - 2) { return ktl::err(ktl::errc::out_of_range); }

    auto parsed = kernel::elf::parse_image(elf, elf_size);
    if (parsed.is_err()) {
        g_log.warn("task: '{0}' rejected: {1}", name, kernel::elf::to_string(parsed.unwrap_err()));
        return ktl::err(ktl::errc::invalid_operation);
    }
    auto img  = parsed.unwrap();

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

    auto loaded = kernel::elf::map_image(*aspace, elf, elf_size, img);
    if (loaded.is_err()) { return fail(loaded.unwrap_err()); }

    auto stack = create_anonymous_vmo(USER_STACK_PAGES);
    if (!stack) { return fail(ktl::errc::oom); }
    auto stack_mapped = aspace->root().map(USER_STACK_BASE, USER_STACK_PAGES * KERNEL_MINIMUM_PAGE_SIZE, stack, 0,
                                           vm_prot::USER | vm_prot::READ | vm_prot::WRITE);
    if (stack_mapped.is_err()) { return fail(stack_mapped.unwrap_err()); }

    ktl::ref<kernel::obj::Object> task_object = task;
    auto owner = kernel_task()->handles().insert(task_object, RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE);
    if (owner.is_err()) { return fail(owner.unwrap_err()); }
    task->set_owner_handle(owner.unwrap());

    // Everything below unwinds through here. Dropping the mailbox and clearing the table is what
    // releases the bootstrap escrow: the child endpoint dies with the table, the parent end with
    // the mailbox, and the pair's destruction closes the kernel-table entries the message carried
    // -- including the reference that would otherwise pin the task itself.
    auto fail_wired = [&](ktl::errc error) -> ktl::result<ktl::ref<Task>> {
        task->set_mailbox({});
        task->handles().clear();
        unregister_task(task->id());
        (void)kernel_task()->handles().close(task->owner_handle());
        return fail(error);
    };

    // The single insert into the fresh table IS the ABI: a first-generation slot 0 is what
    // abi::syscall::BOOTSTRAP_HANDLE promises the initial thread.
    auto pair_created = Channel::create();
    if (pair_created.is_err()) { return fail_wired(pair_created.unwrap_err()); }
    auto pair      = pair_created.unwrap();
    auto child_end = task->handles().insert(pair.second, Channel::DEFAULT_RIGHTS);
    if (child_end.is_err()) { return fail_wired(child_end.unwrap_err()); }
    assert(pack_handle(child_end.unwrap()) == ::abi::syscall::BOOTSTRAP_HANDLE, "bootstrap endpoint not at slot 0");
    task->set_mailbox(pair.first);

    register_task(task);
    task->set_state(task_state::RUNNING);

    auto created = thread_create_in(task, name, user_thread_entry, reinterpret_cast<void*>(img.entry));
    if (created.is_err()) { return fail_wired(created.unwrap_err()); }
    auto thread  = created.unwrap();

    // The bootstrap message is queued while the thread cannot yet run, so the payload can never
    // observe missing self-handles -- the ordering the old slots-0-and-1 scheme kept safe by
    // holding interrupts off across spawn. The handles ride as owned escrow entries; the cycle
    // (task table -> endpoint -> queued message -> task) is broken by teardown_user_task, which
    // clears the table and drops the mailbox explicitly rather than waiting on refcounts.
    auto message = MessageBuffer::create(0);
    bool endowed = message.is_ok();
    if (endowed) {
        auto boot = message.unwrap();
        endowed =
            escrow_into(boot, task, RIGHT_READ | RIGHT_WRITE) && escrow_into(boot, thread, RIGHT_READ | RIGHT_WAIT);
        for (size_t i = 0; endowed && i < extras.size(); i++) {
            endowed = escrow_into(boot, extras[i].object, extras[i].rights);
        }
        if (endowed) { endowed = task->mailbox()->write(ktl::move(boot)).is_ok(); }
    }
    if (!endowed) {
        thread_discard(thread);
        return fail_wired(ktl::errc::oom);
    }

    auto queued = thread_enqueue(thread);
    if (queued.is_err()) { return fail_wired(queued.unwrap_err()); }

    if (lifecycle_log_enabled()) { g_log.debug("task: created '{0}' id={1}", name, task->id()); }
    return ktl::result<ktl::ref<Task>>::ok(ktl::move(task));
}

void teardown_user_task(ktl::ref<Task> task) {
    task->handles().clear();
    // Dropping the parent's end after the table means both endpoints are now gone, which frees
    // the pair's state and closes any handles still escrowed on its queues -- an undrained
    // bootstrap message is what releases the task's self-reference here.
    task->set_mailbox({});
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

[[noreturn]] void terminate_current_user_task_from_fault(uint64_t cause, uint64_t detail, uintptr_t pc) {
    auto thread = current();
    auto task   = ktl::static_ref_cast<Task>(thread->owner());
    assert(task && task.get() != kernel_task().get(), "user fault has no user task");

    // Keep this a single parseable record: a fault path must report enough to correlate an
    // architecture-specific trap with the task and thread it terminated, without invoking the
    // crash dumper or touching faultable user memory.
    g_log.error("task_fault task={0} thread={1} cause={2} detail=0x{3:x} pc=0x{4:p} action=terminate", task->id(),
                thread->id(), cause, detail, pc);

    // exit_current() queues the thread and switches stacks. It may not run while fault_depth is
    // nonzero, so callers must fault_exit() before this handoff.
    kernel::synchronization::assert_blocking_allowed("user fault termination still in fault context");
    exit_current();
}

}  // namespace kernel::sched
