#include <stddef.h>
#include <stdint.h>

#include <ktl/result>

#include "kernel/mm/vm_aspace.h"
#include "kernel/sched/ipc_buffer.h"
#include "kernel/testing/testing.h"

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
    KTEST_UNWRAP(whole, buffer.range(0, buffer.size_bytes()));
    auto page        = whole.next();
    auto* kernel_ptr = page.data();
    KTEST_REQUIRE_EQUAL(page.size(), size_t{KERNEL_MINIMUM_PAGE_SIZE});
    for (size_t i = 0; i < KERNEL_MINIMUM_PAGE_SIZE; i++) { KTEST_REQUIRE_TRUE(kernel_ptr[i] == '\0'); }

    // What a syscall does: write through the kernel view, read it back at an offset.
    kernel_ptr[0]  = 'A';
    kernel_ptr[41] = 'Z';
    KTEST_UNWRAP(tail, buffer.range(41, buffer.size_bytes() - 41));
    auto at_41 = tail.next();
    KTEST_EXPECT_TRUE(at_41[0] == 'Z');
    KTEST_EXPECT_EQUAL(at_41.size(), size_t{KERNEL_MINIMUM_PAGE_SIZE - 41});
}

KTEST_CASE(ipc_buffer_bounds_reject_bad_ranges) {
    kernel::mm::vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());

    auto created = ipc_buffer::create(aspace, 1, 0);
    KTEST_REQUIRE_TRUE(created.is_ok());
    auto buffer         = created.unwrap();
    const uint64_t size = buffer.size_bytes();

    // Legitimate ranges, including both empty edges and the exact whole buffer.
    KTEST_EXPECT_ALL(buffer.range(0, size).is_ok(), buffer.range(0, 0).is_ok(), buffer.range(size, 0).is_ok(),
                     buffer.range(size - 1, 1).is_ok());

    // One byte too far, in each of the two ways to ask for it.
    KTEST_EXPECT_ALL(!buffer.range(0, size + 1).is_ok(), !buffer.range(size, 1).is_ok(),
                     !buffer.range(size + 1, 0).is_ok());

    // The case a naive `offset + length <= size` check would wave through: the sum wraps to a small
    // value and looks in-bounds. This is the reason the check is written as a subtraction.
    KTEST_EXPECT_FALSE(buffer.range(0xFFFFFFFFFFFFFF00ull, 0x200).is_ok());
    KTEST_EXPECT_FALSE(buffer.range(0x200, 0xFFFFFFFFFFFFFF00ull).is_ok());
    KTEST_EXPECT_FALSE(buffer.range(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull).is_ok());
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
    KTEST_EXPECT_TRUE(buffer.range(0, 3 * KERNEL_MINIMUM_PAGE_SIZE).is_ok());

    // Walking the whole buffer must cover every byte exactly once and stop at the end.
    KTEST_UNWRAP(whole, buffer.range(0, buffer.size_bytes()));
    size_t covered = 0;
    for (auto page = whole.next(); !page.empty(); page = whole.next()) {
        KTEST_REQUIRE_EQUAL(page.size(), size_t{KERNEL_MINIMUM_PAGE_SIZE});
        page[0] = static_cast<uint8_t>('a' + covered / KERNEL_MINIMUM_PAGE_SIZE);
        covered += page.size();
    }
    KTEST_EXPECT_EQUAL(covered, buffer.size_bytes());
    KTEST_EXPECT_TRUE(whole.next().empty());
    for (size_t page = 0; page < 3; page++) {
        KTEST_UNWRAP(marker, buffer.range(page * KERNEL_MINIMUM_PAGE_SIZE, 1));
        auto chunk = marker.next();
        KTEST_EXPECT_EQUAL(chunk[0], static_cast<uint8_t>('a' + page));
        KTEST_EXPECT_TRUE(marker.next().empty());
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
    KTEST_EXPECT_ALL(!none.valid(), none.range(0, 1).is_err(), none.range(0, 0).is_err());
}

KTEST_CASE(ipc_range_unaligned_chunks_and_copy) {
    kernel::mm::vm_aspace aspace;
    KTEST_REQUIRE_TRUE(aspace.init());
    KTEST_UNWRAP(buffer, ipc_buffer::create(aspace, 3, 0));
    constexpr size_t PAGE = KERNEL_MINIMUM_PAGE_SIZE;
    KTEST_UNWRAP(range, buffer.range(PAGE - 11, PAGE + 29));
    auto cursor          = range;
    const size_t sizes[] = {11, PAGE, 18};
    for (size_t size : sizes) {
        auto chunk = cursor.next();
        KTEST_REQUIRE_EQUAL(chunk.size(), size);
        for (auto& byte : chunk) { byte = 0xA5; }
    }
    KTEST_EXPECT_TRUE(cursor.next().empty());
    KTEST_EXPECT_EQUAL(cursor.size(), 0u);

    uint8_t source[64];
    uint8_t copied[64] = {};
    for (size_t i = 0; i < sizeof(source); i++) { source[i] = static_cast<uint8_t>(i); }
    range.write(source, sizeof(source));
    range.read(copied, sizeof(copied));
    for (size_t i = 0; i < sizeof(source); i++) { KTEST_EXPECT_EQUAL(copied[i], source[i]); }
    KTEST_EXPECT_EQUAL(range.size(), PAGE + 29);

    KTEST_UNWRAP(before, buffer.range(PAGE - 12, 1));
    KTEST_UNWRAP(after_copy, buffer.range(PAGE - 11 + sizeof(source), 1));
    KTEST_UNWRAP(after_range, buffer.range(2 * PAGE + 18, 1));
    KTEST_EXPECT_EQUAL(before.next()[0], uint8_t{0});
    KTEST_EXPECT_EQUAL(after_copy.next()[0], uint8_t{0xA5});
    KTEST_EXPECT_EQUAL(after_range.next()[0], uint8_t{0});

    KTEST_UNWRAP(empty, buffer.range(buffer.size_bytes(), 0));
    KTEST_EXPECT_TRUE(empty.next().empty());
    empty.read(nullptr, 0);
    empty.write(nullptr, 0);
}
