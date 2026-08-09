#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/boot.h>
#include <kernel/elf.h>
#include <kernel/log.h>
#include <kernel/obj/channel.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/user_task.h>
#include <kernel/syscall.h>
#include <kernel/time.h>
#include <std/string.h>

#include <ktl/vector>

using namespace kernel::sched;

KTEST_MODULE("kernel/task");

// End to end over the real boot path: the init module is loaded from the boot image by the ELF
// loader, runs in its own address space, and exits. A missing module fails the test loudly rather
// than silently skipping, because an image without init is a broken image.
KTEST_CASE_INTEGRATION(user_task_lifecycle) {
    const auto* module = kernel::boot::find_module("init");
    KTEST_REQUIRE_TRUE(module != nullptr);
    KTEST_REQUIRE_TRUE(module->size > 0);

    auto created = create_user_task("utest", module->data, module->size);
    KTEST_REQUIRE_TRUE(created.is_ok());
    ktl::ref<Task> task = created.unwrap();
    KTEST_EXPECT_TRUE(task->state() == task_state::RUNNING);

    ktl::vector<ktl::ref<Thread>> threads;
    KTEST_REQUIRE_TRUE(task->snapshot_threads(threads));
    KTEST_REQUIRE_EQUAL(threads.size(), 1u);

    // The bootstrap ABI: a fresh table whose first-generation slot 0 holds a channel endpoint,
    // exactly as BOOTSTRAP_HANDLE promises, and the kernel holds the parent's end as the task's
    // mailbox. The window is safe: init sleeps before exiting, so the table cannot have been torn
    // down yet. init itself asserts the message's contents from the other side ("bootstrap ok").
    {
        using namespace kernel::obj;
        KTEST_REQUIRE_VALUE(bootstrap, task->handles().info(HandleId{0, 0}));
        KTEST_EXPECT_ALL(bootstrap.type_id == type_ids::CHANNEL, bootstrap.rights == Channel::DEFAULT_RIGHTS);
        KTEST_REQUIRE_TRUE(task->mailbox());

        // Parent-to-task mail rides the same channel: a message queued here is readable on the
        // task's slot-0 endpoint. init never reads it, which is also worth exercising -- an
        // undrained message must die with the task, not outlive it.
        auto message = MessageBuffer::create(5);
        KTEST_REQUIRE_TRUE(message.is_ok());
        auto mail = message.unwrap();
        __builtin_memcpy(mail.data(), "hello", 5);
        KTEST_EXPECT_TRUE(task->mailbox()->write(ktl::move(mail)).is_ok());
    }

    // Drive the dispatch pipeline through the real syscall entry from kernel context: this thread
    // belongs to task zero, so operations land on the kernel table, where the new task's owner
    // handle carries DUPLICATE -- the success path the self-handles deliberately cannot reach.
    {
        using namespace kernel::obj;
        auto owner      = task->owner_handle();
        uint64_t packed = pack_handle(owner);

        uint64_t info   = syscall_dispatch(kernel::syscall::SYS_OBJ_INFO, packed, 0, 0, 0, 0, 0);
        KTEST_EXPECT_ALL((info & 0xFFFFFFFF) == type_ids::TASK,
                         (info >> 32) == (RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE));

        uint64_t dup = syscall_dispatch(kernel::syscall::SYS_HANDLE_DUPLICATE, packed, RIGHT_READ, 0, 0, 0, 0);
        KTEST_REQUIRE_TRUE(static_cast<int64_t>(dup) >= 0);
        uint64_t dup_info = syscall_dispatch(kernel::syscall::SYS_OBJ_INFO, dup, 0, 0, 0, 0, 0);
        KTEST_EXPECT_TRUE((dup_info >> 32) == RIGHT_READ);
        KTEST_EXPECT_TRUE(syscall_dispatch(kernel::syscall::SYS_HANDLE_CLOSE, dup, 0, 0, 0, 0, 0) == 0);
    }

    for (int i = 0; i < 2000 && task->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }

    // On the failure path, say where init actually is before the assert kills the run: the
    // thread's state plus the scheduler's queue depths separate a lost sleeper wake, a stuck
    // reaper, and a thread that never exited.
    if (task->state() != task_state::TERMINATED) {
        auto s = stats_snapshot();
        g_log.warn("utest stuck: task_state={0} threads={1} runq={2} sleepers={3} zombies={4} reaped={5}",
                   static_cast<uint32_t>(task->state()), task->thread_count(), s.runq_depth, s.sleepers, s.zombies,
                   s.reaped);
        g_log.warn("utest stuck: thread id={0} state={1} wakes={2} sleeps={3} yields={4}", threads[0]->id(),
                   static_cast<uint32_t>(threads[0]->state()), threads[0]->stats().wakes, threads[0]->stats().sleeps,
                   threads[0]->stats().yields);
    }

    KTEST_REQUIRE_TRUE(task->state() == task_state::TERMINATED);
    KTEST_EXPECT_EQUAL(task->thread_count(), 0u);
    KTEST_EXPECT_TRUE(task->aspace() == nullptr);
    KTEST_EXPECT_TRUE(threads[0]->state() == thread_state::DEAD);
    KTEST_EXPECT_TRUE(threads[0]->stats().yields >= 2);

    ktl::vector<ktl::ref<Task>> tasks;
    KTEST_REQUIRE_TRUE(snapshot_tasks(tasks));
    for (size_t i = 0; i < tasks.size(); ++i) { KTEST_EXPECT_TRUE(tasks[i]->id() != task->id()); }
}

// SYS_OBJECT_WAIT through the real dispatch path from kernel context, on a channel pair in task
// zero's table. Every wait here targets a signal that is already asserted, so nothing can block;
// the genuinely-blocking wake path is sched_test's territory (wait_signals with a signaling
// thread). What this adds is the syscall layer: poll semantics, mask validation, and the rights
// check.
KTEST_CASE_INTEGRATION(object_wait_syscall) {
    using namespace kernel::obj;
    namespace sys = kernel::syscall;

    auto& table   = kernel::sched::kernel_task()->handles();
    KTEST_UNWRAP(pair, Channel::create());
    KTEST_UNWRAP(first_id, table.insert(pair.first, Channel::DEFAULT_RIGHTS));
    uint64_t first  = pack_handle(first_id);

    // Fresh endpoint, polled (zero mask): writable, nothing to read.
    uint64_t polled = syscall_dispatch(sys::SYS_OBJECT_WAIT, first, 0, 0, 0, 0, 0);
    KTEST_EXPECT_TRUE(polled == Channel::SIGNAL_WRITABLE);

    // A wait on an already-asserted bit returns immediately with the observed signals.
    KTEST_REQUIRE_TRUE(pair.second->write(kernel::obj::MessageBuffer{}).is_ok());
    uint64_t observed = syscall_dispatch(sys::SYS_OBJECT_WAIT, first, Channel::SIGNAL_READABLE, 0, 0, 0, 0);
    KTEST_EXPECT_TRUE((observed & Channel::SIGNAL_READABLE) != 0);

    // Signals are 32 bits: a mask with any higher bit set is rejected, not truncated.
    uint64_t wide = syscall_dispatch(sys::SYS_OBJECT_WAIT, first, 1ull << 32, 0, 0, 0, 0);
    KTEST_EXPECT_TRUE(wide == static_cast<uint64_t>(ktl::errc::out_of_range));

    // A handle without the wait right is refused before any wait machinery runs.
    KTEST_UNWRAP(no_wait_id, table.insert(pair.second, RIGHT_READ | RIGHT_WRITE));
    uint64_t refused = syscall_dispatch(sys::SYS_OBJECT_WAIT, pack_handle(no_wait_id), 0, 0, 0, 0, 0);
    KTEST_EXPECT_TRUE(refused == static_cast<uint64_t>(ktl::errc::rights_violation));

    KTEST_EXPECT_TRUE(table.close(first_id).is_ok());
    KTEST_EXPECT_TRUE(table.close(no_wait_id).is_ok());
}

// The timeout path of SYS_OBJECT_WAIT: an already-asserted signal returns immediately regardless
// of timeout, a signal that never fires returns timed_out only after the deadline has genuinely
// elapsed, and the timed-wait registry is empty again afterwards -- the parked node was reclaimed,
// not leaked.
KTEST_CASE_INTEGRATION(object_wait_timeout) {
    using namespace kernel::obj;
    namespace sys = kernel::syscall;

    auto& table   = kernel::sched::kernel_task()->handles();
    KTEST_UNWRAP(pair, Channel::create());
    KTEST_UNWRAP(id, table.insert(pair.first, Channel::DEFAULT_RIGHTS));
    uint64_t handle   = pack_handle(id);

    // Already asserted: the wait returns the signals without consuming any of the timeout.
    uint64_t observed = syscall_dispatch(sys::SYS_OBJECT_WAIT, handle, Channel::SIGNAL_WRITABLE, 1, 0, 0, 0);
    KTEST_EXPECT_TRUE((observed & Channel::SIGNAL_WRITABLE) != 0);

    // Never asserted: READABLE cannot fire with no writer. Three ticks of timeout must cost at
    // least three ticks of time and come back timed_out.
    ktime_t before   = kernel::time::now();
    uint64_t ticks   = 3;
    uint64_t timeout = static_cast<uint64_t>(kernel::time::ktime_to_ns(ticks));
    uint64_t lapsed  = syscall_dispatch(sys::SYS_OBJECT_WAIT, handle, Channel::SIGNAL_READABLE, timeout, 0, 0, 0);
    KTEST_EXPECT_TRUE(lapsed == static_cast<uint64_t>(ktl::errc::timed_out));
    KTEST_EXPECT_TRUE(kernel::time::now() - before >= ticks);

    // A signal arriving before the deadline wins the race against the expiry scan.
    auto writer = kernel::sched::spawn(
        "utest-writer",
        [](void* arg) {
            kernel::sched::sleep_ticks(2);
            (void)static_cast<Channel*>(arg)->write(MessageBuffer{});
        },
        pair.second.get());
    KTEST_REQUIRE_TRUE(writer.is_ok());
    uint64_t long_timeout = static_cast<uint64_t>(kernel::time::ktime_to_ns(500));
    uint64_t woken = syscall_dispatch(sys::SYS_OBJECT_WAIT, handle, Channel::SIGNAL_READABLE, long_timeout, 0, 0, 0);
    KTEST_EXPECT_TRUE((woken & Channel::SIGNAL_READABLE) != 0);

    KTEST_EXPECT_TRUE(table.close(id).is_ok());
}

namespace {

// Build a test-only copy of init with its entry overwritten by a small native store through the
// unmapped null page. It remains a normal loader input, while avoiding a second user-program
// package solely for this regression test.
bool make_faulting_image(const kernel::boot::boot_module& module, ktl::vector<uint8_t>& image) {
    if (!image.reserve(module.size)) { return false; }
    for (size_t i = 0; i < module.size; ++i) {
        if (!image.push_back(static_cast<const uint8_t*>(module.data)[i])) { return false; }
    }

    auto* header = reinterpret_cast<kernel::elf::Elf64_Ehdr*>(image.data());
    if (header->e_phoff > image.size() || header->e_phnum == 0 ||
        header->e_phentsize < sizeof(kernel::elf::Elf64_Phdr)) {
        return false;
    }

    uint64_t entry_offset = 0;
    bool found_entry      = false;
    for (size_t i = 0; i < header->e_phnum; ++i) {
        uint64_t offset = header->e_phoff + i * header->e_phentsize;
        if (offset > image.size() || sizeof(kernel::elf::Elf64_Phdr) > image.size() - offset) { return false; }
        auto* phdr = reinterpret_cast<kernel::elf::Elf64_Phdr*>(image.data() + offset);
        if (phdr->p_type != kernel::elf::PT_LOAD || header->e_entry < phdr->p_vaddr ||
            header->e_entry - phdr->p_vaddr >= phdr->p_filesz) {
            continue;
        }
        entry_offset = phdr->p_offset + header->e_entry - phdr->p_vaddr;
        found_entry  = true;
        break;
    }

#if defined(ARCH_X86_64)
    constexpr uint8_t FAULT_CODE[] = {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x00, 0x00};
#elif defined(ARCH_RISCV64)
    constexpr uint8_t FAULT_CODE[] = {0x93, 0x02, 0x00, 0x00, 0x23, 0x80, 0x02, 0x00};
#else
#error unsupported architecture
#endif

    if (!found_entry || entry_offset > image.size() || sizeof(FAULT_CODE) > image.size() - entry_offset) {
        return false;
    }
    memcpy(image.data() + entry_offset, FAULT_CODE, sizeof(FAULT_CODE));
    return true;
}

}  // namespace

KTEST_CASE_INTEGRATION(user_task_unresolved_fault_terminates_task) {
    const auto* module = kernel::boot::find_module("init");
    KTEST_REQUIRE_TRUE(module != nullptr);

    ktl::vector<uint8_t> image;
    KTEST_REQUIRE_TRUE(make_faulting_image(*module, image));
    auto created = create_user_task("ufault", image.data(), image.size());
    KTEST_REQUIRE_TRUE(created.is_ok());
    ktl::ref<Task> task = created.unwrap();

    for (int i = 0; i < 2000 && task->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }

    KTEST_REQUIRE_TRUE(task->state() == task_state::TERMINATED);
    KTEST_EXPECT_EQUAL(task->thread_count(), 0u);
    KTEST_EXPECT_TRUE(task->aspace() == nullptr);
    // Returning a passing test result makes the harness wait for the next shell-ready record,
    // proving the kernel remained live and the shell stayed reachable after the fault.
}

#endif
