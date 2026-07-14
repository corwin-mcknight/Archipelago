#include <kernel/config.h>

#if CONFIG_KERNEL_SHELL && CONFIG_KERNEL_TESTING

#include <kernel/arch.h>
#include <kernel/crash.h>
#include <kernel/panic.h>
#include <kernel/shell/shell.h>
#include <kernel/testing/testing.h>
#include <kernel/time.h>

#include <ktl/algorithm>
#include <ktl/fixed_string>
#include <ktl/fmt>
#include <ktl/maybe>
#include <ktl/string_view>

extern "C" kernel::testing::ktest __start__ktests[], __stop__ktests[];

namespace {

constexpr size_t kHarnessBufferSize    = 512;

kernel::testing::ktest* g_current_test = nullptr;
bool g_current_test_failed             = false;
bool g_failure_reason_recorded         = false;
ktl::fixed_string<256> g_failure_reason{};

kernel::testing::ktest* tests_begin() { return __start__ktests; }
kernel::testing::ktest* tests_end() { return __stop__ktests; }

ktl::maybe<kernel::testing::ktest&> find_test(ktl::string_view name) {
    return ktl::find_if(tests_begin(), tests_end(), [&](const kernel::testing::ktest& t) { return name == t.name; });
}

template <typename... Args>
void emit_harness_event(kernel::shell::ShellOutput& output, const char* fmt, const Args&... args) {
    ktl::fixed_string<kHarnessBufferSize> buffer;
    ktl::format::format_to_buffer_raw(buffer.m_buffer, sizeof(buffer.m_buffer), fmt, args...);
    // Atomic: an interrupt-context log line splicing into a protocol event corrupts the
    // harness JSON stream (seen live with the unhandled-vector warning during interrupt tests).
    output.write_atomic(buffer.c_str());
}

void reset_test_state() {
    g_current_test_failed        = false;
    g_failure_reason_recorded    = false;
    g_failure_reason.m_buffer[0] = '\0';
}

void record_failure(const char* message) {
    g_current_test_failed = true;
    if (!g_failure_reason_recorded) {
        ktl::string_view msg_view(message);
        size_t copy_len = ktl::min(msg_view.size(), sizeof(g_failure_reason.m_buffer) - 1);
        msg_view.copy(g_failure_reason.m_buffer, copy_len);
        g_failure_reason.m_buffer[copy_len] = '\0';
        g_failure_reason_recorded           = true;
    }
    auto& output = kernel::shell::shell_output();
    emit_harness_event(output, "@@HARNESS {{\"event\":\"error\",\"message\":\"{0}\"}}\n", message);
}

void send_test_start(kernel::shell::ShellOutput& output, const kernel::testing::ktest& test) {
    auto current_time = kernel::time::ns_since_boot();
    emit_harness_event(output, "@@HARNESS {{\"event\":\"test_start\",\"name\":\"{0}\",\"timestamp\":{1}}}\n", test.name,
                       current_time);
}

void send_test_end(kernel::shell::ShellOutput& output, const kernel::testing::ktest& test, const char* status,
                   const char* reason = nullptr) {
    auto current_time = kernel::time::ns_since_boot();
    if (reason) {
        emit_harness_event(output,
                           "@@HARNESS "
                           "{{\"event\":\"test_end\",\"name\":\"{0}\",\"status\":\"{1}\",\"reason\":\"{2}\","
                           "\"timestamp\":{3}}}\n",
                           test.name, status, reason, current_time);
    } else {
        emit_harness_event(output,
                           "@@HARNESS "
                           "{{\"event\":\"test_end\",\"name\":\"{0}\",\"status\":\"{1}\",\"timestamp\":{2}}}\n",
                           test.name, status, current_time);
    }
}

void execute_test(kernel::shell::ShellOutput& output, kernel::testing::ktest& test) {
    g_current_test = &test;
    kernel::crash::set_test_name(test.name);
    reset_test_state();
    send_test_start(output, test);

    if (test.init_fn) { test.init_fn(); }
    test.fn();

    if (g_current_test_failed) {
        send_test_end(output, test, "fail", g_failure_reason_recorded ? g_failure_reason.c_str() : nullptr);
    } else {
        send_test_end(output, test, "pass");
    }
    g_current_test = nullptr;
    kernel::crash::set_test_name(nullptr);
}

void list_tests(kernel::shell::ShellOutput& output) {
    for (auto* test = tests_begin(); test != tests_end(); ++test) {
        const char* module    = test->submodule ? test->submodule : "";
        bool clean_env        = (test->flags & kernel::testing::KTEST_FLAG_REQUIRES_CLEAN_ENV) != 0;
        bool expects_crash    = (test->flags & kernel::testing::KTEST_FLAG_EXPECTS_CRASH) != 0;
        const char* clean_str = clean_env ? ",\"requires_clean_env\":true" : "";
        const char* crash_str = expects_crash ? ",\"expects_crash\":true" : "";
        emit_harness_event(output, "@@HARNESS {{\"event\":\"test\",\"name\":\"{0}\",\"module\":\"{1}\"{2}{3}}}\n",
                           test->name, module, clean_str, crash_str);
    }
}

void test_handler(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: test list|run|run-all\n");
        return;
    }

    if (argv[1] == "list") {
        list_tests(output);
    } else if (argv[1] == "run") {
        if (argc < 3) {
            output.print("usage: test run <name>\n");
            return;
        }
        auto test = find_test(argv[2]);
        if (!test) {
            emit_harness_event(output, "@@HARNESS {{\"event\":\"error\",\"message\":\"Test not found: {0}\"}}\n",
                               argv[2]);
            return;
        }
        execute_test(output, *test);
    } else if (argv[1] == "run-all") {
        int total  = 0;
        int passed = 0;
        int failed = 0;
        for (auto* test = tests_begin(); test != tests_end(); ++test) {
            ++total;
            execute_test(output, *test);
            if (g_current_test_failed) {
                ++failed;
            } else {
                ++passed;
            }
        }
        output.print("{0} tests: {1} passed, {2} failed\n", total, passed, failed);
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

void handle_assertion_failure(const char* message, bool fatal, unsigned char exit_code = 1) {
    record_failure(message);
    if (fatal) { kernel::testing::abort(exit_code); }
}

}  // namespace

KSHELL_COMMAND(test, "test", "Test runner", test_handler);

void kernel::testing::abort(unsigned char exit_code) {
    auto& output = kernel::shell::shell_output();

    if (g_current_test) {
        if (!g_failure_reason_recorded) {
            ktl::fixed_string<128> reason;
            ktl::format::format_to_buffer_raw(reason.m_buffer, sizeof(reason.m_buffer), "abort({0}) requested",
                                              static_cast<unsigned>(exit_code));
            record_failure(reason.c_str());
        }
        send_test_end(output, *g_current_test, "fail", g_failure_reason_recorded ? g_failure_reason.c_str() : nullptr);
        g_current_test = nullptr;
    }

    emit_harness_event(output, "@@HARNESS {{\"event\":\"abort\",\"code\":{0}}}\n", static_cast<unsigned>(exit_code));
    kernel::arch::harness_exit(static_cast<uint8_t>(exit_code));
    hcf();
}

// Assertion backend.
//
// The legacy KTEST_* assertion macros are now thin aliases over the expression-capturing EXPECT/REQUIRE
// machinery (see <kernel/testing/expect.h> and <kernel/testing/testing.h>), so the single sink below
// serves every assertion on this tier -- there are no longer per-type ktest_expect_equal overloads.

// Backend for the expression-capturing EXPECT/REQUIRE macros. Operands arrive pre-formatted from the
// shared decomposer; here we assemble the reason string and route it through the existing failure path.
void kernel::testing::report_assertion(bool passed, bool fatal, const char* file, int line, const char* expr_text,
                                       const char* lhs, const char* op, const char* rhs) {
    if (passed) { return; }
    ktl::fixed_string<256> reason;
    if (op && op[0]) {
        ktl::format::format_to_buffer_raw(reason.m_buffer, sizeof(reason.m_buffer), "{0}  ({1} {2} {3})  at {4}:{5}",
                                          expr_text, lhs, op, rhs, file, line);
    } else {
        ktl::format::format_to_buffer_raw(reason.m_buffer, sizeof(reason.m_buffer), "{0}  (= {1})  at {2}:{3}",
                                          expr_text, lhs, file, line);
    }
    handle_assertion_failure(reason.c_str(), fatal);
}

#endif  // CONFIG_KERNEL_SHELL && CONFIG_KERNEL_TESTING
