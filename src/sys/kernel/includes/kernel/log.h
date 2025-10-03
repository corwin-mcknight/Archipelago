#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ktl/circular_buffer>
#include <ktl/fixed_string>
#include <ktl/static_vector>
#include <ktl/string_view>
#include <ktl/utility>

#include "kernel/config.h"
#include "kernel/drivers/logging_device.h"
#include "kernel/syncronization/semaphore.h"
#include "kernel/syncronization/spinlock.h"
#include "kernel/time.h"

namespace kernel {

// Log level is a 4-bit value that represents the severity of a log message.
enum class log_level : uint8_t {
    trace = 0b000,
    debug = 0b001,
    info  = 0b010,
    warn  = 0b011,
    error = 0b100,
    fatal = 0b101,
};

/// A log message is a 256 byte structure that contains a
/// timestamp, a level, a sequence number, and a message.
struct log_message {
    constexpr static size_t max_size         = 256;
    constexpr static size_t max_message_size = max_size - 16;
    constexpr static size_t sequence_bits    = 60;

    ktime_t timestamp;   // 8 bytes
    uint64_t level_seq;  // 8 bytes, 60 bits for sequence, a nibble for level
    ktl::fixed_string<max_message_size> text;

    log_message() = default;
    explicit log_message(ktime_t time, log_level level, uint64_t sequence, const char* message)
        : timestamp(time), level_seq((static_cast<uint64_t>(level) << sequence_bits) | sequence), text(message) {}

    explicit log_message(ktime_t time, log_level level, uint64_t sequence, const ktl::string_view message)
        : timestamp(time), level_seq((static_cast<uint64_t>(level) << sequence_bits) | sequence), text(message) {}

    explicit log_message(ktime_t time, log_level level, uint64_t sequence,
                         const ktl::fixed_string<max_message_size> message)
        : timestamp(time), level_seq((static_cast<uint64_t>(level) << sequence_bits) | sequence), text(message) {}

    // Move constructor
    log_message(log_message&& other) noexcept
        : timestamp(other.timestamp), level_seq(other.level_seq), text(ktl::move(other.text)) {}

    // Copy constructor
    log_message(const log_message& other) noexcept
        : timestamp(other.timestamp), level_seq(other.level_seq), text(other.text) {}

    // Copy assignment operator
    log_message& operator=(const log_message& other) noexcept {
        timestamp = other.timestamp;
        level_seq = other.level_seq;
        text      = other.text;
        return *this;
    }

    // Move assignment operator
    log_message& operator=(log_message&& other) noexcept {
        timestamp = other.timestamp;
        level_seq = other.level_seq;
        text      = ktl::move(other.text);
        return *this;
    }

    // Sequence is first 56 bits, level is last 4 bits (and really only consumes 3.)
    uint64_t sequence() const { return level_seq & 0xFFFFFFFFFFFFFFF; }
    log_level level() const { return static_cast<log_level>(level_seq >> sequence_bits); }
};

class system_log {
   public:
    static constexpr size_t max_messages = 64;
    uint64_t last_seq                    = 0;

    ktl::circular_buffer<log_message, max_messages> messages;
    kernel::synchronization::semaphore message_gate;
    kernel::synchronization::spinlock flush_lock;

    template <log_level level, typename... Args> inline void log(const ktl::string_view fmt, Args... args) {
        message_gate.acquire();
        uint64_t seq = last_seq++;

        ktl::fixed_string<log_message::max_message_size> string;
        ktl::format::format_to_buffer_raw(string.m_buffer, log_message::max_message_size, fmt, args...);

        messages.emplace(log_message(kernel::time::now(), level, seq, string));
        message_gate.release();

        if (this->m_autoflush) { flush(); }
    }

// Specialize for log_level::debug
#define LOG_LEVEL_HELPER(thelevel)                                                               \
    template <typename... Args> inline void thelevel(const ktl::string_view fmt, Args... args) { \
        log<log_level::thelevel>(fmt, args...);                                                  \
    }

    LOG_LEVEL_HELPER(trace)
    LOG_LEVEL_HELPER(debug)
    LOG_LEVEL_HELPER(info)
    LOG_LEVEL_HELPER(warn)
    LOG_LEVEL_HELPER(error)
    LOG_LEVEL_HELPER(fatal)

    template <typename F> uint64_t for_each(uint64_t minimum_sequence_id, F func) {
        uint64_t next_sequence = minimum_sequence_id;
        messages.for_each([&next_sequence, func](const log_message message) {
            if (message.sequence() >= next_sequence) {
                func(&message);
                next_sequence = message.sequence() + 1;
            }
        });
        return next_sequence;
    }

    void flush();
    ktl::static_vector<kernel::driver::logging_device*, CONFIG_LOG_MAX_DEVICES> devices;

   private:
    uint64_t last_flushed_sequence = 0;
    bool m_autoflush               = true;
    [[maybe_unused]] char pad[7];  // Ensure 64-byte alignment
};

};  // namespace kernel

extern kernel::system_log g_log;
