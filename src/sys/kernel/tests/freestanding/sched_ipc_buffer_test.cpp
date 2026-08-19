#include <stddef.h>
#include <stdint.h>

#include <ktl/result>

#include "kernel/mm/vm_aspace.h"
#include "kernel/sched/ipc_buffer.h"
#include "kernel/testing/testing.h"

// The IPC buffer is the only memory a syscall reads from its caller, so its bounds check is the
// whole of the kernel's input validation for buffered syscalls. These drive it against a scratch
// address space rather than a live thread: the interesting behaviour is in the range arithmetic and
// the page walk, neither of which needs a scheduler.

KTEST_MODULE("sched/ipc_buffer");

namespace { using namespace kernel::sched; }  // namespace

KTEST_CASE(ipc_buffer_maps_and_reads_back) {
    kernel::mm::vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());

    auto created = ipc_buffer::create(aspace, 1, 0);
    KTEST_REQUIRE_TRUE(created.is_ok());
    auto buffer = created.unwrap();

    KTEST_EXPECT_TRUE(buffer.valid());
    KTEST_EXPECT_EQUAL(buffer.size_bytes(), size_t{KERNEL_MINIMUM_PAGE_SIZE});
    KTEST_EXPECT_EQUAL(buffer.user_base(), IPC_BUFFER_REGION_BASE);

    // Anonymous VMOs zero-fill, so a fresh buffer must not hand the thread anyone else's bytes.
    size_t run       = 0;
    auto* kernel_ptr = reinterpret_cast<volatile char*>(buffer.kernel_at(0, run));
    KTEST_REQUIRE_EQUAL(run, size_t{KERNEL_MINIMUM_PAGE_SIZE});
    for (size_t i = 0; i < KERNEL_MINIMUM_PAGE_SIZE; i++) { KTEST_REQUIRE_TRUE(kernel_ptr[i] == '\0'); }

    // What a syscall does: write through the kernel view, read it back at an offset.
    kernel_ptr[0]   = 'A';
    kernel_ptr[41]  = 'Z';
    size_t tail_run = 0;
    auto* at_41     = reinterpret_cast<volatile char*>(buffer.kernel_at(41, tail_run));
    KTEST_EXPECT_TRUE(at_41[0] == 'Z');
    KTEST_EXPECT_EQUAL(tail_run, size_t{KERNEL_MINIMUM_PAGE_SIZE - 41});
}

KTEST_CASE(ipc_buffer_bounds_reject_bad_ranges) {
    kernel::mm::vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());

    auto created = ipc_buffer::create(aspace, 1, 0);
    KTEST_REQUIRE_TRUE(created.is_ok());
    auto buffer         = created.unwrap();
    const uint64_t size = buffer.size_bytes();

    // Legitimate ranges, including both empty edges and the exact whole buffer.
    KTEST_EXPECT_ALL(buffer.contains(0, size), buffer.contains(0, 0), buffer.contains(size, 0),
                     buffer.contains(size - 1, 1));

    // One byte too far, in each of the two ways to ask for it.
    KTEST_EXPECT_ALL(!buffer.contains(0, size + 1), !buffer.contains(size, 1), !buffer.contains(size + 1, 0));

    // The case a naive `offset + length <= size` check would wave through: the sum wraps to a small
    // value and looks in-bounds. This is the reason the check is written as a subtraction.
    KTEST_EXPECT_FALSE(buffer.contains(0xFFFFFFFFFFFFFF00ull, 0x200));
    KTEST_EXPECT_FALSE(buffer.contains(0x200, 0xFFFFFFFFFFFFFF00ull));
    KTEST_EXPECT_FALSE(buffer.contains(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull));
}

KTEST_CASE(ipc_buffer_multi_page_and_slots) {
    kernel::mm::vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());

    // A multi-page buffer's frames are not physically contiguous, so a range crossing a page
    // boundary is several runs -- the loop the write syscall relies on.
    auto created = ipc_buffer::create(aspace, 3, 1);
    KTEST_REQUIRE_TRUE(created.is_ok());
    auto buffer = created.unwrap();

    KTEST_EXPECT_EQUAL(buffer.size_bytes(), size_t{3 * KERNEL_MINIMUM_PAGE_SIZE});
    KTEST_EXPECT_EQUAL(buffer.user_base(), IPC_BUFFER_REGION_BASE + IPC_BUFFER_SLOT_BYTES);
    KTEST_EXPECT_TRUE(buffer.contains(0, 3 * KERNEL_MINIMUM_PAGE_SIZE));

    // Walking the whole buffer must cover every byte exactly once and stop at the end.
    uint64_t covered = 0;
    while (covered < buffer.size_bytes()) {
        size_t run = 0;
        auto* p    = reinterpret_cast<volatile char*>(buffer.kernel_at(covered, run));
        KTEST_REQUIRE_TRUE(p != nullptr);
        KTEST_REQUIRE_TRUE(run > 0 && run <= KERNEL_MINIMUM_PAGE_SIZE);
        p[0] = static_cast<char>('a' + covered / KERNEL_MINIMUM_PAGE_SIZE);
        covered += run;
    }
    KTEST_EXPECT_EQUAL(covered, buffer.size_bytes());

    // Each page got its own marker, proving the walk crossed real page boundaries.
    for (size_t page = 0; page < 3; page++) {
        size_t run = 0;
        auto* p    = reinterpret_cast<volatile char*>(buffer.kernel_at(page * KERNEL_MINIMUM_PAGE_SIZE, run));
        KTEST_EXPECT_TRUE(p[0] == static_cast<char>('a' + page));
    }
}

// A slot is only reusable once its address range is free again. release_thread_ipc() unmaps before
// returning the slot to the task's bitmap for exactly this reason: a task that spawns and reaps
// workers hands the same slot out repeatedly, and without the unmap the second thread to hold it
// would fail to map over the first one's buffer.
KTEST_CASE(ipc_buffer_slot_is_reusable_only_after_unmap) {
    kernel::mm::vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());

    auto first = ipc_buffer::create(aspace, 1, 0);
    KTEST_REQUIRE_TRUE(first.is_ok());
    auto buffer    = first.unwrap();

    // Still mapped: taking the slot again must be refused rather than aliasing the live buffer.
    auto collision = ipc_buffer::create(aspace, 1, 0);
    KTEST_EXPECT_TRUE(collision.is_err());

    KTEST_REQUIRE_TRUE(aspace.root().unmap(buffer.user_base(), buffer.size_bytes()).is_ok());

    auto reused = ipc_buffer::create(aspace, 1, 0);
    KTEST_EXPECT_TRUE(reused.is_ok());
}

KTEST_CASE(ipc_buffer_rejects_oversized_requests) {
    kernel::mm::vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());

    // Wired memory a task can demand of the kernel is capped; a request past the cap must fail
    // rather than pin whatever it asked for.
    auto too_big = ipc_buffer::create(aspace, IPC_BUFFER_MAX_PAGES + 1, 0);
    KTEST_EXPECT_TRUE(too_big.is_err());

    auto empty = ipc_buffer::create(aspace, 0, 0);
    KTEST_EXPECT_TRUE(empty.is_err());

    auto bad_slot = ipc_buffer::create(aspace, 1, IPC_BUFFER_MAX_SLOTS);
    KTEST_EXPECT_TRUE(bad_slot.is_err());

    // A default-constructed buffer belongs to a thread that never got one (a kernel thread); every
    // range must be refused rather than reading frame zero.
    ipc_buffer none;
    KTEST_EXPECT_ALL(!none.valid(), !none.contains(0, 1));
}
