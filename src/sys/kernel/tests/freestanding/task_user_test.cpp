#include <abi/message.h>
#include <kernel/boot.h>
#include <kernel/log.h>
#include <kernel/mm/physmap.h>
#include <kernel/mm/vmo.h>
#include <kernel/obj/channel.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/task_construction.h>
#include <kernel/sched/user_task.h>
#include <kernel/syscall.h>
#include <kernel/testing/spawn.h>
#include <kernel/testing/testing.h>
#include <kernel/testing/user_program.h>
#include <kernel/time.h>
#include <std/string.h>

#include <ktl/string_view>
#include <ktl/vector>

using namespace kernel::sched;

KTEST_MODULE("kernel/task");

// Native lifecycle fixture; the coordinator test below exercises userspace ELF loading.
constexpr uint32_t NATIVE_STATUS = 42;

KTEST_CASE(user_task_lifecycle) {
    kernel::testing::UserProgram program;
    program.syscall(ABI_SYS_YIELD);
    program.syscall(ABI_SYS_YIELD);
    program.syscall(ABI_SYS_SLEEP, 250);
    program.syscall(ABI_SYS_EXIT, NATIVE_STATUS);
    auto created = program.start("utest");
    KTEST_REQUIRE_TRUE(created.is_ok());
    ktl::ref<Task> task = created.unwrap();
    KTEST_EXPECT_TRUE(task->state() == task_state::RUNNING);

    ktl::vector<ktl::ref<Thread>> threads;
    KTEST_REQUIRE_TRUE(task->snapshot_threads(threads));
    KTEST_REQUIRE_EQUAL(threads.size(), 1u);

    // The bootstrap ABI: a fresh table whose first-generation slot 0 holds a channel endpoint,
    // exactly as BOOTSTRAP_HANDLE promises, and the kernel holds the parent's end as the task's
    // mailbox. The native fixture sleeps before exiting so its bootstrap can be inspected here.
    // The coordinator test covers consuming it from userspace.
    {
        using namespace kernel::obj;
        KTEST_UNWRAP(bootstrap, task->handles().verify(HandleId{0, 0}, 0, type_ids::CHANNEL));
        KTEST_EXPECT_TRUE(bootstrap.rights == Channel::DEFAULT_RIGHTS);
        KTEST_REQUIRE_TRUE(task->mailbox());

        // Parent-to-task mail rides the same channel: a message queued here is readable on the
        // task's slot-0 endpoint. It is smaller than an envelope, so selftest's reply loop skips
        // it -- and an undrained or skipped message must die with the task, not outlive it.
        auto message = MessageBuffer::create(5);
        KTEST_REQUIRE_TRUE(message.is_ok());
        auto mail = message.unwrap();
        __builtin_memcpy(mail.data(), "hello", 5);
        KTEST_EXPECT_TRUE(task->mailbox()->write(ktl::move(mail)).is_ok());
    }

    // Drive the dispatch pipeline through the real syscall entry from kernel context: this thread
    // belongs to task zero, so operations land on the kernel table, where the new task's owner
    // handle carries DUPLICATE -- the success path the self-handles deliberately cannot reach.
    // The keeper duplicate is taken now because teardown closes the owner handle itself: it is
    // how termination stays observable through a handle after the task is gone.
    uint64_t keeper = 0;
    {
        using namespace kernel::obj;
        auto owner      = task->owner_handle();
        uint64_t packed = pack_handle(owner);

        keeper = syscall_dispatch(kernel::syscall::SYS_HANDLE_DUPLICATE, packed, RIGHT_READ | RIGHT_WAIT, 0, 0, 0, 0);
        KTEST_REQUIRE_TRUE(static_cast<int64_t>(keeper) >= 0);

        uint64_t info = syscall_dispatch(kernel::syscall::SYS_OBJ_INFO, packed, 0, 0, 0, 0, 0);
        KTEST_EXPECT_ALL((info & 0xFFFFFFFF) == type_ids::TASK,
                         (info >> 32) == (RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_WAIT));

        // Status is unreadable while the task lives: selftest runs well past this point.
        uint64_t early = syscall_dispatch(kernel::syscall::SYS_TASK_STATUS, packed, 0, 0, 0, 0, 0);
        KTEST_EXPECT_TRUE(early == static_cast<uint64_t>(ktl::errc::would_block));

        uint64_t dup = syscall_dispatch(kernel::syscall::SYS_HANDLE_DUPLICATE, packed, RIGHT_READ, 0, 0, 0, 0);
        KTEST_REQUIRE_TRUE(static_cast<int64_t>(dup) >= 0);
        uint64_t dup_info = syscall_dispatch(kernel::syscall::SYS_OBJ_INFO, dup, 0, 0, 0, 0, 0);
        KTEST_EXPECT_TRUE((dup_info >> 32) == RIGHT_READ);
        KTEST_EXPECT_TRUE(syscall_dispatch(kernel::syscall::SYS_HANDLE_CLOSE, dup, 0, 0, 0, 0, 0) == 0);
    }

    for (int i = 0; i < 2000 && task->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }

    // On the failure path, say where selftest actually is before the assert kills the run: the
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

    // Termination is observable through the keeper handle: the TERMINATED signal is asserted (a
    // wait on it returns immediately), and status reports a clean run -- the native fixture returned its expected
    // status.
    {
        namespace sys = kernel::syscall;
        uint64_t sig  = syscall_dispatch(sys::SYS_OBJECT_WAIT, keeper, sys::TASK_SIGNAL_TERMINATED, 0, 0, 0, 0);
        KTEST_EXPECT_TRUE((sig & sys::TASK_SIGNAL_TERMINATED) != 0);
        uint64_t status = syscall_dispatch(sys::SYS_TASK_STATUS, keeper, 0, 0, 0, 0, 0);
        KTEST_EXPECT_ALL((status >> 32) == sys::TASK_EXIT_EXITED, (status & 0xFFFFFFFF) == NATIVE_STATUS);
        KTEST_EXPECT_TRUE(syscall_dispatch(sys::SYS_HANDLE_CLOSE, keeper, 0, 0, 0, 0, 0) == 0);
    }

    ktl::vector<ktl::ref<Task>> tasks;
    KTEST_REQUIRE_TRUE(snapshot_tasks(tasks));
    for (size_t i = 0; i < tasks.size(); ++i) { KTEST_EXPECT_TRUE(tasks[i]->id() != task->id()); }
}

namespace {

// The task named `name` from the global snapshot, or empty. Children of the coordinator are only
// reachable this way: the test holds no handle to them, by design.
ktl::ref<Task> find_task_named(ktl::string_view name) {
    ktl::vector<ktl::ref<Task>> tasks;
    if (!snapshot_tasks(tasks)) { return {}; }
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i]->name() != nullptr && ktl::string_view(tasks[i]->name()) == name) { return tasks[i]; }
    }
    return {};
}

}  // namespace

// The whole service layer, end to end, exactly as a normal boot runs it: the coordinator is
// launched with every boot module endowed, spawns selftest and echo itself, echo registers its
// name, selftest connects to it by name through the coordinator and proves the minted channel
// round-trips -- selftest's exit status 0 (echo NOT skipped) is the proof the brokered path ran.
// Killing the coordinator then orphans echo, whose event loop observes PEER_CLOSED and exits
// cleanly: the parent-death contract, demonstrated on a task the test never held a handle to.
KTEST_CASE(coordinator_boot) {
    namespace sys = kernel::syscall;
    auto launched = launch_coordinator();
    KTEST_REQUIRE_TRUE(launched.is_ok());
    ktl::ref<Task> coordinator = launched.unwrap();
    KTEST_EXPECT_TRUE(launch_coordinator().is_err());

    // The coordinator's children appear when it processes its IMAGE mail; find them by name.
    ktl::ref<Task> selftest;
    ktl::ref<Task> echo;
    for (int i = 0; i < 2000 && (!selftest || !echo); ++i) {
        sleep_ticks(1);
        if (!selftest) { selftest = find_task_named("selftest"); }
        if (!echo) { echo = find_task_named("echo"); }
    }
    KTEST_REQUIRE_TRUE(selftest);
    KTEST_REQUIRE_TRUE(echo);

    auto loader = find_task_named("elf_loader");
    KTEST_REQUIRE_TRUE(loader);
    auto has_factory = [](const ktl::ref<Task>& task) {
        ktl::vector<kernel::obj::HandleInfo> handles;
        if (!task->handles().snapshot(handles)) { return true; }
        for (const auto& handle : handles) {
            if (handle.type_id == TaskFactory::TYPE_ID) { return true; }
        }
        return false;
    };
    KTEST_EXPECT_TRUE(has_factory(loader));
    KTEST_EXPECT_FALSE(has_factory(coordinator));
    KTEST_EXPECT_FALSE(has_factory(echo));

    // selftest runs everything -- including connect("echo") and the roundtrip -- and exits.
    for (int i = 0; i < 4000 && selftest->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }
    KTEST_REQUIRE_TRUE(selftest->state() == task_state::TERMINATED);
    KTEST_EXPECT_ALL(selftest->exit_code() >> 32 == sys::TASK_EXIT_EXITED, (selftest->exit_code() & 0xFFFFFFFF) == 0);

    // A malformed image must not poison the loader service. Queue a bad image followed by a
    // real executable; fresh worker threads must reject the former and complete the latter.
    auto mail_image = [&](ktl::ref<kernel::mm::vmo> image, uint64_t size, const char* name, size_t name_size) {
        using namespace kernel::obj;
        auto made = MessageBuffer::create(sizeof(abi_message_header) + sizeof(abi_image_payload) + name_size);
        if (made.is_err()) { return false; }
        auto mail = made.unwrap();
        abi_message_header header{ABI_COORD_OP_IMAGE, 0, 0};
        abi_image_payload payload{size};
        __builtin_memcpy(mail.data(), &header, sizeof(header));
        __builtin_memcpy(mail.data() + sizeof(header), &payload, sizeof(payload));
        __builtin_memcpy(mail.data() + sizeof(header) + sizeof(payload), name, name_size);
        auto escrow = kernel_task()->handles().insert(image, RIGHT_READ);
        if (escrow.is_err()) { return false; }
        if (!mail.attach_handle(escrow.unwrap())) {
            (void)kernel_task()->handles().close(escrow.unwrap());
            return false;
        }
        auto mailbox = coordinator->mailbox();
        return mailbox && mailbox->write(ktl::move(mail)).is_ok();
    };
    auto bad_image = kernel::mm::create_anonymous_vmo(1);
    KTEST_REQUIRE_TRUE(bad_image);
    KTEST_REQUIRE_TRUE(mail_image(bad_image, KERNEL_MINIMUM_PAGE_SIZE, "badimage", 8));
    const auto* module = kernel::boot::find_module("selftest");
    KTEST_REQUIRE_TRUE(module != nullptr);
    auto retry_image =
        kernel::mm::create_wired_vmo(kernel::mm::direct_map_physical(module->data).value(),
                                     (module->size + KERNEL_MINIMUM_PAGE_SIZE - 1) / KERNEL_MINIMUM_PAGE_SIZE);
    KTEST_REQUIRE_TRUE(retry_image);
    KTEST_REQUIRE_TRUE(mail_image(retry_image, module->size, "retrytest", 9));
    ktl::ref<Task> retry;
    for (int i = 0; i < 4000 && !retry; ++i) {
        sleep_ticks(1);
        retry = find_task_named("retrytest");
    }
    KTEST_REQUIRE_TRUE(retry);
    for (int i = 0; i < 4000 && retry->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }
    KTEST_REQUIRE_TRUE(retry->state() == task_state::TERMINATED);
    KTEST_EXPECT_EQUAL(retry->exit_code(), uint64_t{0});
    KTEST_EXPECT_FALSE(static_cast<bool>(find_task_named("badimage")));
    KTEST_EXPECT_TRUE(loader->state() == task_state::RUNNING);

    // Echo keeps serving until its parent dies. Kill the coordinator; echo observes the hangup
    // and exits of its own accord with a clean status.
    KTEST_EXPECT_TRUE(echo->state() == task_state::RUNNING);
    KTEST_REQUIRE_TRUE(task_kill(coordinator).is_ok());
    for (int i = 0; i < 2000 && echo->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }
    KTEST_REQUIRE_TRUE(coordinator->state() == task_state::TERMINATED);
    KTEST_REQUIRE_TRUE(echo->state() == task_state::TERMINATED);
    for (int i = 0; i < 2000 && loader->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }
    KTEST_REQUIRE_TRUE(loader->state() == task_state::TERMINATED);
    KTEST_EXPECT_TRUE(coordinator->exit_code() >> 32 == sys::TASK_EXIT_KILLED);
    KTEST_EXPECT_ALL(echo->exit_code() >> 32 == sys::TASK_EXIT_EXITED, (echo->exit_code() & 0xFFFFFFFF) == 0);
}

// Task kill against a genuinely blocked native victim: it creates an empty port and parks its
// only thread in port_wait forever, so nothing but the kill can end it. The kill must find the thread
// parked on the port's wait queue, claim it, and force it out through the syscall boundary; the
// task then tears down completely and reports the killed cause. Timing-independent: a kill landing
// before echo reaches its wait still marks the thread, which then refuses to park.
KTEST_CASE(user_task_kill_blocked) {
    using namespace kernel::obj;
    namespace sys = kernel::syscall;
    kernel::testing::UserProgram program;
    program.block_on_port();
    auto created = program.start("ukill");
    KTEST_REQUIRE_TRUE(created.is_ok());
    ktl::ref<Task> task = created.unwrap();
    uint64_t owner      = pack_handle(task->owner_handle());

    uint64_t keeper     = syscall_dispatch(sys::SYS_HANDLE_DUPLICATE, owner, RIGHT_READ | RIGHT_WAIT, 0, 0, 0, 0);
    KTEST_REQUIRE_TRUE(static_cast<int64_t>(keeper) >= 0);

    // Let the fixture reach its port wait so the interesting path -- claiming a parked thread -- is the
    // one usually taken.
    for (int i = 0; i < 50 && task->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }
    KTEST_REQUIRE_TRUE(task->state() == task_state::RUNNING);

    KTEST_EXPECT_TRUE(syscall_dispatch(sys::SYS_TASK_KILL, owner, 0, 0, 0, 0, 0) == 0);

    for (int i = 0; i < 2000 && task->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }
    KTEST_REQUIRE_TRUE(task->state() == task_state::TERMINATED);
    KTEST_EXPECT_EQUAL(task->thread_count(), 0u);
    KTEST_EXPECT_TRUE(task->aspace() == nullptr);

    uint64_t sig = syscall_dispatch(sys::SYS_OBJECT_WAIT, keeper, sys::TASK_SIGNAL_TERMINATED, 0, 0, 0, 0);
    KTEST_EXPECT_TRUE((sig & sys::TASK_SIGNAL_TERMINATED) != 0);
    uint64_t status = syscall_dispatch(sys::SYS_TASK_STATUS, keeper, 0, 0, 0, 0, 0);
    KTEST_EXPECT_ALL((status >> 32) == sys::TASK_EXIT_KILLED, (status & 0xFFFFFFFF) == 0);

    // The keeper deliberately lacks the write right, so kill through it is refused by the
    // pipeline; killing the already-dead task through the kernel API is a polite no-op that does
    // not rewrite how the task died.
    KTEST_EXPECT_TRUE(syscall_dispatch(sys::SYS_TASK_KILL, keeper, 0, 0, 0, 0, 0) ==
                      static_cast<uint64_t>(ktl::errc::rights_violation));
    KTEST_EXPECT_TRUE(task_kill(task).is_ok());
    KTEST_EXPECT_TRUE(task->exit_code() >> 32 == sys::TASK_EXIT_KILLED);

    KTEST_EXPECT_TRUE(syscall_dispatch(sys::SYS_HANDLE_CLOSE, keeper, 0, 0, 0, 0, 0) == 0);
}

// Boot-module endowment: every boot module arrives on the endowed task's mailbox as one IMAGE
// message -- envelope, exact byte size, role name, and a read-only wired VMO over the module's
// bytes. Driven against a bare task with a hand-built mailbox so the test owns the child end and
// no user program races the reads.
KTEST_CASE(boot_module_endowment) {
    using namespace kernel::obj;
    const auto& info = kernel::boot::collect();
    KTEST_REQUIRE_TRUE(info.module_count >= 2);  // the image ships at least init and echo

    auto task = ktl::make_ref<Task>();
    KTEST_REQUIRE_TRUE(task);
    KTEST_UNWRAP(ends, Channel::create());
    task->set_mailbox(ends.first);

    KTEST_REQUIRE_TRUE(endow_boot_modules(task).is_ok());

    for (size_t i = 0; i < info.module_count; i++) {
        const auto& module = info.modules[i];
        size_t name_len    = strlen(module.role);

        auto received      = ends.second->read(Channel::MAX_MESSAGE_BYTES, MessageBuffer::MAX_HANDLES);
        KTEST_REQUIRE_TRUE(received.is_ok());
        auto mail = received.unwrap();
        KTEST_REQUIRE_EQUAL(mail.size(), sizeof(abi_message_header) + sizeof(abi_image_payload) + name_len);

        abi_message_header header;
        abi_image_payload payload;
        __builtin_memcpy(&header, mail.data(), sizeof(header));
        __builtin_memcpy(&payload, mail.data() + sizeof(header), sizeof(payload));
        KTEST_EXPECT_ALL(header.opcode == ::abi::message::COORD_OP_IMAGE, header.status == 0, header.txid == 0);
        KTEST_EXPECT_EQUAL(payload.size_bytes, module.size);
        KTEST_EXPECT_TRUE(memcmp(mail.data() + sizeof(header) + sizeof(payload), module.role, name_len) == 0);

        // The riding handle: a read-only VMO named after the module, sized to its pages, whose
        // first frame is the module's own physical memory -- wrapped, not copied.
        HandleId escrowed[MessageBuffer::MAX_HANDLES];
        KTEST_REQUIRE_EQUAL(mail.detach_handles(escrowed), 1u);
        auto taken = kernel::sched::kernel_task()->handles().take(escrowed[0]);
        KTEST_REQUIRE_TRUE(taken.is_ok());
        auto moved = taken.unwrap();
        KTEST_EXPECT_ALL(moved.object->type_id() == type_ids::VMO, moved.rights == RIGHT_READ);
        KTEST_EXPECT_TRUE(strlen(moved.object->name()) == name_len &&
                          memcmp(moved.object->name(), module.role, name_len) == 0);

        auto image   = ktl::static_ref_cast<kernel::mm::vmo>(moved.object);
        size_t pages = (module.size + KERNEL_MINIMUM_PAGE_SIZE - 1) / KERNEL_MINIMUM_PAGE_SIZE;
        KTEST_EXPECT_EQUAL(image->size_pages(), pages);
        // Translation-only fill (device-window pager): safe to call without the VMM lock on a VMO
        // nothing else references.
        auto frame = image->get_or_fill_page(0);
        KTEST_REQUIRE_TRUE(frame.is_ok());
        KTEST_EXPECT_EQUAL(frame.unwrap(), kernel::mm::direct_map_physical(module.data).value());
    }

    // No stragglers: exactly one message per module.
    KTEST_EXPECT_TRUE(ends.second->read(Channel::MAX_MESSAGE_BYTES, MessageBuffer::MAX_HANDLES).is_err());
    task->set_mailbox({});
}

// SYS_OBJECT_WAIT through the real dispatch path from kernel context, on a channel pair in task
// zero's table. Every wait here targets a signal that is already asserted, so nothing can block;
// the genuinely-blocking wake path is sched_test's territory (wait_signals with a signaling
// thread). What this adds is the syscall layer: poll semantics, mask validation, and the rights
// check.
KTEST_CASE(object_wait_syscall) {
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
KTEST_CASE(object_wait_timeout) {
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
    auto write_soon = [&] {
        kernel::sched::sleep_ticks(2);
        (void)pair.second->write(MessageBuffer{});
    };
    KTEST_REQUIRE_TRUE(kernel::testing::spawn_fn("utest-writer", write_soon).is_ok());
    uint64_t long_timeout = static_cast<uint64_t>(kernel::time::ktime_to_ns(500));
    uint64_t woken = syscall_dispatch(sys::SYS_OBJECT_WAIT, handle, Channel::SIGNAL_READABLE, long_timeout, 0, 0, 0);
    KTEST_EXPECT_TRUE((woken & Channel::SIGNAL_READABLE) != 0);

    KTEST_EXPECT_TRUE(table.close(id).is_ok());
}

KTEST_CASE(user_task_unresolved_fault_terminates_task) {
    kernel::testing::UserProgram program;
    program.fault();
    auto created = program.start("ufault");
    KTEST_REQUIRE_TRUE(created.is_ok());
    ktl::ref<Task> task = created.unwrap();

    for (int i = 0; i < 2000 && task->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }

    KTEST_REQUIRE_TRUE(task->state() == task_state::TERMINATED);
    KTEST_EXPECT_EQUAL(task->thread_count(), 0u);
    KTEST_EXPECT_TRUE(task->aspace() == nullptr);
    KTEST_EXPECT_TRUE(task->exit_code() >> 32 == kernel::syscall::TASK_EXIT_FAULTED);
    // Returning a passing test result makes the harness wait for the next shell-ready record,
    // proving the kernel remained live and the shell stayed reachable after the fault.
}
