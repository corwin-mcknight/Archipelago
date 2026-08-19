#include <abi/message.h>
#include <kernel/boot.h>
#include <kernel/elf.h>
#include <kernel/log.h>
#include <kernel/mm/vmo.h>
#include <kernel/obj/channel.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/user_task.h>
#include <kernel/syscall.h>
#include <kernel/testing/spawn.h>
#include <kernel/testing/testing.h>
#include <kernel/time.h>
#include <std/string.h>

#include <ktl/string_view>
#include <ktl/vector>

extern uintptr_t g_hhdm_offset;

using namespace kernel::sched;

KTEST_MODULE("kernel/task");

// The exit status selftest reports when no coordinator answered its connect: everything passed,
// echo roundtrip skipped. Mirrors STATUS_ECHO_SKIPPED in sys/selftest/main.cpp.
constexpr uint32_t SELFTEST_STATUS_ECHO_SKIPPED = 0x100;

// End to end over the real boot path: the selftest module is loaded from the boot image by the
// ELF loader, runs in its own address space, and exits. A missing module fails the test loudly
// rather than silently skipping, because an image without selftest is a broken image.
KTEST_CASE(user_task_lifecycle) {
    const auto* module = kernel::boot::find_module("selftest");
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
    // mailbox. The window is safe: selftest waits and sleeps before exiting, so the table cannot
    // have been torn down yet. selftest asserts the message's contents from the other side
    // ("bootstrap ok").
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
    // wait on it returns immediately), and status reports a clean run -- every section passed,
    // echo skipped, because no coordinator exists to answer the connect.
    {
        namespace sys = kernel::syscall;
        uint64_t sig  = syscall_dispatch(sys::SYS_OBJECT_WAIT, keeper, sys::TASK_SIGNAL_TERMINATED, 0, 0, 0, 0);
        KTEST_EXPECT_TRUE((sig & sys::TASK_SIGNAL_TERMINATED) != 0);
        uint64_t status = syscall_dispatch(sys::SYS_TASK_STATUS, keeper, 0, 0, 0, 0, 0);
        KTEST_EXPECT_ALL((status >> 32) == sys::TASK_EXIT_EXITED,
                         (status & 0xFFFFFFFF) == SELFTEST_STATUS_ECHO_SKIPPED);
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

    // selftest runs everything -- including connect("echo") and the roundtrip -- and exits.
    for (int i = 0; i < 4000 && selftest->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }
    KTEST_REQUIRE_TRUE(selftest->state() == task_state::TERMINATED);
    KTEST_EXPECT_ALL(selftest->exit_code() >> 32 == sys::TASK_EXIT_EXITED, (selftest->exit_code() & 0xFFFFFFFF) == 0);

    // Echo keeps serving until its parent dies. Kill the coordinator; echo observes the hangup
    // and exits of its own accord with a clean status.
    KTEST_EXPECT_TRUE(echo->state() == task_state::RUNNING);
    KTEST_REQUIRE_TRUE(task_kill(coordinator).is_ok());
    for (int i = 0; i < 2000 && echo->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }
    KTEST_REQUIRE_TRUE(coordinator->state() == task_state::TERMINATED);
    KTEST_REQUIRE_TRUE(echo->state() == task_state::TERMINATED);
    KTEST_EXPECT_TRUE(coordinator->exit_code() >> 32 == sys::TASK_EXIT_KILLED);
    KTEST_EXPECT_ALL(echo->exit_code() >> 32 == sys::TASK_EXIT_EXITED, (echo->exit_code() & 0xFFFFFFFF) == 0);
}

// Task kill against a genuinely blocked victim: echo spawned without a client parks its only
// thread in port_wait forever, so nothing but the kill can end it. The kill must find the thread
// parked on the port's wait queue, claim it, and force it out through the syscall boundary; the
// task then tears down completely and reports the killed cause. Timing-independent: a kill landing
// before echo reaches its wait still marks the thread, which then refuses to park.
KTEST_CASE(user_task_kill_blocked) {
    using namespace kernel::obj;
    namespace sys      = kernel::syscall;
    const auto* module = kernel::boot::find_module("echo");
    KTEST_REQUIRE_TRUE(module != nullptr);

    auto created = create_user_task("ukill", module->data, module->size);
    KTEST_REQUIRE_TRUE(created.is_ok());
    ktl::ref<Task> task = created.unwrap();
    uint64_t owner      = pack_handle(task->owner_handle());

    uint64_t keeper     = syscall_dispatch(sys::SYS_HANDLE_DUPLICATE, owner, RIGHT_READ | RIGHT_WAIT, 0, 0, 0, 0);
    KTEST_REQUIRE_TRUE(static_cast<int64_t>(keeper) >= 0);

    // Let echo reach its event loop so the interesting path -- claiming a parked thread -- is the
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

// Spawn from an image VMO, end to end: wrap selftest's module bytes exactly as endowment does,
// spawn through the buffer-free core (kernel test threads have no IPC buffer), and observe the
// whole parent contract through the two returned handles -- typed handles, the child running and
// then terminating cleanly, status readable, and the bootstrap channel's parent end reporting
// PEER_CLOSED once the child's table is gone.
KTEST_CASE(task_spawn_from_vmo) {
    using namespace kernel::obj;
    namespace sys      = kernel::syscall;
    const auto* module = kernel::boot::find_module("selftest");
    KTEST_REQUIRE_TRUE(module != nullptr);

    uintptr_t phys = reinterpret_cast<uintptr_t>(module->data) - g_hhdm_offset;
    size_t pages   = (module->size + KERNEL_MINIMUM_PAGE_SIZE - 1) / KERNEL_MINIMUM_PAGE_SIZE;
    auto image     = kernel::mm::create_wired_vmo(phys, pages);
    KTEST_REQUIRE_TRUE(image);
    image->set_name("selftest");

    auto spawned = task_spawn(*kernel_task(), image);
    KTEST_REQUIRE_TRUE(spawned.is_ok());
    uint64_t child        = pack_handle(spawned.unwrap().task);
    uint64_t mailbox      = pack_handle(spawned.unwrap().mailbox);

    uint64_t child_info   = syscall_dispatch(sys::SYS_OBJ_INFO, child, 0, 0, 0, 0, 0);
    uint64_t mailbox_info = syscall_dispatch(sys::SYS_OBJ_INFO, mailbox, 0, 0, 0, 0, 0);
    KTEST_EXPECT_ALL((child_info & 0xFFFFFFFF) == type_ids::TASK,
                     (child_info >> 32) == (RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_WAIT),
                     (mailbox_info & 0xFFFFFFFF) == type_ids::CHANNEL);

    // selftest runs its whole self-check and exits (echo skipped: no coordinator answers its
    // connect); a bounded wait keeps a broken spawn from wedging the run. 30 seconds is orders of
    // magnitude past a healthy pass.
    uint64_t sig =
        syscall_dispatch(sys::SYS_OBJECT_WAIT, child, sys::TASK_SIGNAL_TERMINATED, 30'000'000'000ull, 0, 0, 0);
    KTEST_REQUIRE_TRUE((sig & sys::TASK_SIGNAL_TERMINATED) != 0);
    uint64_t status = syscall_dispatch(sys::SYS_TASK_STATUS, child, 0, 0, 0, 0, 0);
    KTEST_EXPECT_ALL((status >> 32) == sys::TASK_EXIT_EXITED, (status & 0xFFFFFFFF) == SELFTEST_STATUS_ECHO_SKIPPED);

    // The child's death closed its end of the bootstrap channel: the parent end we hold reports
    // the orphan signal's mirror image.
    uint64_t mail_sig = syscall_dispatch(sys::SYS_OBJECT_WAIT, mailbox, 0, 0, 0, 0, 0);
    KTEST_EXPECT_TRUE((mail_sig & sys::CHANNEL_SIGNAL_PEER_CLOSED) != 0);

    KTEST_EXPECT_TRUE(syscall_dispatch(sys::SYS_HANDLE_CLOSE, child, 0, 0, 0, 0, 0) == 0);
    KTEST_EXPECT_TRUE(syscall_dispatch(sys::SYS_HANDLE_CLOSE, mailbox, 0, 0, 0, 0, 0) == 0);

    // Not an ELF: an anonymous VMO full of zeroes is refused, with nothing created.
    auto garbage = kernel::mm::create_anonymous_vmo(2);
    KTEST_REQUIRE_TRUE(garbage);
    KTEST_EXPECT_TRUE(task_spawn(*kernel_task(), garbage).is_err());
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
        KTEST_EXPECT_EQUAL(frame.unwrap(), reinterpret_cast<uintptr_t>(module.data) - g_hhdm_offset);
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

KTEST_CASE(user_task_unresolved_fault_terminates_task) {
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
    KTEST_EXPECT_TRUE(task->exit_code() >> 32 == kernel::syscall::TASK_EXIT_FAULTED);
    // Returning a passing test result makes the harness wait for the next shell-ready record,
    // proving the kernel remained live and the shell stayed reachable after the fault.
}
