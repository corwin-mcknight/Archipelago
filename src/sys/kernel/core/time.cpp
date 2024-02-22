#include "kernel/time.h"

ktime_t kernel::time::_now = 0;
time_ns_t kernel::time::_ns_per_tick = 0;

void kernel::time::tick() { _now++; }
ktime_t kernel::time::now() { return _now; }
time_ns_t kernel::time::ns_since_boot() { return (time_ns_t)_now * _ns_per_tick; }
void kernel::time::init(time_ns_t ns_per_tick) { _ns_per_tick = ns_per_tick; }
time_ns_t kernel::time::ktime_to_ns(ktime_t ktime) { return (time_ns_t)ktime * _ns_per_tick; }