#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ktl/atomic>
#include <ktl/fixed_string>
#include <ktl/fmt>
#include <ktl/static_vector>
#include <ktl/string_view>
#include <ktl/utility>

#include "kernel/config.h"
#include "kernel/drivers/logging_device.h"
#include "kernel/log_ring.h"
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

    // Sequence is the low 60 bits, level is the top 4 bits (and really only consumes 3).
    uint64_t sequence() const { return level_seq & 0xFFFFFFFFFFFFFFF; }
    log_level level() const { return static_cast<log_level>(level_seq >> sequence_bits); }
};

class system_log {
   public:
    static constexpr size_t max_messages = 128;

    template <log_level level, typename... Args>
    INLINE_RELEASE_ONLY void log(const ktl::string_view fmt, Args... args) {
        uint64_t seq;
        log_message* message = m_ring.reserve(seq);
        if (message == nullptr) { return; }  // ring full -- fail-to-log (see m_ring.dropped())

        message->timestamp = kernel::time::now();
        message->level_seq = (static_cast<uint64_t>(level) << log_message::sequence_bits) | (seq & k_sequence_mask);
        ktl::format::format_to_buffer_raw(message->text.m_buffer, log_message::max_message_size, fmt, args...);
        m_ring.publish(seq);

        // Autoflush puts messages on the console during bring-up. Flushing from interrupt context
        // is permitted and deadlock-free (the flush flag never blocks); a per-CPU in-interrupt gate
        // to make interrupt producers enqueue-only, and the autoflush-off background-worker mode,
        // are deferred refinements -- see .claude/docs/specs/interrupt-safe-log-ring.md.
        if (this->m_autoflush) { flush(); }
    }

// Specialize for log_level::debug
#define LOG_LEVEL_HELPER(thelevel)                                                                            \
    template <typename... Args> INLINE_RELEASE_ONLY void thelevel(const ktl::string_view fmt, Args... args) { \
        log<log_level::thelevel>(fmt, args...);                                                               \
    }

    LOG_LEVEL_HELPER(trace)
    LOG_LEVEL_HELPER(debug)
    LOG_LEVEL_HELPER(info)
    LOG_LEVEL_HELPER(warn)
    LOG_LEVEL_HELPER(error)
    LOG_LEVEL_HELPER(fatal)

    // History scan over the retained window in sequence order (shell `log show`).
    template <typename F> uint64_t for_each(uint64_t minimum_sequence_id, F func) {
        return m_ring.for_each(minimum_sequence_id, [&func](const log_message& message) { func(&message); });
    }

    // Crash-time drain: forces the flush flag and scans the retained window. `visit` is called as
    // visit(const log_message*, bool in_progress); an in-progress slot's bytes are untrustworthy.
    template <typename F> void crash_for_each(F visit) {
        m_ring.crash_scan([&visit](const log_message& message, bool in_progress) { visit(&message, in_progress); });
    }

    void flush();
    ktl::static_vector<kernel::driver::logging_device*, CONFIG_LOG_MAX_DEVICES> devices;

    uint64_t dropped() const { return m_ring.dropped(); }

   private:
    static constexpr uint64_t k_sequence_mask = (static_cast<uint64_t>(1) << log_message::sequence_bits) - 1;

    log_ring<log_message, max_messages> m_ring;
    bool m_autoflush = true;
};

};  // namespace kernel

extern kernel::system_log g_log;
