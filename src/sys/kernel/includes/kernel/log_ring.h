#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/atomic>

#include "kernel/config.h"

namespace kernel {

/// Lock-free, multi-producer / single-consumer bounded ring.
///
/// 
/// Producers reserve a sequence lock-free (compare-exchange on the write index, with a
/// fullness check that leaves no hole on overflow -- "fail to log"), write the payload into
/// the reserved slot, and publish it with a release store of the slot state. A single active
/// flusher (serialized by a try-skip flag that never blocks) drains READY slots in order,
/// marking them FLUSHED but retaining their payload as history until the slot is reclaimed.
///
/// The ring assigns and stores each slot's sequence itself, so the payload type `T` is opaque;
/// readers use the stored sequence to reject stale occupants left by an earlier lap.
template <typename T, size_t Capacity> class log_ring {
   public:
    static constexpr size_t capacity = Capacity;

    enum slot_state : uint8_t {
        FREE    = 0,  // never written (pre-fill only)
        WRITING = 1,  // reserved; a producer is copying the payload
        READY   = 2,  // payload committed, awaiting flush
        FLUSHED = 3,  // emitted; payload retained as history until reclaimed
    };

    log_ring()                           = default;
    log_ring(const log_ring&)            = delete;
    log_ring& operator=(const log_ring&) = delete;

    /// Reserve a slot for one message. On success returns a pointer to the payload to fill in
    /// and sets `seq` to the assigned sequence; the caller must call publish(seq) once the
    /// payload is written. Returns nullptr when the ring is full (the message is dropped and
    /// dropped() is incremented). Safe from any context including interrupts: wait-free, never
    /// blocks, and never overwrites a slot a reader may be touching.
    T* reserve(uint64_t& seq) {
        while (true) {
            uint64_t w = m_write.load(ktl::memory_order::acquire);
            uint64_t r = m_read.load(ktl::memory_order::acquire);
            if (w - r >= Capacity) {
                m_dropped.fetch_add(1, ktl::memory_order::relaxed);
                return nullptr;
            }
            uint64_t expected = w;
            if (m_write.compare_exchange(expected, w + 1, ktl::memory_order::acquire)) {
                slot& s = m_slots[w % Capacity];
                s.seq.store(w, ktl::memory_order::relaxed);  // ordered with value by the READY release/acquire
                s.state.store(WRITING, ktl::memory_order::relaxed);
                seq = w;
                return &s.value;
            }
            // Lost the race for this sequence; reload and retry.
        }
    }

    /// Publish a reserved slot, making its payload and sequence visible to readers.
    void publish(uint64_t seq) { m_slots[seq % Capacity].state.store(READY, ktl::memory_order::release); }

    /// Live drain by the single active flusher. `emit(const T&)` is called for each READY slot
    /// in sequence order; the scan stops at the first non-READY slot so output is never
    /// reordered past an in-progress message. Returns immediately if another flusher already
    /// holds the flag -- it never blocks, so it is deadlock-free from any context (an interrupt
    /// that flushes either wins and drains or loses and returns).
    template <typename Emit> void drain(Emit emit) {
        bool expected = false;
        if (!m_flushing.compare_exchange(expected, true, ktl::memory_order::acquire)) { return; }
        uint64_t r = m_read.load(ktl::memory_order::relaxed);  // ordered by the flushing acquire
        while (m_slots[r % Capacity].state.load(ktl::memory_order::acquire) == READY) {
            emit(m_slots[r % Capacity].value);
            m_slots[r % Capacity].state.store(FLUSHED, ktl::memory_order::relaxed);
            r += 1;
            m_read.store(r, ktl::memory_order::release);  // free the slot promptly for reuse
        }
        m_flushing.store(false, ktl::memory_order::release);
    }

    /// Read-only history scan of the retained window in sequence order, for messages with
    /// sequence >= min_seq. `visit(const T&)` is called for each retained (READY or FLUSHED)
    /// message whose stored sequence matches its window position; stale occupants and
    /// in-progress slots are skipped. Does not mutate the ring. Returns one past the highest
    /// sequence visited (or min_seq if none).
    ///
    /// Best-effort under concurrent logging: a producer reclaiming the oldest in-window slot
    /// between the checks and the visit can cause that one entry to be misattributed (single
    /// core) or read torn (SMP, which is deferred). The live drain and the crash scan are not
    /// affected. This path is the developer `log show` view, where that is acceptable.
    template <typename Visit> uint64_t for_each(uint64_t min_seq, Visit visit) const {
        uint64_t w     = m_write.load(ktl::memory_order::acquire);
        uint64_t start = (w > Capacity) ? (w - Capacity) : 0;
        if (start < min_seq) { start = min_seq; }
        uint64_t next = min_seq;
        for (uint64_t seq = start; seq < w; seq++) {
            const slot& s = m_slots[seq % Capacity];
            uint8_t st    = s.state.load(ktl::memory_order::acquire);
            if (st != READY && st != FLUSHED) { continue; }                   // FREE or WRITING
            if (s.seq.load(ktl::memory_order::relaxed) != seq) { continue; }  // stale occupant from an earlier lap
            visit(s.value);
            next = seq + 1;
        }
        return next;
    }

    /// Crash-time drain: forcibly take the flushing flag (the holder may have faulted, so this
    /// does NOT skip if the flag is already held) to lock out other flushers, then scan the
    /// retained window in sequence order. `visit(const T&, bool in_progress)` is called for each
    /// slot; in_progress is true for a slot still being written, whose payload is untrustworthy
    /// (the caller should emit a placeholder rather than the bytes).
    template <typename Visit> void crash_scan(Visit visit) {
        m_flushing.store(true, ktl::memory_order::release);  // force; do not skip if already held
        uint64_t w     = m_write.load(ktl::memory_order::acquire);
        uint64_t start = (w > Capacity) ? (w - Capacity) : 0;
        for (uint64_t seq = start; seq < w; seq++) {
            const slot& s = m_slots[seq % Capacity];
            uint8_t st    = s.state.load(ktl::memory_order::acquire);
            if (st == WRITING) {
                visit(s.value, true);
                continue;
            }
            if (st == FREE) { continue; }
            if (s.seq.load(ktl::memory_order::relaxed) != seq) { continue; }  // stale occupant
            visit(s.value, false);
        }
    }

    /// Number of messages dropped because the ring was full at reservation time.
    uint64_t dropped() const { return m_dropped.load(ktl::memory_order::relaxed); }

    /// Current occupancy (reserved-but-not-yet-flushed-past slots).
    uint64_t size() const {
        uint64_t w = m_write.load(ktl::memory_order::acquire);
        uint64_t r = m_read.load(ktl::memory_order::acquire);
        return w - r;
    }

   private:
    struct slot {
        ktl::atomic<uint8_t> state{};  // FREE
        ktl::atomic<uint64_t> seq{0};  // sequence of the current occupant, for stale-slot rejection
        T value{};
    };

    alignas(CONFIG_CPU_CACHE_LINE_SIZE) ktl::atomic<uint64_t> m_write{0};  // next sequence to assign
    alignas(CONFIG_CPU_CACHE_LINE_SIZE) ktl::atomic<uint64_t> m_read{0};   // next sequence to flush
    ktl::atomic<uint64_t> m_dropped{0};
    ktl::atomic<bool> m_flushing{false};
    slot m_slots[Capacity];
};

}  // namespace kernel
