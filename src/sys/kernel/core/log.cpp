#include "kernel/log.h"

#include "kernel/time.h"

void kernel::system_log::flush() {
    this->last_flushed_sequence =
        this->for_each(this->last_flushed_sequence, [&](const log_message* message) {
            for (auto dev : devices) {
                dev->write_string("[");
                switch (message->level()) {
                    case log_level::debug: dev->write_string("."); break;
                    case log_level::info: dev->write_string("i"); break;
                    case log_level::warn: dev->write_string("W"); break;
                    case log_level::error: dev->write_string("E"); break;
                    case log_level::fatal: dev->write_string("F"); break;
                }
                dev->write_string("] ");

                message->text.for_each([&](char c) { dev->write_byte(c); });
                dev->write_byte('\n');
            }
        });
}
