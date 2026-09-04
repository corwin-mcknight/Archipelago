#include <abi/syscall.h>
#include <kernel/obj/handle_dispatch.h>
#include <kernel/testing/test_objects.h>
#include <kernel/testing/testing.h>

using namespace kernel::testing;
using namespace kernel::obj;

namespace sys = abi::syscall;

KTEST_MODULE_WITH_INIT("obj/handle_dispatch", handle_dispatch_init);

static void handle_dispatch_init() { register_all_test_types(); }

namespace {

uint64_t pack(HandleId id) { return static_cast<uint64_t>(id.index) | (static_cast<uint64_t>(id.generation) << 32); }

uint64_t as_ret(ktl::errc error) { return static_cast<uint64_t>(error); }

}  // namespace

KTEST_CASE(handle_dispatch_info_packs_type_and_rights) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHT_READ | RIGHT_SIGNAL));
    uint64_t info = dispatch_handle_op(table, sys::SYS_OBJ_INFO, pack(id), 0);
    KTEST_EXPECT_ALL((info & 0xFFFFFFFF) == TEST_TYPE_A, (info >> 32) == (RIGHT_READ | RIGHT_SIGNAL));
}

KTEST_CASE(handle_dispatch_close_invalidates) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_EXPECT_TRUE(dispatch_handle_op(table, sys::SYS_HANDLE_CLOSE, pack(id), 0) == 0);
    KTEST_EXPECT_TRUE(table.count() == 0);
    // Both a re-close and any later operation on the stale handle die in the pipeline's lookup.
    KTEST_EXPECT_TRUE(dispatch_handle_op(table, sys::SYS_HANDLE_CLOSE, pack(id), 0) ==
                      as_ret(ktl::errc::handle_invalid));
    KTEST_EXPECT_TRUE(dispatch_handle_op(table, sys::SYS_OBJ_INFO, pack(id), 0) == as_ret(ktl::errc::handle_invalid));
}

// A recycled slot must not revalidate a stale handle: the generation moved when the slot was
// closed, and the pipeline checks it on every operation.
KTEST_CASE(handle_dispatch_stale_generation_rejected) {
    HandleTable table;
    KTEST_UNWRAP(old_id, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_EXPECT_TRUE(table.close(old_id).is_ok());
    KTEST_UNWRAP(new_id, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_REQUIRE_TRUE(new_id.index == old_id.index);
    KTEST_EXPECT_TRUE(dispatch_handle_op(table, sys::SYS_OBJ_INFO, pack(old_id), 0) ==
                      as_ret(ktl::errc::handle_invalid));
    KTEST_EXPECT_FALSE(dispatch_handle_op(table, sys::SYS_OBJ_INFO, pack(new_id), 0) ==
                       as_ret(ktl::errc::handle_invalid));
}

KTEST_CASE(handle_dispatch_duplicate_requires_the_right) {
    HandleTable table;
    KTEST_UNWRAP(bare, table.emplace<TestObjA>(RIGHT_READ));
    KTEST_EXPECT_TRUE(dispatch_handle_op(table, sys::SYS_HANDLE_DUPLICATE, pack(bare), RIGHTS_ALL) ==
                      as_ret(ktl::errc::rights_violation));

    KTEST_UNWRAP(dupable, table.emplace<TestObjA>(RIGHT_READ | RIGHT_DUPLICATE));
    uint64_t dup = dispatch_handle_op(table, sys::SYS_HANDLE_DUPLICATE, pack(dupable), RIGHT_READ);
    KTEST_REQUIRE_TRUE(static_cast<int64_t>(dup) >= 0);
    // The duplicate is a live handle carrying only the masked rights.
    uint64_t info = dispatch_handle_op(table, sys::SYS_OBJ_INFO, dup, 0);
    KTEST_EXPECT_ALL((info & 0xFFFFFFFF) == TEST_TYPE_A, (info >> 32) == RIGHT_READ, table.count() == 3);
}

KTEST_CASE(handle_dispatch_rejects_garbage) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL));
    (void)id;
    KTEST_EXPECT_TRUE(dispatch_handle_op(table, sys::SYS_OBJ_INFO, 0xDEADBEEFCAFEF00Dull, 0) ==
                      as_ret(ktl::errc::handle_invalid));
    KTEST_EXPECT_TRUE(dispatch_handle_op(table, 999, pack(id), 0) == as_ret(ktl::errc::invalid_operation));
}

// The ABI promises the first handle created in a fresh table is first-generation slot 0 -- that
// is what makes BOOTSTRAP_HANDLE a constant at all. Allocation stays in ascending slot order.
KTEST_CASE(handle_dispatch_fresh_table_allocates_slot_zero_first) {
    HandleTable table;
    KTEST_UNWRAP(first, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_UNWRAP(second, table.emplace<TestObjB>(RIGHTS_ALL));
    KTEST_EXPECT_ALL(pack(first) == sys::BOOTSTRAP_HANDLE, pack(second) == 1);
}

KTEST_CASE(handle_dispatch_restrict_checks_full_width) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHT_READ | RIGHT_WRITE));
    for (unsigned bit = 2; bit < 64; ++bit) {
        KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, pack(id), RIGHT_READ | (1ull << bit)),
                           as_ret(ktl::errc::rights_violation));
        KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_OBJ_INFO, pack(id), 0) >> 32,
                           uint64_t{RIGHT_READ | RIGHT_WRITE});
    }
    KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, pack(id), UINT64_MAX),
                       as_ret(ktl::errc::rights_violation));
    KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, pack(id), RIGHT_READ), 0ull);
    KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, pack(id), RIGHT_READ), 0ull);
    KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_OBJ_INFO, pack(id), 0) >> 32, uint64_t{RIGHT_READ});
    KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, pack(id), RIGHT_WRITE),
                       as_ret(ktl::errc::rights_violation));
    KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, pack(id), 0), 0ull);
    KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_OBJ_INFO, pack(id), 0) >> 32, 0ull);
    KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_HANDLE_CLOSE, pack(id), 0), 0ull);
    KTEST_UNWRAP(reused, table.emplace<TestObjA>(RIGHTS_ALL));
    KTEST_REQUIRE_TRUE(reused.index == id.index);
    // Invalid-handle errors take precedence even when the requested rights are malformed.
    KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, pack(id), UINT64_MAX),
                       as_ret(ktl::errc::handle_invalid));
    KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, UINT64_MAX, 0),
                       as_ret(ktl::errc::handle_invalid));
    KTEST_EXPECT_TRUE(table.verify(reused, RIGHTS_ALL).is_ok());
}

KTEST_CASE(handle_dispatch_remove_accepts_every_mask_bit) {
    HandleTable table;
    for (unsigned bit = 0; bit < 64; ++bit) {
        KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL));
        uint64_t mask = 1ull << bit;
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            KTEST_EXPECT_EQUAL(
                dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, pack(id), mask, sys::HANDLE_RESTRICT_REMOVE), 0ull);
            KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_OBJ_INFO, pack(id), 0) >> 32,
                               uint64_t{RIGHTS_ALL} & ~mask);
        }
        KTEST_EXPECT_EQUAL(
            dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, pack(id), UINT64_MAX, sys::HANDLE_RESTRICT_REMOVE),
            0ull);
        KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_OBJ_INFO, pack(id), 0) >> 32, 0ull);
        KTEST_EXPECT_EQUAL(
            dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, pack(id), 0, sys::HANDLE_RESTRICT_REMOVE), 0ull);
        KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_HANDLE_CLOSE, pack(id), 0), 0ull);
        KTEST_EXPECT_EQUAL(
            dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, pack(id), UINT64_MAX, sys::HANDLE_RESTRICT_REMOVE),
            as_ret(ktl::errc::handle_invalid));
    }
}

KTEST_CASE(handle_dispatch_restrict_rejects_unknown_modes_without_changes) {
    HandleTable table;
    KTEST_UNWRAP(id, table.emplace<TestObjA>(RIGHTS_ALL));
    for (unsigned bit = 1; bit < 64; ++bit) {
        for (uint64_t base = 0; base <= 1; ++base) {
            KTEST_EXPECT_EQUAL(
                dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, pack(id), RIGHT_READ, base | (1ull << bit)),
                as_ret(ktl::errc::invalid_operation));
            KTEST_EXPECT_TRUE(table.verify(id, RIGHTS_ALL).is_ok());
        }
    }
    KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, pack(id), 0, UINT64_MAX),
                       as_ret(ktl::errc::invalid_operation));
    KTEST_EXPECT_EQUAL(dispatch_handle_op(table, sys::SYS_HANDLE_RESTRICT, UINT64_MAX, 0, UINT64_MAX),
                       as_ret(ktl::errc::handle_invalid));
}
