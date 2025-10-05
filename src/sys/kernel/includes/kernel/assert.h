#ifndef KERNEL_ASSERT_H
#define KERNEL_ASSERT_H

#ifndef NDEBUG
#include "kernel/log.h"
#include "kernel/panic.h"
constexpr bool assertions_enabled = true;
#endif

enum class assertion_action { panic, warn, debug };

template <typename T, assertion_action A = assertion_action::panic>
void kernel_assert(T condition, const char* message, const char* message_text, const char* fname, int line,
                   assertion_action action = A) {
    if constexpr (assertions_enabled) {
        if (!condition) {
            switch (action) {
                default:
                case assertion_action::debug:
                    g_log.debug("Assertion failed: {0} ({1}), {2}:{3}", message_text, message, fname, line);
                    break;
                case assertion_action::warn:
                    g_log.warn("Assertion failed: {0} (1}), {2}:{3}", message_text, message, fname, line);
                    break;
                case assertion_action::panic:
                    g_log.fatal("Assertion failed: {0} ({1}), {2}:{3}", message_text, message, fname, line);
                    panic("Assertion failed");
                    break;
            }
        }
    }
}

#undef assert
#define assert(x, msg) kernel_assert(x, msg, #x, __FILE__, __LINE__)
#define assert_and(x, msg, y) kernel_assert(x, msg, #x, __FILE__, __LINE__, y)

#endif
