#pragma once
#include <stddef.h>
#include <stdint.h>

// Kernel time is represented as a 'clock tick'. This may not be real time.
typedef uint64_t ktime_t;
typedef int64_t time_ns_t;

namespace kernel {

class time {
   public:
    static ktime_t now();
    static time_ns_t ns_since_boot();
    static time_ns_t ktime_to_ns(ktime_t ktime);
    static void tick();
    static void init(time_ns_t ns_per_tick);

   private:
    static ktime_t _now;
    static time_ns_t _ns_per_tick;
};

};  // namespace kernel