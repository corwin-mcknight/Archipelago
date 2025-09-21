#include "kernel/log.h"

#include "kernel/config.h"
#include "kernel/time.h"
#include "ktl/fixed_string"

kernel::system_log g_log;

// Color codes for the log levels...
#if CONFIG_KERNEL_LOG_COLORS
static const char* log_level_colors[] = {
    "\033[0m",     // Reset
    "\033[0;37m",  // Debug
    "\033[0;36m",  // Info
    "\033[0;33m",  // Warn
    "\033[0;31m",  // Error
    "\033[1;91m",  // Fatal
};
#endif

void kernel::system_log::flush() {
    this->last_flushed_sequence =
        this->for_each(this->last_flushed_sequence, [&](const log_message* message) {
            char status = '-';
            int color = 0;

            switch (message->level()) {
                case log_level::trace:
                    status = 't';
                    color = 1;
                    break;
                case log_level::debug:
                    status = 'd';
                    color = 1;
                    break;
                case log_level::info:
                    status = 'i';
                    color = 2;
                    break;
                case log_level::warn:
                    status = 'w';
                    color = 3;
                    break;
                case log_level::error:
                    status = 'E';
                    color = 4;
                    break;
                case log_level::fatal:
                    status = 'F';
                    color = 5;
                    break;
            }

            time_ns_t timestamp = kernel::time::ktime_to_ns(message->timestamp);
            uint64_t time_seconds = (uint64_t)timestamp / 1'000'000'000;
            uint64_t time_ms = (uint64_t)(timestamp / 1'000'000) % 1'000;

            // Render the front string
            ktl::fixed_string<32> front;

#if CONFIG_KERNEL_LOG_COLORS
            ktl::format::format_to_buffer_raw(front.m_buffer, front.length(),
                                              "{0:s}{1:03d}.{2:03d} {3:1c} | ", log_level_colors[color],
                                              time_seconds, time_ms, status);

#else 
            ktl::format::format_to_buffer_raw(front.m_buffer, front.length(),
                                                "{0:03d}.{1:03d} {2:1c} | ", time_seconds, time_ms, status);
#endif
            for (auto dev : devices) {
                dev->write_string(front);
                message->text.for_each([&](char c) {
                    dev->write_byte(c);
                    if (c == '\n') {
                        for (size_t i = 0; i < 10; i++) { dev->write_byte(' '); }
                        dev->write_byte('|');
                        dev->write_byte(' ');
                    }
                });

#if CONFIG_KERNEL_LOG_COLORS
                dev->write_string("\033[0m");
#endif
                dev->write_byte('\n');
            }
        });
}
