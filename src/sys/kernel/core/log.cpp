#include "kernel/log.h"

#include "kernel/config.h"
#include "kernel/time.h"
#include "ktl/algorithm"
#include "ktl/fixed_string"

kernel::system_log g_log;

struct loglevel_config_static {
    const char status;
    const char* color;
};

static constexpr loglevel_config_static log_level_data[] = {
    {.status = 't', .color = "\033[0m"},     // Reset
    {.status = 'd', .color = "\033[0;37m"},  // Debug
    {.status = 'i', .color = "\033[0;36m"},  // Info
    {.status = 'w', .color = "\033[0;33m"},  // Warn
    {.status = 'E', .color = "\033[0;31m"},  // Error
    {.status = 'F', .color = "\033[1;91m"}   // Fatal
};

void kernel::system_log::flush() {
    // Single active flusher: the ring's flushing flag is a try-skip, so concurrent (and
    // interrupt-context) flushes never block. The drain emits READY slots in sequence order and
    // stops at the first in-progress slot.
    m_ring.drain([this](const log_message& message) {
        const int clamped_level = ktl::clamp((int)message.level(), 0, (int)ktl::size(log_level_data) - 1);
        char status             = log_level_data[clamped_level].status;
        time_ns_t timestamp     = kernel::time::ktime_to_ns(message.timestamp);
        uint64_t time_seconds   = (uint64_t)timestamp / 1'000'000'000;
        uint64_t time_ms        = (uint64_t)(timestamp / 1'000'000) % 1'000;

        // Render the front string
        ktl::fixed_string<32> front;

        if (m_colors) {
            const char* color = log_level_data[ktl::max(clamped_level, 1)].color;
            ktl::format::format_to_buffer_raw(front.m_buffer, front.size(), "{0:s}{1:03d}.{2:03d} {3:1c} | ", color,
                                              time_seconds, time_ms, status);
        } else {
            ktl::format::format_to_buffer_raw(front.m_buffer, front.size(), "{0:03d}.{1:03d} {2:1c} | ", time_seconds,
                                              time_ms, status);
        }
        for (auto dev : devices) {
            dev->write_string(front);
            message.text.for_each([&](char c) {
                dev->write_byte(c);
                if (c == '\n') { dev->write_string("..........| "); }
            });

            if (m_colors) { dev->write_string("\033[0m"); }
            dev->write_byte('\n');
        }
    });
}
