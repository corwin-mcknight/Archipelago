#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::sched {

enum class trace_kind : uint8_t { SWITCH = 1, WAKE, SPAWN, EXIT };
enum class switch_reason : uint8_t { NONE = 0, PREEMPT, YIELD, BLOCK, SLEEP, EXIT };

struct trace_record {
    uint64_t timestamp   = 0;  // raw cycles (arch::timestamp)
    uint64_t from_id     = 0;  // thread object id, 0 = none
    uint64_t to_id       = 0;
    trace_kind kind      = trace_kind::SWITCH;
    switch_reason reason = switch_reason::NONE;
};

// Fixed-capacity flight recorder. Carries no lock: the scheduler writes and the shell reads
// with interrupts disabled on the single scheduling core. Pure data -- host-testable.
template <size_t N> class trace_ring {
   public:
    trace_ring() = default;

    void push(const trace_record& r) {
        m_records[m_next] = r;
        m_next            = (m_next + 1) % N;
        if (m_size < N) { ++m_size; }
    }

    void clear() {
        m_next = 0;
        m_size = 0;
    }

    size_t size() const { return m_size; }
    static constexpr size_t capacity() { return N; }

    // Copies up to max records into out, newest first. Returns the count copied.
    size_t copy_newest(trace_record* out, size_t max) const {
        size_t n = m_size < max ? m_size : max;
        for (size_t i = 0; i < n; ++i) {
            size_t idx = (m_next + N - 1 - i) % N;
            out[i]     = m_records[idx];
        }
        return n;
    }

   private:
    trace_record m_records[N] = {};
    size_t m_next             = 0;
    size_t m_size             = 0;
};

// Cycle count rendered for humans without floating point. hz == 0 (uncalibrated) falls back
// to raw cycles. hundredths is meaningful for ms and s units only.
struct human_time {
    uint64_t whole      = 0;
    uint64_t hundredths = 0;
    const char* unit    = "cyc";
};

inline human_time cycles_to_human(uint64_t cycles, uint64_t hz) {
    if (hz == 0) { return human_time{cycles, 0, "cyc"}; }
    // Split before multiplying so cycles near 2^64 cannot overflow: rem < hz <= ~5e9,
    // rem * 1e6 <= 5e15 < 2^64.
    uint64_t us = (cycles / hz) * 1'000'000ull + (cycles % hz) * 1'000'000ull / hz;
    if (us < 1'000ull) { return human_time{us, 0, "us"}; }
    if (us < 1'000'000ull) { return human_time{us / 1'000, (us % 1'000) / 10, "ms"}; }
    return human_time{us / 1'000'000, (us % 1'000'000) / 10'000, "s"};
}

}  // namespace kernel::sched
