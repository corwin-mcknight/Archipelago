#pragma once

#include <kernel/sched/thread.h>
#include <kernel/sched/trace.h>

#include <ktl/fmt>

// Display helpers shared by the scheduler-inspection shell commands (sched, top).

namespace kernel::shell {

inline const char* state_name(kernel::sched::thread_state s) {
    using kernel::sched::thread_state;
    switch (s) {
        case thread_state::READY: return "READY";
        case thread_state::RUNNING: return "RUNNING";
        case thread_state::BLOCKED: return "BLOCKED";
        case thread_state::DEAD: return "DEAD";
        default: return "?";
    }
}

// Renders a cycle count into buf ("1.71s", "454us", "123cyc") and returns buf for inline use.
inline const char* human_str(char* buf, size_t len, uint64_t cycles, uint64_t hz) {
    auto h = kernel::sched::cycles_to_human(cycles, hz);
    if (h.unit[0] == 'c' || h.unit[0] == 'u') {
        ktl::format::format_to_buffer_raw(buf, len, "{0}{1}", h.whole, h.unit);
    } else {
        ktl::format::format_to_buffer_raw(buf, len, "{0}.{1}{2}{3}", h.whole, h.hundredths / 10, h.hundredths % 10,
                                          h.unit);
    }
    return buf;
}

}  // namespace kernel::shell
