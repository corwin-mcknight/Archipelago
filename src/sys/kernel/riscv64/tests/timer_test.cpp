#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING
#include <kernel/time.h>

namespace {
uint64_t rdtime() {
    uint64_t t;
    asm volatile("rdtime %0" : "=r"(t));
    return t;
}
}  // namespace

// The SBI timer arms at boot with a 1 ms period; kernel time must advance on
// its own. The wait is bounded by the hart's own 10 MHz counter (50 ms of
// guest time) so a slow TCG host cannot flake the test.
KTEST(riscv_sbi_timer_advances_time, "riscv64/timer") {
    ktime_t start     = kernel::time::now();
    uint64_t deadline = rdtime() + 500000;
    while (rdtime() < deadline && kernel::time::now() == start) {}
    KTEST_REQUIRE_TRUE(kernel::time::now() > start);
    KTEST_EXPECT_TRUE(kernel::time::ns_since_boot() > 0);
}
#endif
