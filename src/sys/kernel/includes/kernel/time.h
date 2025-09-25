#pragma once
#include <stddef.h>
#include <stdint.h>

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
     */
    static time_ns_t ns_since_boot();

    /**
     * @brief Converts a kernel tick value into nanoseconds using the configured tick period.
     */
    static time_ns_t ktime_to_ns(ktime_t ktime);

    /**
     * @brief Advances the kernel tick counter by one and updates cached time bookkeeping.
     */
    static void tick();

    /**
     * @brief Initializes the time subsystem with the number of nanoseconds represented by each tick.
     */
    static void init(time_ns_t ns_per_tick);

   private:
    /** Current kernel tick count. */
    static ktime_t _now;
    /** Nanoseconds represented by a single kernel tick. */
    static time_ns_t _ns_per_tick;
};

};  // namespace kernel