#include <kernel/obj/channel.h>
#include <kernel/sched/task.h>

#include <ktl/string_view>

#include "kernel/testing/testing.h"
#include "shell_capture.h"

KTEST_MODULE("shell/handle");

// `handle all` walks every task and prints every handle. Running from kernel context, task zero is
// always present; a channel endpoint parked in its table must show up with its type name.
KTEST_CASE(shell_handle_all_lists_tables) {
    using namespace kernel::obj;
    auto& table = kernel::sched::kernel_task()->handles();
    KTEST_UNWRAP(pair, Channel::create());
    KTEST_UNWRAP(id, table.insert(pair.first, Channel::DEFAULT_RIGHTS));

    ktl::string_view out = run_shell("handle all");

    KTEST_EXPECT_TRUE(contains(out, "(kernel)"));
    KTEST_EXPECT_TRUE(contains(out, "channel"));
    KTEST_EXPECT_TRUE(table.close(id).is_ok());
}
