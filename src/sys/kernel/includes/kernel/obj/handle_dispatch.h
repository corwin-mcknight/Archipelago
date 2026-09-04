#pragma once

#include <kernel/obj/handle_table.h>

namespace kernel::obj {

// The dispatch pipeline for handle syscalls: decode the packed handle, verify it in `table`
// (lookup with generation check, type check, rights check -- HandleTable::verify), and only then
// run the operation. Each operation is one row in a table declaring the type it expects and the
// rights it requires, so an operation cannot be reached without its checks and adding one cannot
// forget them.
//
// Returns the operation's result, or a negative ktl::errc as a uint64. Unknown numbers return
// invalid_operation. The table is passed in rather than looked up so the pipeline stays free of
// scheduler dependencies and hosted tests can drive it directly.
uint64_t dispatch_handle_op(HandleTable& table, uint64_t nr, uint64_t handle, uint64_t arg, uint64_t mode = 0);

}  // namespace kernel::obj
