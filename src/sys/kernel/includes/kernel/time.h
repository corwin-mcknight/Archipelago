#pragma once
#include <stddef.h>
#include <stdint.h>

#include <ktl/atomic>

// Kernel time is represented as a 'clock tick'. The duration of a tick is set to the minimum granularity of the highest
// precision clock.
/** @typedef ktime_t
 *  @brief Kernel tick counter used by the scheduler timebase.
 */
typedef uint64_t ktime_t;
/** @typedef time_ns_t
 *  @brief Signed nanosecond duration relative to kernel boot.
 */
typedef int64_t time_ns_t;

namespace kernel {

class time {
   public:
    /**
     * @brief Returns the current kernel tick count.
     *
     * The value monotonically increases each time the scheduler's tick handler
     * advances the time source. It is expressed in abstract ticks rather than
     * wall-clock units.
     */
    static ktime_t now();

    /**
     * @brief Returns the elapsed nanoseconds since the kernel booted.
     *
     * Once use_timestamp_clock() has adopted the platform's cycle counter this
     * reads at counter resolution; before that it is derived from the tick
     * counter and quantizes to one tick.
     */
    static time_ns_t ns_since_boot();

    /**
     * @brief Converts a kernel tick value into nanoseconds using the configured tick period.
     */
    static time_ns_t ktime_to_ns(ktime_t ktime);

    /**
     * @brief Converts a nanosecond duration into ticks, rounding up.
     *
     * Rounding up keeps a timeout a floor -- a wait never expires early because the duration
     * quantized down to fewer ticks. Returns at least 1 for any nonzero duration.
     */
    static ktime_t ns_to_ticks_ceil(uint64_t ns);

    /**
     * @brief Advances the kernel tick counter by one and updates cached time bookkeeping.
     */
    static void tick();

    /**
     * @brief Initializes the time subsystem with the number of nanoseconds represented by each tick.
     */
    static void init(time_ns_t ns_per_tick);

    /**
     * @brief Adopts kernel::arch::timestamp() as the high-resolution timebase for ns_since_boot().
     *
     * Anchors the counter to the current tick-derived time so the switch is continuous. A no-op
     * while kernel::platform::timestamp_hz() is still 0 (counter rate not yet established).
     */
    static void use_timestamp_clock();

   private:
    /** Current kernel tick count. */
    static ktl::atomic<ktime_t> _now;
    /** Nanoseconds represented by a single kernel tick. */
    static time_ns_t _ns_per_tick;
    /** Counter rate adopted by use_timestamp_clock(); 0 means tick-derived time. */
    static ktl::atomic<uint64_t> _timestamp_hz;
    /** Counter value at adoption time. */
    static uint64_t _anchor_timestamp;
    /** Tick-derived nanoseconds at adoption time. */
    static time_ns_t _anchor_ns;
};

};  // namespace kernel
