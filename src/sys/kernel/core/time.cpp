#include "kernel/time.h"

#include "kernel/sched/scheduler.h"

ktl::atomic<ktime_t> kernel::time::_now = 0;
time_ns_t kernel::time::_ns_per_tick    = 0;

void kernel::time::tick() {
    _now.fetch_add(1, ktl::memory_order::relaxed);
    kernel::sched::on_tick();
}
ktime_t kernel::time::now() { return _now.load(ktl::memory_order::relaxed); }
time_ns_t kernel::time::ns_since_boot() {
    return (time_ns_t)((uint64_t)_now.load(ktl::memory_order::relaxed) * (uint64_t)_ns_per_tick);
}
void kernel::time::init(time_ns_t ns_per_tick) { _ns_per_tick = ns_per_tick; }
time_ns_t kernel::time::ktime_to_ns(ktime_t ktime) { return (time_ns_t)((uint64_t)ktime * (uint64_t)_ns_per_tick); }