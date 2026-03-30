#include <kernel/config.h>

#if CONFIG_KERNEL_SHELL && CONFIG_KERNEL_TESTING

#include <kernel/shell/shell.h>
#include <kernel/testing/testing.h>
#include <kernel/time.h>
#include <kernel/x86/ioport.h>

#include <ktl/algorithm>
#include <ktl/fixed_string>
#include <ktl/fmt>
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

template <typename... Args>
void emit_harness_event(kernel::shell::ShellOutput& output, const char* fmt, const Args&... args) {
    ktl::fixed_string<kHarnessBufferSize> buffer;
    ktl::format::format_to_buffer_raw(buffer.m_buffer, sizeof(buffer.m_buffer), fmt, args...);
    output.write(buffer.c_str());
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
    // Also emit as error
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
}

void list_tests(kernel::shell::ShellOutput& output) {
    for (auto* test = tests_begin(); test != tests_end(); ++test) {
        const char* module = test->submodule ? test->submodule : "";
        if (test->flags & kernel::testing::KTEST_FLAG_REQUIRES_CLEAN_ENV) {
            emit_harness_event(
                output,
                "@@HARNESS "
                "{{\"event\":\"test\",\"name\":\"{0}\",\"module\":\"{1}\",\"requires_clean_env\":true}}\n",
                test->name, module);
        } else {
            emit_harness_event(output, "@@HARNESS {{\"event\":\"test\",\"name\":\"{0}\",\"module\":\"{1}\"}}\n",
                               test->name, module);
        }
    }
}

void test_handler(int argc, const char* const argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: test list|run|run-all\n");
        return;
    }

    ktl::string_view sub(argv[1]);
    if (sub == "list") {
        list_tests(output);
    } else if (sub == "run") {
        if (argc < 3) {
            output.print("usage: test run <name>\n");
            return;
        }
        ktl::string_view name(argv[2]);
        kernel::testing::ktest* test = nullptr;
        for (auto* t = tests_begin(); t != tests_end(); ++t) {
            if (name == t->name) {
                test = t;
                break;
            }
        }
        if (!test) {
            emit_harness_event(output, "@@HARNESS {{\"event\":\"error\",\"message\":\"Test not found: {0}\"}}\n",
                               argv[2]);
            return;
        }
        execute_test(output, *test);
    } else if (sub == "run-all") {
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

// Assertion helpers

constexpr const char* kExpectationLabel = "Expectation";
constexpr const char* kRequirementLabel = "Requirement";

void format_condition_failure(ktl::fixed_string<256>& buffer, const char* label, const char* condition_str,
                              const char* file, int line) {
    ktl::format::format_to_buffer_raw(buffer.m_buffer, sizeof(buffer.m_buffer), "{0} failed: {1} at {2}:{3}", label,
                                      condition_str, file, line);
}

void format_equality_failure(ktl::fixed_string<256>& buffer, const char* label, bool expect_equal, int actual,
                             int expected, const char* actual_str, const char* expected_str, const char* file,
                             int line) {
    if (expect_equal) {
        ktl::format::format_to_buffer_raw(buffer.m_buffer, sizeof(buffer.m_buffer),
                                          "{0} failed: {1} == {2} (lhs {3}, rhs {4}) at {5}:{6}", label, actual_str,
                                          expected_str, actual, expected, file, line);
        return;
    }

    ktl::format::format_to_buffer_raw(buffer.m_buffer, sizeof(buffer.m_buffer),
                                      "{0} failed: {1} != {2} (both {3}) at {4}:{5}", label, actual_str, expected_str,
                                      actual, file, line);
}

void handle_assertion_failure(const char* message, bool fatal, unsigned char exit_code = 1) {
    record_failure(message);
    if (fatal) { kernel::testing::abort(exit_code); }
}

}  // namespace

KSHELL_COMMAND(test, "test", "Test runner", test_handler);

// kernel::testing::abort implementation
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
    outw(0x604, static_cast<uint16_t>(exit_code | 0x2000));
    while (true) { asm volatile("hlt"); }
}

// Assertion function implementations

void ktest_expect(bool condition, const char* file, int line, const char* condition_str) {
    if (!condition) {
        ktl::fixed_string<256> reason;
        format_condition_failure(reason, kExpectationLabel, condition_str, file, line);
        handle_assertion_failure(reason.c_str(), false);
    }
}

void ktest_require(bool condition, const char* file, int line, const char* condition_str) {
    if (!condition) {
        ktl::fixed_string<256> reason;
        format_condition_failure(reason, kRequirementLabel, condition_str, file, line);
        handle_assertion_failure(reason.c_str(), true);
    }
}

template <typename T>
void ktest_equality_check(T actual, T expected, bool expect_equal, bool fatal, const char* label, const char* file,
                          int line, const char* actual_str, const char* expected_str) {
    bool mismatch = expect_equal ? (actual != expected) : (actual == expected);
    if (mismatch) {
        ktl::fixed_string<256> reason;
        format_equality_failure(reason, label, expect_equal, static_cast<int>(actual), static_cast<int>(expected),
                                actual_str, expected_str, file, line);
        handle_assertion_failure(reason.c_str(), fatal);
    }
}

#define DEFINE_EQUALITY_ASSERTIONS(T)                                                                      \
    void ktest_expect_equal(T actual, T expected, const char* file, int line, const char* actual_str,      \
                            const char* expected_str) {                                                    \
        ktest_equality_check<T>(actual, expected, true, false, kExpectationLabel, file, line, actual_str,  \
                                expected_str);                                                             \
    }                                                                                                      \
    void ktest_require_equal(T actual, T expected, const char* file, int line, const char* actual_str,     \
                             const char* expected_str) {                                                   \
        ktest_equality_check<T>(actual, expected, true, true, kRequirementLabel, file, line, actual_str,   \
                                expected_str);                                                             \
    }                                                                                                      \
    void ktest_expect_not_equal(T actual, T expected, const char* file, int line, const char* actual_str,  \
                                const char* expected_str) {                                                \
        ktest_equality_check<T>(actual, expected, false, false, kExpectationLabel, file, line, actual_str, \
                                expected_str);                                                             \
    }                                                                                                      \
    void ktest_require_not_equal(T actual, T expected, const char* file, int line, const char* actual_str, \
                                 const char* expected_str) {                                               \
        ktest_equality_check<T>(actual, expected, false, true, kRequirementLabel, file, line, actual_str,  \
                                expected_str);                                                             \
    }

DEFINE_EQUALITY_ASSERTIONS(int)
DEFINE_EQUALITY_ASSERTIONS(size_t)
#undef DEFINE_EQUALITY_ASSERTIONS

#endif  // CONFIG_KERNEL_SHELL && CONFIG_KERNEL_TESTING
