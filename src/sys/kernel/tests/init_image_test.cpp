#include <kernel/init_image.h>
#include <kernel/testing/testing.h>

KTEST_MODULE("kernel/init_image");

namespace {
using namespace kernel::init_image;
constexpr size_t IMAGE_SIZE = sizeof(Header) + 3 * PAGE;
Header valid_header() { return {MAGIC, VERSION, MACHINE_X86_64, BASE, PAGE, PAGE, PAGE, PAGE}; }
// Validation reads only the header; the size describes the bounds of the boot module.
bool accepts(Header h, size_t size = IMAGE_SIZE, uint64_t machine = MACHINE_X86_64) {
    Header out{};
    return validate(&h, size, machine, out);
}
}  // namespace

KTEST_CASE(init_image_validates_both_machines_and_unaligned_headers) {
    auto h = valid_header();
    KTEST_EXPECT_TRUE(accepts(h));
    KTEST_EXPECT_FALSE(accepts(h, IMAGE_SIZE, MACHINE_RISCV64));
    h.machine = MACHINE_RISCV64;
    KTEST_EXPECT_TRUE(accepts(h, IMAGE_SIZE, MACHINE_RISCV64));
    h.entry++;
    KTEST_EXPECT_FALSE(accepts(h, IMAGE_SIZE, MACHINE_RISCV64));
    h = valid_header();
    uint8_t bytes[sizeof(Header) + 1];
    __builtin_memcpy(bytes + 1, &h, sizeof(h));
    Header out{};
    KTEST_EXPECT_TRUE(validate(bytes + 1, IMAGE_SIZE, MACHINE_X86_64, out));
    KTEST_EXPECT_EQUAL(out.entry, BASE);
}

KTEST_CASE(init_image_rejects_invalid_boot_contracts) {
    Header out{};
    KTEST_EXPECT_FALSE(validate(nullptr, IMAGE_SIZE, MACHINE_X86_64, out));
    auto h = valid_header();
    KTEST_EXPECT_FALSE(accepts(h, sizeof(Header) - 1));
    KTEST_EXPECT_FALSE(accepts(h, IMAGE_SIZE - 1));
    KTEST_EXPECT_FALSE(accepts(h, IMAGE_SIZE + 1));
    h.magic ^= 1;
    KTEST_EXPECT_FALSE(accepts(h));
    h = valid_header();
    h.version++;
    KTEST_EXPECT_FALSE(accepts(h));
    h       = valid_header();
    h.entry = BASE - 1;
    KTEST_EXPECT_FALSE(accepts(h));
    h.entry = BASE + PAGE;
    KTEST_EXPECT_FALSE(accepts(h));
    h           = valid_header();
    h.text_size = 0;
    KTEST_EXPECT_FALSE(accepts(h, IMAGE_SIZE - PAGE));
}

KTEST_CASE(init_image_bounds_memory_and_rejects_wrapping_extents) {
    auto h     = valid_header();
    h.bss_size = LIMIT - BASE - 3 * PAGE;
    KTEST_EXPECT_TRUE(accepts(h));
    h.bss_size += PAGE;
    KTEST_EXPECT_FALSE(accepts(h));
    h           = valid_header();
    h.data_size = UINT64_MAX - PAGE + 1;
    KTEST_EXPECT_FALSE(accepts(h));
    h = valid_header();
    h.ro_size--;
    KTEST_EXPECT_FALSE(accepts(h, IMAGE_SIZE - 1));
}
