#include <abi/message.h>
#include <abi/syscall.h>
#include <kernel/arch.h>
#include <kernel/assert.h>
#include <kernel/boot.h>
#include <kernel/config.h>
#include <kernel/log.h>
#include <kernel/mm/physmap.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/mm/vmo.h>
#include <kernel/sched/internal.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task_construction.h>
#include <kernel/sched/user_task.h>
#include <std/new.h>
#include <std/string.h>

namespace kernel::sched {

namespace {

[[noreturn]] void user_thread_entry(void* entry) {
    // The temporary ref from current() must die before enter_user: the kernel stack is
    // abandoned on exit, so a ref still live here would never run its destructor and
    // would pin the Thread (and its owner Task) forever.
    uintptr_t kstack_top = 0;
    uintptr_t ipc_base   = 0;
    uintptr_t ipc_size   = 0;
    uintptr_t user_sp    = 0;
    {
        auto self  = current();
        kstack_top = self->kstack_top();
        ipc_base   = self->ipc().user_base();
        ipc_size   = self->ipc().size_bytes();
        user_sp    = self->user_stack_pointer();
    }
    kernel::arch::enter_user(reinterpret_cast<uintptr_t>(entry), user_sp, kstack_top, ipc_base, ipc_size);
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

ktl::result<ktl::ref<Task>> start_prepared_user_task(const char* name, kernel::mm::vm_aspace* aspace, uintptr_t entry,
                                                     uintptr_t stack_pointer,
                                                     ktl::ref<kernel::obj::Channel>* parent_end_out,
                                                     ktl::ref<TaskFactory> factory) {
    using namespace kernel::obj;
    auto task = ktl::make_ref<Task>();
    if (!task) {
        delete aspace;
        return ktl::err(ktl::errc::oom);
    }
    task->set_owned_name(name);
    task->set_aspace(aspace);
    auto fail = [&](ktl::errc error) -> ktl::result<ktl::ref<Task>> {
        task->set_aspace(nullptr);
        delete aspace;
        return ktl::err(error);
    };

    ktl::ref<kernel::obj::Object> task_object = task;
    auto owner = kernel_task()->handles().insert(task_object, RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_WAIT);
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
    // The parent end stays local until the task is irreversibly launched; who keeps it is decided
    // at the end, once nothing can still unwind. fail_wired paths drop it with this frame, which
    // closes the pair exactly as before.

    register_task(task);
    task->set_state(task_state::RUNNING);

    auto created = thread_create_in(task, task->name(), user_thread_entry, reinterpret_cast<void*>(entry));
    if (created.is_err()) { return fail_wired(created.unwrap_err()); }
    auto thread = created.unwrap();
    thread->set_user_stack_pointer(stack_pointer);

    // The bootstrap message is queued while the thread cannot yet run, so the payload can never
    // observe missing self-handles -- the ordering the old slots-0-and-1 scheme kept safe by
    // holding interrupts off across spawn. The handles ride as owned escrow entries; the cycle
    // (task table -> endpoint -> queued message -> task) is broken by teardown_user_task, which
    // clears the table and drops the mailbox explicitly rather than waiting on refcounts.
    auto thread_factory = ktl::make_ref<ThreadFactory>(task->id());
    auto message        = MessageBuffer::create(0);
    bool endowed        = message.is_ok() && static_cast<bool>(thread_factory);
    if (endowed) {
        auto boot = message.unwrap();
        endowed   = escrow_into(boot, task, RIGHT_READ | RIGHT_WRITE) &&
                    escrow_into(boot, thread, RIGHT_READ | RIGHT_WAIT) &&
                    escrow_into(boot, thread_factory, RIGHT_WRITE | RIGHT_DUPLICATE);
        if (endowed && factory) { endowed = escrow_into(boot, factory, RIGHT_WRITE | RIGHT_DUPLICATE); }
        if (endowed) { endowed = pair.first->write(ktl::move(boot)).is_ok(); }
    }
    if (!endowed) {
        thread_discard(thread);
        return fail_wired(ktl::errc::oom);
    }

    // Ownership of the parent end settles before the thread can run: once the child is on the
    // queue it may run to completion (and teardown) at any preemption, and nothing may touch the
    // pair after that. An enqueue failure below unwinds either owner the same way -- the kernel
    // path through fail_wired's set_mailbox({}), the out-param path when the caller drops the ref
    // its failed spawn returned with.
    if (parent_end_out != nullptr) {
        *parent_end_out = ktl::move(pair.first);
    } else {
        task->set_mailbox(ktl::move(pair.first));
    }

    auto queued = thread_enqueue(thread);
    if (queued.is_err()) { return fail_wired(queued.unwrap_err()); }

    if (lifecycle_log_enabled()) { g_log.debug("task: created '{0}' id={1}", name, task->id()); }
    return ktl::result<ktl::ref<Task>>::ok(ktl::move(task));
}

ktl::result<kernel::obj::HandleId> start_user_thread(ktl::ref<Task> task, uintptr_t entry, uintptr_t stack) {
    using namespace kernel::obj;
    if (!task->aspace() || !valid_user_start(*task->aspace(), entry, stack)) {
        return ktl::err(ktl::errc::invalid_operation);
    }
    auto made = thread_create_in(task, task->name(), user_thread_entry, reinterpret_cast<void*>(entry));
    if (made.is_err()) { return ktl::err(made.unwrap_err()); }
    auto thread = made.unwrap();
    thread->set_user_stack_pointer(stack);
    auto handle = task->handles().insert(thread, RIGHT_READ | RIGHT_WAIT);
    if (handle.is_err()) {
        thread_discard(thread);
        return ktl::err(handle.unwrap_err());
    }
    auto queued = thread_enqueue(thread);
    if (queued.is_err()) {
        (void)task->handles().close(handle.unwrap());
        return ktl::err(queued.unwrap_err());
    }
    return ktl::result<HandleId>::ok(handle.unwrap());
}

ktl::result<void> endow_initrd(const ktl::ref<Task>& task) {
    using namespace kernel::obj;
    const auto* module    = kernel::boot::find_module("initrd");
    constexpr size_t PAGE = KERNEL_MINIMUM_PAGE_SIZE;
    if (!task || module == nullptr || module->data == nullptr || module->size == 0 ||
        module->size > SIZE_MAX - (PAGE - 1)) {
        return ktl::err(ktl::errc::invalid_operation);
    }
    uintptr_t phys = kernel::mm::direct_map_physical(module->data).value();
    size_t rounded = (module->size + PAGE - 1) & ~(PAGE - 1);
    if ((phys & (PAGE - 1)) != 0 || phys > UINTPTR_MAX - rounded) { return ktl::err(ktl::errc::invalid_operation); }
    auto image = kernel::mm::create_wired_vmo(phys, rounded / PAGE);
    if (!image) { return ktl::err(ktl::errc::oom); }
    image->set_name("initrd");

    auto created = MessageBuffer::create(sizeof(abi_message_header) + sizeof(abi_image_payload));
    if (created.is_err()) { return ktl::err(created.unwrap_err()); }
    auto mail = created.unwrap();
    abi_message_header header{::abi::message::COORD_OP_INITRD, 0, 0};
    abi_image_payload payload{module->size};
    __builtin_memcpy(mail.data(), &header, sizeof(header));
    __builtin_memcpy(mail.data() + sizeof(header), &payload, sizeof(payload));
    if (!escrow_into(mail, image, RIGHT_READ)) { return ktl::err(ktl::errc::oom); }

    // Bootstrap self-handles plus this one blob fit independently of archive member count.
    // Hold a local endpoint ref: the enqueued coordinator can die and clear Task::mailbox.
    auto mailbox = task->mailbox();
    if (!mailbox) { return ktl::err(ktl::errc::peer_closed); }
    return mailbox->write(ktl::move(mail));
}

ktl::result<void> task_kill(const ktl::ref<Task>& task) {
    if (!task || task.get() == kernel_task().get()) { return ktl::err(ktl::errc::invalid_operation); }
    if (task->state() == task_state::TERMINATED) { return ktl::result<void>::ok(); }

    task->close_thread_creation();
    // Snapshot outside the interrupts-off window: snapshot_threads takes the task mutex, which may
    // block. Creation was closed under the same mutex used by add_thread, so the snapshot cannot
    // miss a new thread. A thread that dies in between is skipped by the DEAD check below.
    ktl::vector<ktl::ref<Thread>> threads;
    if (!task->snapshot_threads(threads)) { return ktl::err(ktl::errc::oom); }

    // A task whose every thread is already dead is mid-reap: it terminated on its own, and marking
    // it killed now would misreport how it died.
    bool any_live = false;
    for (size_t i = 0; i < threads.size(); i++) { any_live = any_live || threads[i]->state() != thread_state::DEAD; }
    if (!any_live) { return ktl::result<void>::ok(); }

    task->record_exit(::abi::syscall::TASK_EXIT_KILLED, 0);

    // Under the scheduler lock thread states are stable and no thread can park somewhere new (block_if
    // and sleep_ticks take the same lock), so nothing marked here slips past the scan.
    {
        sched_guard guard(g_sched_lock);
        for (size_t i = 0; i < threads.size(); i++) {
            Thread* thread = threads[i].get();
            if (thread->state() == thread_state::DEAD) { continue; }
            thread->set_killed();
            if (thread->state() != thread_state::BLOCKED) { continue; }
            // Parked on a wait queue: claim resolves the race against ordinary wakers under the
            // queue's own lock, exactly as the timed-wait expiry scan does. Not parked and not
            // sleeping means the thread is between a wake and its next run; it will observe the mark.
            if (auto* queue = thread->parked_queue()) {
                ktl::ref<Thread> woken = queue->claim(thread->parked_node());
                if (woken) { make_ready_locked(ktl::move(woken)); }
            } else {
                (void)wake_sleeper(thread);
            }
        }
    }

    if (lifecycle_log_enabled()) { g_log.debug("task: killed id={0}", task->id()); }
    return ktl::result<void>::ok();
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
    task->signal_set(Task::SIGNAL_TERMINATED);
    if (lifecycle_log_enabled()) { g_log.debug("task: torn down id={0}", task->id()); }
}

[[noreturn]] void terminate_current_user_task_from_fault(uint64_t cause, uint64_t detail, uintptr_t pc) {
    auto thread = current();
    auto task   = thread->owner();
    assert(task && task.get() != kernel_task().get(), "user fault has no user task");

    // Keep this a single parseable record: a fault path must report enough to correlate an
    // architecture-specific trap with the task and thread it terminated, without invoking the
    // crash dumper or touching faultable user memory.
    g_log.error("task_fault task={0} thread={1} cause={2} detail=0x{3:x} pc=0x{4:p} action=terminate", task->id(),
                thread->id(), cause, detail, pc);
    task->record_exit(::abi::syscall::TASK_EXIT_FAULTED, static_cast<uint32_t>(cause));

    // exit_current() queues the thread and switches stacks. It may not run while fault_depth is
    // nonzero, so callers must fault_exit() before this handoff.
    kernel::synchronization::assert_blocking_allowed("user fault termination still in fault context");
    exit_current();
}

}  // namespace kernel::sched
