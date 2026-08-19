#include <kernel/arch.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/platform.h>
#include <kernel/time.h>

namespace kernel::platform {

namespace {
// 1 ms kernel tick, matching the x86_64 tick rate.
constexpr uint64_t TICK_HZ                = 1000;

constexpr unsigned SUPERVISOR_TIMER_CAUSE = 5;       // scause code with the interrupt bit stripped
constexpr uint64_t SIE_STIE               = 1 << 5;  // sie.STIE: supervisor timer interrupt enable

// SBI TIME extension (EID "TIME", FID 0): program the next timer deadline.
// Programming a deadline also clears the pending STIP bit, so the handler
// never touches sip. Returns the SBI error code (0 on success).
int64_t sbi_set_timer(uint64_t deadline) {
    register uint64_t a0 asm("a0") = deadline;
    register uint64_t a1 asm("a1");
    register uint64_t a6 asm("a6") = 0;
    register uint64_t a7 asm("a7") = 0x54494D45;
    asm volatile("ecall" : "+r"(a0), "=r"(a1) : "r"(a6), "r"(a7) : "memory");
    return static_cast<int64_t>(a0);
}
/// Kernel tick timer backed by the SBI TIME extension and the supervisor timer
/// interrupt. Shared by every riscv64 board; the tick interval is derived from
/// the board's timestamp_hz() at init.
class sbi_timer : public kernel::hal::IInterruptHandler {
   public:
    void init();
    time_ns_t resolution_ns();
    bool handle_interrupt(register_frame_t* regs);

   private:
    uint64_t m_ticks_per_interval = 0;
};

// Interrupt-handler object, so its constructor must have run (global ctors)
// before timer_init().
sbi_timer g_timer;
}  // namespace

void timer_init() { g_timer.init(); }

void sbi_timer::init() {
    m_ticks_per_interval = timestamp_hz() / TICK_HZ;
    kernel::time::init(resolution_ns());
    g_interrupt_manager.register_interrupt(SUPERVISOR_TIMER_CAUSE, this, 0);
    if (sbi_set_timer(kernel::arch::timestamp() + m_ticks_per_interval) != 0) {
        g_log.error("timer: SBI TIME extension unavailable; kernel time will not advance");
        return;
    }
    asm volatile("csrs sie, %0" ::"r"(SIE_STIE));
}

time_ns_t sbi_timer::resolution_ns() { return static_cast<time_ns_t>(1'000'000'000ULL / TICK_HZ); }

bool sbi_timer::handle_interrupt(register_frame_t*) {
    // Re-arm first: tick() may preempt into another thread and not return for a full timeslice.
    sbi_set_timer(kernel::arch::timestamp() + m_ticks_per_interval);
    kernel::time::tick();
    return true;
}

}  // namespace kernel::platform
