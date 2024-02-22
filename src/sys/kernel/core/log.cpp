#include "kernel/log.h"

#include "kernel/time.h"
#include "ktl/fixed_string"

void kernel::system_log::flush() {
    kernel::time::tick();  // temporary
    this->last_flushed_sequence =
        this->for_each(this->last_flushed_sequence, [&](const log_message* message) {
            char status = '-';

            switch (message->level()) {
                case log_level::debug: status = 'd'; break;
                case log_level::info: status = 'i'; break;
                case log_level::warn: status = 'w'; break;
                case log_level::error: status = 'E'; break;
                case log_level::fatal: status = 'F'; break;
            }

            time_ns_t timestamp = kernel::time::ktime_to_ns(message->timestamp);
            uint64_t time_seconds = (uint64_t)timestamp / 1'000'000'000;
            uint64_t time_ms = (uint64_t)(timestamp / 1'000'000) % 1'000;

            // Render the front string
            ktl::fixed_string<16> front;
            ktl::format::format_to_buffer_raw(front.m_buffer, 50, "{0:03d}.{1:03d} {2:1c} | ", time_seconds,
                                              time_ms, status);

            for (auto dev : devices) {
                dev->write_string(front);
                message->text.for_each([&](char c) {
                    dev->write_byte(c);
                    if (c == '\n') {
                        for (size_t i = 0; i < front.length() - 2; i++) { dev->write_byte(' '); }
                        dev->write_byte('|');
                        dev->write_byte(' ');
                    }
                });
                dev->write_byte('\n');
            }
        });
}
