#include <kernel/x86/ioport.h>
#include <kernel/x86/platforms/pc/pit.h>
#include <stdint.h>

namespace kernel::platform::pc {

namespace {
constexpr uint64_t PIT_BASE_FREQ_HZ = 1193182;

uint16_t pit_read_count() {
    outb(0x43, 0x00);  // latch channel 0
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)((hi << 8) | lo);
}
}  // namespace

bool pit_poll_wait_ms(unsigned int ms) {
    // Every wait is bounded so a dead or absent PIT (legacy-free boards, QEMU -machine pit=off)
    // degrades to a false return instead of hanging boot. Each poll is ~3 port I/O ops (a few us),
    // so the cap gives seconds of real time -- orders of magnitude past any healthy window.
    constexpr uint64_t SPIN_CAP = 500'000;

    // Channel 0, mode 2 (rate generator), lo/hi access, reload 0 = 65536: the counter free-runs
    // over the full 16-bit range, so unsigned 16-bit subtraction handles wrap between polls.
    outb(0x43, 0x34);
    outb(0x40, 0x00);
    outb(0x40, 0x00);

    // The counting element is undefined until the first CLK edge after the reload write, so the
    // first latched value can be garbage. Discard everything up to the first observed change; the
    // reload lands within one PIT clock (~838 ns), well inside the first poll's port-I/O latency,
    // so the post-change value is the freshly loaded counter.
    uint16_t last  = pit_read_count();
    uint64_t spins = 0;
    while (true) {
        uint16_t cur = pit_read_count();
        if (cur != last) {
            last = cur;
            break;
        }
        if (++spins >= SPIN_CAP) { return false; }
    }

    const uint64_t target = PIT_BASE_FREQ_HZ * ms / 1000;
    uint64_t elapsed      = 0;
    spins                 = 0;
    while (elapsed < target) {
        uint16_t cur = pit_read_count();
        elapsed += (uint16_t)(last - cur);
        last = cur;
        if (++spins >= SPIN_CAP) { return false; }
    }
    return true;
}

}  // namespace kernel::platform::pc
