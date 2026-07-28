#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING
#include <kernel/arch.h>
#include <kernel/platform.h>
#include <kernel/time.h>

KTEST_MODULE("riscv64/virt/timer");

// The SBI timer arms at boot with a 1 ms period; kernel time must advance on
// its own. The wait is bounded by the hart's own counter (50 ms of guest time)
// so a slow TCG host cannot flake the test. The bound comes from the board's
// timebase rather than a hardcoded tick count, so it holds on any board whose
// counter runs at a different rate.
KTEST_CASE(virt_sbi_timer_advances_time) {
    ktime_t start     = kernel::time::now();
    uint64_t deadline = kernel::arch::timestamp() + kernel::platform::timestamp_hz() / 20;
    while (kernel::arch::timestamp() < deadline && kernel::time::now() == start) {}
    KTEST_REQUIRE_TRUE(kernel::time::now() > start);
    KTEST_EXPECT_TRUE(kernel::time::ns_since_boot() > 0);
}
#endif
