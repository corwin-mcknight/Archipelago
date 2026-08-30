#include "kernel/time.h"

#include "kernel/arch.h"
#include "kernel/platform.h"
#include "kernel/sched/scheduler.h"

ktl::atomic<ktime_t> kernel::time::_now           = 0;
time_ns_t kernel::time::_ns_per_tick              = 0;
ktl::atomic<uint64_t> kernel::time::_timestamp_hz = 0;
uint64_t kernel::time::_anchor_timestamp          = 0;
time_ns_t kernel::time::_anchor_ns                = 0;

// Every scheduling core ticks for preemption; only the boot core advances kernel time.
void kernel::time::tick() {
    if (kernel::sched::on_boot_core()) { _now.fetch_add(1, ktl::memory_order::relaxed); }
    kernel::sched::on_tick();
}
ktime_t kernel::time::now() { return _now.load(ktl::memory_order::relaxed); }
time_ns_t kernel::time::ns_since_boot() {
    // Acquire pairs with the release in use_timestamp_clock(): a nonzero rate
    // guarantees the anchor fields are visible.
    uint64_t hz = _timestamp_hz.load(ktl::memory_order::acquire);
    if (hz == 0) { return ktime_to_ns(now()); }
    // Counters are per-core and not architecturally synchronized with the BSP's
    // anchor; clamp a lagging reader to the anchor instead of letting the
    // unsigned delta wrap ~584 years forward. Per-core anchors when AP
    // scheduling needs cross-core monotonicity.
    uint64_t ts    = kernel::arch::timestamp();
    // Split the conversion so delta * 1e9 cannot overflow: the remainder term
    // stays under hz * 1e9, safe for any counter rate below ~18 GHz.
    uint64_t delta = (ts > _anchor_timestamp) ? ts - _anchor_timestamp : 0;
    uint64_t ns    = (delta / hz) * 1'000'000'000ull + (delta % hz) * 1'000'000'000ull / hz;
    return _anchor_ns + (time_ns_t)ns;
}
void kernel::time::init(time_ns_t ns_per_tick) { _ns_per_tick = ns_per_tick; }
void kernel::time::use_timestamp_clock() {
    uint64_t hz = kernel::platform::timestamp_hz();
    if (hz == 0) { return; }  // no calibrated counter; stay on tick-derived time
    // Anchor at the current tick-derived time so the switch is continuous.
    _anchor_timestamp = kernel::arch::timestamp();
    _anchor_ns        = ktime_to_ns(now());
    _timestamp_hz.store(hz, ktl::memory_order::release);
}
time_ns_t kernel::time::ktime_to_ns(ktime_t ktime) { return (time_ns_t)((uint64_t)ktime * (uint64_t)_ns_per_tick); }
ktime_t kernel::time::ns_to_ticks_ceil(uint64_t ns) {
    uint64_t per = (uint64_t)_ns_per_tick;
    if (per == 0) { return ns; }  // tick period not configured (host tests): treat ns as ticks
    return time_detail::divide_ceil(ns, per);
}
