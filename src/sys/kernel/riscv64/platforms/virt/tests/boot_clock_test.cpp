#include <kernel/boot.h>
#include <kernel/testing/testing.h>

KTEST_MODULE("riscv64/virt/boot");

// QEMU virt's firmware supplies the host RTC through Limine. A missing request,
// unhandled response, or wrong timestamp representation must not silently
// degrade into a clockless boot.
KTEST_CASE(boot_protocol_clock_is_reasonable) {
    int64_t epoch = kernel::boot::collect().boot_epoch_seconds;
    KTEST_EXPECT_TRUE(epoch >= 1'577'836'800);  // 2020-01-01 UTC
    KTEST_EXPECT_TRUE(epoch < 4'102'444'800);   // 2100-01-01 UTC
}
