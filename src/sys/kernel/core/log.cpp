#include "kernel/log.h"

#include "kernel/time.h"

void kernel::system_log::flush() {
    this->last_flushed_sequence =
        this->for_each(this->last_flushed_sequence, [&](const log_message* message) {
            for (auto dev : devices) {
                message->text.for_each([&](char c) { dev->write_byte(c); });
                dev->write_byte('\n');
            }
        });
}
