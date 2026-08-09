#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <abi/syscall.h>
#include <kernel/obj/handle_dispatch.h>
#include <kernel/testing/test_objects.h>

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

#endif
