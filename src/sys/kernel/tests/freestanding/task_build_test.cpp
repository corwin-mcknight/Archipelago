#include <kernel/mm/physmap.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task_construction.h>
#include <kernel/sched/user_task.h>
#include <kernel/testing/spawn.h>
#include <kernel/testing/testing.h>

#include "../../syscalls/internal.h"

using namespace kernel::sched;
using namespace kernel::obj;
using namespace kernel::syscalls;
namespace mm = kernel::mm;

KTEST_MODULE("kernel/task_build");

KTEST_CASE(task_build_requires_authority_and_is_thread_private) {
    mm::vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_UNWRAP(buffer, ipc_buffer::create(space, 1, 0));
    auto task   = ktl::make_ref<Task>();
    auto thread = ktl::make_ref<Thread>(task);
    auto other  = ktl::make_ref<Thread>(task);
    KTEST_REQUIRE_TRUE(task && thread && other);
    thread->set_ipc(buffer);
    other->set_ipc(buffer);
    KTEST_UNWRAP(name, buffer.range(0, 5));
    name.write("child", 5);
    KTEST_UNWRAP(ordinary, task->handles().insert(task, RIGHT_WRITE));
    KTEST_EXPECT_TRUE(sys_task_build_create(*thread, pack_handle(ordinary), 0, 5) != 0);
    KTEST_EXPECT_TRUE(sys_thread_start(*thread, pack_handle(ordinary), 0, 0) != 0);
    auto factory = ktl::make_ref<TaskFactory>();
    KTEST_UNWRAP(denied, task->handles().insert(factory, 0));
    KTEST_EXPECT_TRUE(sys_task_build_create(*thread, pack_handle(denied), 0, 5) != 0);
    KTEST_UNWRAP(allowed, task->handles().insert(factory, RIGHT_WRITE));
    uint64_t cap         = pack_handle(allowed);
    auto local_threads   = ktl::make_ref<ThreadFactory>(task->id());
    auto foreign_threads = ktl::make_ref<ThreadFactory>(task->id() + 1);
    KTEST_UNWRAP(local, task->handles().insert(local_threads, RIGHT_WRITE));
    KTEST_UNWRAP(foreign, task->handles().insert(foreign_threads, RIGHT_WRITE));
    KTEST_UNWRAP(no_rights, task->handles().insert(local_threads, 0));
    KTEST_EXPECT_EQUAL(sys_task_build_create(*thread, pack_handle(local), 0, 5), errc_of(ktl::errc::wrong_type));
    KTEST_EXPECT_EQUAL(sys_thread_start(*thread, cap, 0, 0), errc_of(ktl::errc::wrong_type));
    KTEST_EXPECT_TRUE(sys_thread_start(*thread, pack_handle(foreign), 0, 0) != 0);
    KTEST_EXPECT_EQUAL(sys_thread_start(*thread, pack_handle(no_rights), 0, 0), errc_of(ktl::errc::rights_violation));
    KTEST_EXPECT_TRUE(local_threads->permits(task->id()));
    KTEST_EXPECT_FALSE(foreign_threads->permits(task->id()));
    KTEST_REQUIRE_EQUAL(sys_task_build_create(*thread, cap, 0, 5), uint64_t{0});
    KTEST_EXPECT_TRUE(sys_task_build_create(*thread, cap, 0, 5) != 0);
    KTEST_EXPECT_FALSE(static_cast<bool>(other->construction()));
    KTEST_EXPECT_TRUE(sys_task_build_start(*other, cap, 0x400000, 0x801000, 16) != 0);
    KTEST_EXPECT_TRUE(sys_task_build_start(*thread, cap, 0x400000, 0x801000, 16) != 0);
    KTEST_REQUIRE_EQUAL(sys_task_build_abort(*thread, cap), uint64_t{0});
    KTEST_EXPECT_FALSE(static_cast<bool>(thread->construction()));
    KTEST_REQUIRE_EQUAL(sys_task_build_create(*thread, cap, 0, 5), uint64_t{0});
    KTEST_REQUIRE_EQUAL(sys_task_build_abort(*thread, cap), uint64_t{0});
    task->handles().clear();  // ordinary self-handle would otherwise retain the synthetic task
}

KTEST_CASE(task_build_private_snapshot_executes) {
    constexpr size_t PAGE = KERNEL_MINIMUM_PAGE_SIZE;
    mm::vm_aspace space;
    KTEST_REQUIRE_TRUE(space.init());
    KTEST_UNWRAP(buffer, ipc_buffer::create(space, 1, 0));
    auto caller = ktl::make_ref<Task>();
    auto thread = ktl::make_ref<Thread>(caller);
    KTEST_REQUIRE_TRUE(caller && thread);
    thread->set_ipc(buffer);
    auto factory = ktl::make_ref<TaskFactory>();
    KTEST_UNWRAP(authority, caller->handles().insert(factory, RIGHT_WRITE));
    uint64_t cap = pack_handle(authority);
    KTEST_UNWRAP(name, buffer.range(0, 6));
    name.write("native", 6);
    KTEST_REQUIRE_EQUAL(sys_task_build_create(*thread, cap, 0, 6), uint64_t{0});

    auto source = mm::create_anonymous_vmo(1);
    KTEST_REQUIRE_TRUE(source);
    KTEST_REQUIRE_TRUE(source->commit(0, 1).is_ok());
    auto frame = source->resident_frame(0);
    KTEST_REQUIRE_TRUE(frame.has_value());
    // Native SYS_EXIT(42), deliberately no executable format or userspace runtime involved.
#if defined(ARCH_X86_64)
    constexpr uint8_t code[] = {0xb8, 0, 0, 0, 0, 0xbf, 42, 0, 0, 0, 0x0f, 0x05};
#elif defined(ARCH_RISCV64)
    constexpr uint32_t code[] = {0x00000893, 0x02a00513, 0x00000073};
#endif
    mm::copy_to_frame(*frame, 0, code, sizeof(code));
    KTEST_UNWRAP(vmo_handle, caller->handles().insert(source, RIGHT_READ | RIGHT_WRITE));
    KTEST_UNWRAP(descriptor_range, buffer.range(64, sizeof(abi_task_build_mapping)));
    abi_task_build_mapping descriptor{0x400000, PAGE, 0, ABI_VM_PROT_READ | ABI_VM_PROT_WRITE | ABI_VM_PROT_EXEC};
    descriptor_range.write(&descriptor, sizeof(descriptor));
    KTEST_EXPECT_TRUE(sys_task_build_map(*thread, cap, pack_handle(vmo_handle), 64) != 0);
    descriptor.prot = ABI_VM_PROT_READ | ABI_VM_PROT_EXEC;
    descriptor_range.write(&descriptor, sizeof(descriptor));
    KTEST_REQUIRE_EQUAL(sys_task_build_map(*thread, cap, pack_handle(vmo_handle), 64), uint64_t{0});
    // Destroy the source instructions after mapping. The child must still execute the snapshot.
    mm::zero_frame(*frame);
    descriptor.address = 0x800000;
    descriptor.prot    = ABI_VM_PROT_READ | ABI_VM_PROT_WRITE;
    descriptor_range.write(&descriptor, sizeof(descriptor));
    KTEST_REQUIRE_EQUAL(sys_task_build_map(*thread, cap, pack_handle(vmo_handle), 64), uint64_t{0});
    KTEST_EXPECT_TRUE(sys_task_build_start(*thread, cap, 0x800000, 0x801000, 128) != 0);
    KTEST_EXPECT_TRUE(sys_task_build_start(*thread, cap, 0x400000, 0x801001, 128) != 0);
    KTEST_REQUIRE_EQUAL(sys_task_build_start(*thread, cap, 0x400000, 0x801000, 128), uint64_t{0});
    KTEST_EXPECT_TRUE(sys_task_build_start(*thread, cap, 0x400000, 0x801000, 128) != 0);
    KTEST_UNWRAP(output, buffer.range(128, 16));
    uint64_t handles[2];
    output.read(handles, sizeof(handles));
    KTEST_UNWRAP(child, caller->handles().get<Task>(unpack_handle(handles[0])));
    KTEST_UNWRAP(mailbox, caller->handles().get<Channel>(unpack_handle(handles[1])));
    // Exit status is the snapshot execution proof.
    for (int i = 0; i < 2000 && child->state() != task_state::TERMINATED; ++i) { sleep_ticks(1); }
    KTEST_REQUIRE_TRUE(child->state() == task_state::TERMINATED);
    KTEST_EXPECT_EQUAL(child->exit_code(), (uint64_t{ABI_TASK_EXIT_EXITED} << 32) | 42);
    KTEST_EXPECT_FALSE(static_cast<bool>(thread->construction()));
    KTEST_EXPECT_TRUE((mailbox->signals() & Channel::SIGNAL_PEER_CLOSED) != 0);
    caller->handles().clear();
}

KTEST_CASE(task_build_thread_exit_releases_unstarted_construction) {
    ktl::atomic<bool> prepared{false};
    auto body = [&] {
        auto pending = ktl::make_ref<TaskConstruction>();
        if (pending && pending->init("abandoned", 9).is_ok()) {
            current()->set_construction(pending);
            prepared.store(true);
        }
    };
    KTEST_UNWRAP(worker, kernel::testing::spawn_fn("builder", body));
    for (int i = 0; i < 2000 && worker->state() != thread_state::DEAD; ++i) { sleep_ticks(1); }
    KTEST_REQUIRE_TRUE(prepared.load());
    // Removal from the task's thread list is after construction cleanup. Snapshot under its
    // mutex establishes ordering before inspecting the now-inert retained Thread reference.
    bool removed = false;
    for (int i = 0; i < 2000 && !removed; ++i) {
        ktl::vector<ktl::ref<Thread>> threads;
        KTEST_REQUIRE_TRUE(worker->owner()->snapshot_threads(threads));
        removed = true;
        for (const auto& t : threads) {
            if (t->id() == worker->id()) { removed = false; }
        }
        if (!removed) { sleep_ticks(1); }
    }
    KTEST_REQUIRE_TRUE(removed);
    KTEST_EXPECT_FALSE(static_cast<bool>(worker->construction()));
}
