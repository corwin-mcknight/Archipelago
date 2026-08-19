#ifndef KERNEL_ASSERT_H
#define KERNEL_ASSERT_H

#include "kernel/crash.h"
#include "kernel/log.h"

#ifdef NDEBUG
constexpr bool assertions_enabled = false;
#else
constexpr bool assertions_enabled = true;
#endif

template <typename T>
void kernel_assert(T condition, const char* message, const char* message_text, const char* fname, int line) {
    if constexpr (assertions_enabled) {
        if (!condition) {
            g_log.fatal("Assertion failed: {0} ({1}), {2}:{3}", message_text, message, fname, line);
            kernel::crash::dispatch(kernel::crash::trigger_kind::assertion, nullptr, message_text, fname, line);
        }
    }
}

// Always-on companion to assert, for invariants whose failure must never be silent: at these
// sites a compiled-out check would mean a silently lost thread or leaked resource rather than a
// caught bug, so the panic survives NDEBUG.
template <typename T>
void kernel_ensure(T condition, const char* message, const char* message_text, const char* fname, int line) {
    if (!condition) {
        g_log.fatal("Invariant failed: {0} ({1}), {2}:{3}", message_text, message, fname, line);
        kernel::crash::dispatch(kernel::crash::trigger_kind::assertion, nullptr, message_text, fname, line);
    }
}

#undef assert
#define assert(x, msg) kernel_assert(x, msg, #x, __FILE__, __LINE__)
#define ensure(x, msg) kernel_ensure(x, msg, #x, __FILE__, __LINE__)

#endif
