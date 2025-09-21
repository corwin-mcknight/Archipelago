#include <kernel/drivers/uart.h>
#include <kernel/testing/testing.h>

#include <ktl/fixed_string>
#include <ktl/fmt>
#include <ktl/string_view>

#include "kernel/config.h"
#include "kernel/log.h"
#include "kernel/x86/ioport.h"

#if CONFIG_KERNEL_TESTING

extern "C" kernel::testing::ktest __start__ktests[], __stop__ktests[];

// UART is provided by the platform layer.
extern kernel::driver::uart uart;

namespace {

class TestRunner {
   public:
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
                if (kernel::testing::ktest* test =
                        find_test(ktl::string_view(test_name_cstr, test_name_length))) {
                    execute_test(*test);
                } else {
                    emit_harness_event(
                        "@@HARNESS {{\"event\":\"error\",\"message\":\"Test not found: {0}\"}}\n",
                        test_name_cstr);
                }
            } else {
                emit_harness_event("@@HARNESS {{\"event\":\"error\",\"message\":\"Unknown command: {0}\"}}\n",
                                   command_buffer_);
            }
        }
    }

    void notify_abort(unsigned char exit_code) {
        if (current_test_) {
            ktl::fixed_string<64> reason;
            ktl::format::format_to_buffer_raw(reason.m_buffer, sizeof(reason.m_buffer),
                                              "abort({0}) requested", static_cast<unsigned>(exit_code));
            send_test_end(*current_test_, "fail", reason.c_str());
            current_test_ = nullptr;
        }

        send_abort_event(exit_code);

        kernel::testing::abort(exit_code);
        while (true) {}
    }

    void send_error(const char* message) const {
        emit_harness_event("@@HARNESS {{\"event\":\"error\",\"message\":\"{0}\"}}\n", message);
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
                "@@HARNESS {{\"event\":\"test\",\"name\":\"{0}\",\"module\":\"{1}\",\"requires_clean_env\":true}}\n",
                test.name, module);
            return;
        }
        emit_harness_event("@@HARNESS {{\"event\":\"test\",\"name\":\"{0}\",\"module\":\"{1}\"}}\n",
                           test.name, module);
    }

    void send_test_start(const kernel::testing::ktest& test) {
        emit_harness_event("@@HARNESS {{\"event\":\"test_start\",\"name\":\"{0}\"}}\n", test.name);
    }

    void send_test_end(const kernel::testing::ktest& test, const char* status, const char* reason = nullptr) {
        if (reason) {
            emit_harness_event(
                "@@HARNESS "
                "{{\"event\":\"test_end\",\"name\":\"{0}\",\"status\":\"{1}\",\"reason\":\"{2}\"}}\n",
                test.name, status, reason);
        } else {
            emit_harness_event("@@HARNESS {{\"event\":\"test_end\",\"name\":\"{0}\",\"status\":\"{1}\"}}\n",
                               test.name, status);
        }
    }

    void send_abort_event(unsigned char exit_code) const {
        emit_harness_event("@@HARNESS {{\"event\":\"abort\",\"code\":{0}}}\n",
                           static_cast<unsigned>(exit_code));
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
        send_test_start(test);

        if (test.init_fn) { test.init_fn(); }

        test.fn();

        send_test_end(test, "pass");
        current_test_ = nullptr;
    }

    char command_buffer_[kCommandBufferSize]{};
    kernel::testing::ktest* current_test_ = nullptr;
};

TestRunner g_runner;

}  // namespace

void kernel::testing::abort(unsigned char exit_code) {
    g_runner.notify_abort(exit_code);
    outw(0x604, exit_code | 0x2000);
}

void kernel::testing::test_runner() { g_runner.run(); }

void ktest_require(bool condition, const char* file, int line, const char* condition_str) {
    if (!condition) {
        ktl::fixed_string<256> reason;
        ktl::format::format_to_buffer_raw(reason.m_buffer, sizeof(reason.m_buffer),
                                          "Requirement failed: {0} at {1}:{2}", condition_str, file, line);
        g_runner.send_error(reason.c_str());
        g_runner.notify_abort(1);
    }
}

void ktest_require_equal(int actual, int expected, const char* file, int line, const char* actual_str,
                         const char* expected_str) {
    if (actual != expected) {
        ktl::fixed_string<256> reason;
        ktl::format::format_to_buffer_raw(
            reason.m_buffer, sizeof(reason.m_buffer),
            "Requirement failed: {0} == {1} (calculated {2}, expected {3}) at {4}:{5}", actual_str,
            expected_str, actual, expected, file, line);
        g_runner.send_error(reason.c_str());
        g_runner.notify_abort(1);
    }
}

#endif  // CONFIG_KERNEL_TESTING
