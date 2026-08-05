#include <kernel/x86/apic.h>
#include <kernel/x86/cpu.h>

extern uintptr_t g_hhdm_offset;

namespace kernel::x86 {

namespace {
constexpr uint32_t MSR_APIC_BASE   = 0x1B;
// Handled by the dedicated spurious stub in interrupt_handlers.s, which returns without
// dispatching or EOIing (a spurious interrupt sets no ISR bit, so an EOI would dismiss the
// interrupt actually in service). The low four bits must be set for old APICs.
constexpr uint32_t SPURIOUS_VECTOR = 0x2F;

// Register byte offsets into the 4 KiB LAPIC window.
constexpr uintptr_t REG_SPURIOUS   = 0x0F0;
constexpr uintptr_t REG_EOI        = 0x0B0;
constexpr uintptr_t REG_LVT_TIMER  = 0x320;
constexpr uintptr_t REG_TIMER_INIT = 0x380;
constexpr uintptr_t REG_TIMER_CUR  = 0x390;
constexpr uintptr_t REG_TIMER_DIV  = 0x3E0;

constexpr uint32_t SOFTWARE_ENABLE = 1u << 8;
constexpr uint32_t LVT_MASKED      = 1u << 16;
constexpr uint32_t LVT_PERIODIC    = 1u << 17;
constexpr uint32_t DIVIDE_BY_16    = 0x3;

volatile uint32_t* g_lapic         = nullptr;

void reg_write(uintptr_t offset, uint32_t value) { g_lapic[offset / 4] = value; }
uint32_t reg_read(uintptr_t offset) { return g_lapic[offset / 4]; }
}  // namespace

void lapic_init() {
    uint64_t base = rdmsr(MSR_APIC_BASE);
    wrmsr(MSR_APIC_BASE, base | (1ull << 11));  // global enable; normally already set by firmware
    g_lapic = reinterpret_cast<volatile uint32_t*>((base & 0x000FFFFFFFFFF000ull) + g_hhdm_offset);
    reg_write(REG_SPURIOUS, SOFTWARE_ENABLE | SPURIOUS_VECTOR);
}

void lapic_eoi() {
    if (g_lapic == nullptr) { return; }
    reg_write(REG_EOI, 0);
}

void lapic_timer_start_counting() {
    reg_write(REG_TIMER_DIV, DIVIDE_BY_16);
    reg_write(REG_LVT_TIMER, LVT_MASKED);
    reg_write(REG_TIMER_INIT, 0xFFFFFFFFu);
}

uint32_t lapic_timer_elapsed() { return 0xFFFFFFFFu - reg_read(REG_TIMER_CUR); }

void lapic_timer_start_periodic(uint8_t vector, uint32_t initial_count) {
    reg_write(REG_TIMER_DIV, DIVIDE_BY_16);
    reg_write(REG_LVT_TIMER, LVT_PERIODIC | vector);
    reg_write(REG_TIMER_INIT, initial_count);
}

}  // namespace kernel::x86
