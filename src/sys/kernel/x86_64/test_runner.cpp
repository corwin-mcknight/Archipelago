
#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/algorithm>
#include <ktl/fixed_string>
#include <ktl/fmt>
#include <ktl/string_view>

#include "kernel/config.h"
#include "kernel/drivers/uart.h"
#include "kernel/log.h"
#include "kernel/time.h"
#include "kernel/x86/ioport.h"

extern "C" kernel::testing::ktest __start__ktests[], __stop__ktests[];

// UART is provided by the platform layer.
extern kernel::driver::uart uart;

namespace {

class TestRunner {
   public:
    [[noreturn]]
    void run() {
        g_log.info("Entered kernel testing mode...");
        g_log.flush();

        while (true) {
            send_waiting_event();
            const ktl::string_view command = read_command();

            if (command == "LIST") {
                list_tests();
            } else if (command.starts_with("RUN ")) {
                if (command.size() <= 4) {
                    send_error("Missing test name in RUN command");
                    continue;
                }

                const char* test_name_cstr = command_buffer_ + 4;
                const size_t test_name_length = command.size() - 4;
                if (kernel::testing::ktest* test = find_test(ktl::string_view(test_name_cstr, test_name_length))) {
                    execute_test(*test);
                } else {
                    emit_harness_event("@@HARNESS {{\"event\":\"error\",\"message\":\"Test not found: {0}\"}}\n",
                                       test_name_cstr);
                }
            } else {
                emit_harness_event("@@HARNESS {{\"event\":\"error\",\"message\":\"Unknown command: {0}\"}}\n",
                                   command_buffer_);
            }
        }
    }

    [[noreturn]]
    void notify_abort(unsigned char exit_code) {
        if (current_test_) {
            if (!failure_reason_recorded_) {
                ktl::fixed_string<128> reason;
                ktl::format::format_to_buffer_raw(reason.m_buffer, sizeof(reason.m_buffer), "abort({0}) requested",
                                                  static_cast<unsigned>(exit_code));
                record_failure(reason.c_str());
            }
            send_test_end(*current_test_, "fail", failure_reason_recorded_ ? failure_reason_.c_str() : nullptr);
            current_test_ = nullptr;
        }

        send_abort_event(exit_code);
        outw(0x604, exit_code | 0x2000);
        while (true) {}
    }

    void send_error(const char* message) const {
        emit_harness_event("@@HARNESS {{\"event\":\"error\",\"message\":\"{0}\"}}\n", message);
    }

    void handle_assertion_failure(const char* message, bool fatal, unsigned char exit_code = 1) {
        record_failure(message);
        if (fatal) { notify_abort(exit_code); }
    }

   private:
    static constexpr size_t kHarnessBufferSize = 512;
    static constexpr size_t kCommandBufferSize = 256;
    static constexpr unsigned kProtocolVersion = 1;

    static kernel::testing::ktest* tests_begin() { return __start__ktests; }
    static kernel::testing::ktest* tests_end() { return __stop__ktests; }

    template <typename... Args>
    static void emit_harness_event(const char* fmt, const Args&... args) {
        ktl::fixed_string<kHarnessBufferSize> buffer;
        ktl::format::format_to_buffer_raw(buffer.m_buffer, sizeof(buffer.m_buffer), fmt, args...);
        uart.write_string(buffer.c_str());
    }

    void send_waiting_event() const {
        emit_harness_event("@@HARNESS {{\"event\":\"waiting\",\"protocol\":{0}}}\n", kProtocolVersion);
    }

    void send_test_descriptor(const kernel::testing::ktest& test) const {
        const char* module = test.submodule ? test.submodule : "";
        if (test.flags & kernel::testing::KTEST_FLAG_REQUIRES_CLEAN_ENV) {
            emit_harness_event(
                "@@HARNESS "
                "{{\"event\":\"test\",\"name\":\"{0}\",\"module\":\"{1}\",\"requires_clean_env\":true}}\n",
                test.name, module);
            return;
        }
        emit_harness_event("@@HARNESS {{\"event\":\"test\",\"name\":\"{0}\",\"module\":\"{1}\"}}\n", test.name, module);
    }

    void send_test_start(const kernel::testing::ktest& test) {
        auto current_time = kernel::time::ns_since_boot();
        emit_harness_event("@@HARNESS {{\"event\":\"test_start\",\"name\":\"{0}\", \"timestamp\": {1}}}\n", test.name,
                           current_time);
    }

    void send_test_end(const kernel::testing::ktest& test, const char* status, const char* reason = nullptr) {
        auto current_time = kernel::time::ns_since_boot();
        if (reason) {
            emit_harness_event(
                "@@HARNESS "
                "{{\"event\":\"test_end\",\"name\":\"{0}\",\"status\":\"{1}\",\"reason\":\"{2}\","
                "\"timestamp\":{3}}}\n",
                test.name, status, reason, current_time);
        } else {
            emit_harness_event(
                "@@HARNESS "
                "{{\"event\":\"test_end\",\"name\":\"{0}\",\"status\":\"{1}\",\"timestamp\":{2}}}\n",
                test.name, status, current_time);
        }
    }

    void send_abort_event(unsigned char exit_code) const {
        emit_harness_event("@@HARNESS {{\"event\":\"abort\",\"code\":{0}}}\n", static_cast<unsigned>(exit_code));
    }

    ktl::string_view read_command() {
        size_t idx = 0;
        while (idx < sizeof(command_buffer_) - 1) {
            char c = uart.read();
            if (c == '\r') {
                uart.write_byte('\r');
                uart.write_byte('\n');
                break;
            }
            uart.write_byte(c);
            command_buffer_[idx++] = c;
        }
        command_buffer_[idx] = '\0';
        return ktl::string_view(command_buffer_, idx);
    }

    kernel::testing::ktest* find_test(ktl::string_view name) const {
        for (auto* test = tests_begin(); test != tests_end(); ++test) {
            if (name == test->name) { return test; }
        }
        return nullptr;
    }

    void list_tests() {
        for (auto* test = tests_begin(); test != tests_end(); ++test) { send_test_descriptor(*test); }
    }

    void execute_test(kernel::testing::ktest& test) {
        current_test_ = &test;
        reset_current_test_state();
        send_test_start(test);

        if (test.init_fn) { test.init_fn(); }

        test.fn();

        if (current_test_failed_) {
            send_test_end(test, "fail", failure_reason_recorded_ ? failure_reason_.c_str() : nullptr);
        } else {
            send_test_end(test, "pass");
        }
        current_test_ = nullptr;
    }

    char command_buffer_[kCommandBufferSize]{};
    kernel::testing::ktest* current_test_ = nullptr;
    bool current_test_failed_ = false;
    bool failure_reason_recorded_ = false;
    ktl::fixed_string<256> failure_reason_{};

    void reset_current_test_state() {
        current_test_failed_ = false;
        failure_reason_recorded_ = false;
        failure_reason_.m_buffer[0] = '\0';
    }

    void record_failure(const char* message) {
        current_test_failed_ = true;
        if (!failure_reason_recorded_) {
            ktl::string_view msg_view(message);
            size_t copy_len = ktl::min(msg_view.size(), sizeof(failure_reason_.m_buffer) - 1);
            msg_view.copy(failure_reason_.m_buffer, copy_len);
            failure_reason_.m_buffer[copy_len] = '\0';
            failure_reason_recorded_ = true;
        }
        send_error(message);
    }
};

TestRunner g_runner;

}  // namespace

void kernel::testing::abort(unsigned char exit_code) { g_runner.notify_abort(exit_code); }

void kernel::testing::test_runner() { g_runner.run(); }

namespace {

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

}  // namespace

void ktest_expect(bool condition, const char* file, int line, const char* condition_str) {
    if (!condition) {
        ktl::fixed_string<256> reason;
        format_condition_failure(reason, kExpectationLabel, condition_str, file, line);
        g_runner.handle_assertion_failure(reason.c_str(), false);
    }
}

void ktest_require(bool condition, const char* file, int line, const char* condition_str) {
    if (!condition) {
        ktl::fixed_string<256> reason;
        format_condition_failure(reason, kRequirementLabel, condition_str, file, line);
        g_runner.handle_assertion_failure(reason.c_str(), true);
    }
}
// int versions (existing)
void ktest_expect_equal(int actual, int expected, const char* file, int line, const char* actual_str,
                        const char* expected_str) {
    if (actual != expected) {
        ktl::fixed_string<256> reason;
        format_equality_failure(reason, kExpectationLabel, true, actual, expected, actual_str, expected_str, file,
                                line);
        g_runner.handle_assertion_failure(reason.c_str(), false);
    }
}

void ktest_require_equal(int actual, int expected, const char* file, int line, const char* actual_str,
                         const char* expected_str) {
    if (actual != expected) {
        ktl::fixed_string<256> reason;
        format_equality_failure(reason, kRequirementLabel, true, actual, expected, actual_str, expected_str, file,
                                line);
        g_runner.handle_assertion_failure(reason.c_str(), true);
    }
}

void ktest_expect_not_equal(int actual, int expected, const char* file, int line, const char* actual_str,
                            const char* expected_str) {
    if (actual == expected) {
        ktl::fixed_string<256> reason;
        format_equality_failure(reason, kExpectationLabel, false, actual, expected, actual_str, expected_str, file,
                                line);
        g_runner.handle_assertion_failure(reason.c_str(), false);
    }
}

void ktest_require_not_equal(int actual, int expected, const char* file, int line, const char* actual_str,
                             const char* expected_str) {
    if (actual == expected) {
        ktl::fixed_string<256> reason;
        format_equality_failure(reason, kRequirementLabel, false, actual, expected, actual_str, expected_str, file,
                                line);
        g_runner.handle_assertion_failure(reason.c_str(), true);
    }
}

// size_t versions
void ktest_expect_equal(size_t actual, size_t expected, const char* file, int line, const char* actual_str,
                        const char* expected_str) {
    if (actual != expected) {
        ktl::fixed_string<256> reason;
        format_equality_failure(reason, kExpectationLabel, true, static_cast<int>(actual), static_cast<int>(expected),
                                actual_str, expected_str, file, line);
        g_runner.handle_assertion_failure(reason.c_str(), false);
    }
}

void ktest_require_equal(size_t actual, size_t expected, const char* file, int line, const char* actual_str,
                         const char* expected_str) {
    if (actual != expected) {
        ktl::fixed_string<256> reason;
        format_equality_failure(reason, kRequirementLabel, true, static_cast<int>(actual), static_cast<int>(expected),
                                actual_str, expected_str, file, line);
        g_runner.handle_assertion_failure(reason.c_str(), true);
    }
}

void ktest_expect_not_equal(size_t actual, size_t expected, const char* file, int line, const char* actual_str,
                            const char* expected_str) {
    if (actual == expected) {
        ktl::fixed_string<256> reason;
        format_equality_failure(reason, kExpectationLabel, false, static_cast<int>(actual), static_cast<int>(expected),
                                actual_str, expected_str, file, line);
        g_runner.handle_assertion_failure(reason.c_str(), false);
    }
}

void ktest_require_not_equal(size_t actual, size_t expected, const char* file, int line, const char* actual_str,
                             const char* expected_str) {
    if (actual == expected) {
        ktl::fixed_string<256> reason;
        format_equality_failure(reason, kRequirementLabel, false, static_cast<int>(actual), static_cast<int>(expected),
                                actual_str, expected_str, file, line);
        g_runner.handle_assertion_failure(reason.c_str(), true);
    }
}

#endif  // CONFIG_KERNEL_TESTING
