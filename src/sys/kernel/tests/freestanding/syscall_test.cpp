#include <kernel/mm/vm_aspace.h>
#include <kernel/obj/event.h>
#include <kernel/obj/socket.h>
#include <kernel/sched/scheduler.h>
#include <kernel/syscall.h>
#include <kernel/testing/spawn.h>
#include <kernel/testing/testing.h>

#include "../../syscalls/internal.h"

using namespace kernel::obj;
using namespace kernel::sched;
using namespace kernel::syscalls;

KTEST_MODULE("kernel/syscalls");

KTEST_CASE(syscall_pair_install_rolls_back) {
    kernel::mm::vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());
    KTEST_UNWRAP(buffer, ipc_buffer::create(aspace, 2, 0));
    KTEST_UNWRAP(output, buffer.range(KERNEL_MINIMUM_PAGE_SIZE - 8, 16));
    HandleTable table;
    auto first  = ktl::make_ref<Event>();
    auto second = ktl::make_ref<Event>();
    KTEST_REQUIRE_TRUE(first && second);
    uint64_t marker[2] = {UINT64_MAX, UINT64_MAX};
    output.write(marker, sizeof(marker));

    KTEST_EXPECT_EQUAL(install_pair(table, first, {}, RIGHT_READ, output), errc_of(ktl::errc::null_argument));
    KTEST_EXPECT_EQUAL(table.count(), 0u);
    KTEST_EXPECT_EQUAL(first.ref_count(), 1u);
    uint64_t handles[2] = {};
    output.read(handles, sizeof(handles));
    KTEST_EXPECT_ALL(handles[0] == UINT64_MAX, handles[1] == UINT64_MAX);
    KTEST_EXPECT_EQUAL(install_pair(table, {}, second, RIGHT_READ, output), errc_of(ktl::errc::null_argument));
    KTEST_EXPECT_EQUAL(table.count(), 0u);

    KTEST_REQUIRE_EQUAL(install_pair(table, first, second, RIGHT_READ, output), uint64_t{0});
    output.read(handles, sizeof(handles));
    KTEST_UNWRAP(a, table.get<Event>(unpack_handle(handles[0]), RIGHT_READ));
    KTEST_UNWRAP(b, table.get<Event>(unpack_handle(handles[1]), RIGHT_READ));
    KTEST_EXPECT_ALL(a == first, b == second, table.count() == 2);
}

KTEST_CASE(syscall_socket_partial_page_transfers) {
    kernel::mm::vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());
    KTEST_UNWRAP(buffer, ipc_buffer::create(aspace, 3, 0));
    auto task = ktl::make_ref<Task>();
    KTEST_REQUIRE_TRUE(static_cast<bool>(task));
    auto thread = ktl::make_ref<Thread>(task);
    KTEST_REQUIRE_TRUE(static_cast<bool>(thread));
    thread->set_ipc(buffer);
    constexpr size_t PAGE = KERNEL_MINIMUM_PAGE_SIZE;

    KTEST_REQUIRE_EQUAL(sys_socket_create(*thread, PAGE - 8), uint64_t{0});
    KTEST_UNWRAP(pair, buffer.range(PAGE - 8, 16));
    uint64_t handles[2];
    pair.read(handles, sizeof(handles));
    const size_t offsets[] = {0, PAGE - 16};
    for (size_t offset : offsets) {
        KTEST_UNWRAP(source, buffer.range(offset, PAGE + 32));
        for (auto chunk = source.next(); !chunk.empty(); chunk = source.next()) {
            for (auto& byte : chunk) { byte = 0x5A; }
        }
        // Aligned transfers hit an error after a full page; unaligned transfers end on a short chunk.
        KTEST_EXPECT_EQUAL(sys_socket_write(*thread, handles[0], offset, PAGE + 32), uint64_t{Socket::BUFFER_BYTES});
        KTEST_EXPECT_EQUAL(sys_socket_write(*thread, handles[0], 0, 1), errc_of(ktl::errc::capacity_exhausted));
        KTEST_UNWRAP(cleared, buffer.range(offset, PAGE + 32));
        for (auto chunk = cleared.next(); !chunk.empty(); chunk = cleared.next()) {
            for (auto& byte : chunk) { byte = 0; }
        }
        KTEST_EXPECT_EQUAL(sys_socket_read(*thread, handles[1], offset, PAGE + 32), uint64_t{Socket::BUFFER_BYTES});
        KTEST_EXPECT_EQUAL(sys_socket_read(*thread, handles[1], 0, 1), errc_of(ktl::errc::would_block));
        KTEST_UNWRAP(received, buffer.range(offset, Socket::BUFFER_BYTES));
        for (auto chunk = received.next(); !chunk.empty(); chunk = received.next()) {
            for (auto byte : chunk) { KTEST_REQUIRE_EQUAL(byte, uint8_t{0x5A}); }
        }
        KTEST_UNWRAP(tail, buffer.range(offset + Socket::BUFFER_BYTES, 32));
        for (auto byte : tail.next()) { KTEST_REQUIRE_EQUAL(byte, uint8_t{0}); }
    }
    KTEST_EXPECT_EQUAL(sys_socket_write(*thread, handles[0], buffer.size_bytes(), 0), uint64_t{0});
    KTEST_EXPECT_EQUAL(sys_socket_read(*thread, handles[1], UINT64_MAX, 1), errc_of(ktl::errc::out_of_range));
}

KTEST_CASE(syscall_unknown_preserves_exit_boundary) {
    KTEST_EXPECT_EQUAL(syscall_dispatch(UINT64_MAX, 0, 0, 0, 0, 0, 0), static_cast<uint64_t>(-1));
    ktl::atomic<bool> returned{false};
    auto body = [&] {
        {
            auto self = current();
            self->set_killed();
        }
        (void)syscall_dispatch(UINT64_MAX, 0, 0, 0, 0, 0, 0);
        returned.store(true);
    };
    KTEST_UNWRAP(thread, kernel::testing::spawn_fn("killed-syscall", body));
    KTEST_YIELD_UNTIL((thread->signals() & Thread::SIGNAL_TERMINATED) != 0);
    KTEST_EXPECT_FALSE(returned.load());
}

KTEST_CASE(syscall_task_handlers_validate_type_and_rights) {
    HandleTable table;
    KTEST_UNWRAP(event, table.emplace<Event>(RIGHT_READ));
    KTEST_EXPECT_EQUAL(sys_task_kill(table, pack_handle(event)), errc_of(ktl::errc::wrong_type));
    KTEST_EXPECT_EQUAL(sys_task_status(table, pack_handle(event)), errc_of(ktl::errc::wrong_type));
    KTEST_EXPECT_EQUAL(sys_task_kill(table, UINT64_MAX), errc_of(ktl::errc::handle_invalid));
    KTEST_EXPECT_EQUAL(sys_task_status(table, UINT64_MAX), errc_of(ktl::errc::handle_invalid));
    KTEST_UNWRAP(id, table.emplace<Task>(RIGHT_READ));
    KTEST_EXPECT_EQUAL(sys_task_kill(table, pack_handle(id)), errc_of(ktl::errc::rights_violation));
    KTEST_EXPECT_EQUAL(sys_task_status(table, pack_handle(id)), errc_of(ktl::errc::would_block));
    KTEST_UNWRAP(task, table.get<Task>(id));
    task->record_exit(abi::syscall::TASK_EXIT_EXITED, 42);
    task->set_state(task_state::TERMINATED);
    KTEST_EXPECT_EQUAL(sys_task_status(table, pack_handle(id)), task->exit_code());
    KTEST_REQUIRE_TRUE(table.remove_rights(id, RIGHT_READ).is_ok());
    KTEST_EXPECT_EQUAL(sys_task_status(table, pack_handle(id)), errc_of(ktl::errc::rights_violation));
}
