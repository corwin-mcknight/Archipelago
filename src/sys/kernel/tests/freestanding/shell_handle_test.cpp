#include <stddef.h>

#include "kernel/config.h"
#include "kernel/shell/shell.h"
#include "kernel/testing/testing.h"

#if CONFIG_KERNEL_SHELL

#include <kernel/obj/channel.h>
#include <kernel/sched/task.h>

#include <ktl/string_view>

namespace {

char g_capture[4096];
size_t g_capture_len = 0;

void capture_sink(char c, void*) {
    if (g_capture_len < sizeof(g_capture) - 1) { g_capture[g_capture_len++] = c; }
}

bool captured_contains(ktl::string_view needle) {
    ktl::string_view haystack(g_capture, g_capture_len);
    if (needle.size() > haystack.size()) { return false; }
    for (size_t i = 0; i + needle.size() <= haystack.size(); i++) {
        if (haystack.substr(i, needle.size()) == needle) { return true; }
    }
    return false;
}

}  // namespace

KTEST_MODULE("shell/handle");

// `handle all` walks every task and prints every handle. Running from kernel context, task zero is
// always present; a channel endpoint parked in its table must show up with its type name.
KTEST_CASE_INTEGRATION(shell_handle_all_lists_tables) {
    using namespace kernel::obj;
    auto& table = kernel::sched::kernel_task()->handles();
    KTEST_UNWRAP(pair, Channel::create());
    KTEST_UNWRAP(id, table.insert(pair.first, Channel::DEFAULT_RIGHTS));

    kernel::shell::ShellOutput out;
    out.set_sink(capture_sink, nullptr);
    g_capture_len = 0;
    kernel::shell::run_line("handle all", out);

    KTEST_EXPECT_TRUE(captured_contains("(kernel)"));
    KTEST_EXPECT_TRUE(captured_contains("channel"));
    KTEST_EXPECT_TRUE(table.close(id).is_ok());
}

#endif  // CONFIG_KERNEL_SHELL
