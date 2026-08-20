#pragma once

#include <stddef.h>
#include <stdint.h>

// What the kernel needs from the board, as opposed to from the CPU.
//
// `kernel::arch` covers the instruction set: page-table encoding, traps,
// context switch, CSR/MSR access. This covers the machine those instructions
// run on: which devices exist, where they are, and how fast their clocks tick.
// Each board implements these in <arch>/platforms/<board>/, selected by the build's
// BOARD variable. A board belongs to exactly one architecture, so board code
// may use that architecture's instructions directly.
struct register_frame;

namespace kernel::platform {

// CPU trap causes and board IRQs share the interrupt manager. Keep board
// sources in a separate range (hardware source N maps to this base plus N).
constexpr unsigned int BOARD_INTERRUPT_BASE = 16;

/// Bring up the board's console device and register it as a log sink. Called
/// once by the boot CPU before the first log line. On a board whose console is
/// MMIO (virt), the physical map must already be resolved; on a port-I/O
/// console (pc) there is no such constraint.
void console_init();

/// Quiesce the board's fixed interrupt hardware so the first interrupts-enabled
/// window is quiet. Called once by the boot CPU before interrupts are first
/// enabled. On pc this remaps and masks the legacy 8259 PICs; on virt there is
/// nothing to quiesce.
void interrupt_init();

/// Claim, dispatch, and complete a board-level external interrupt. Called for
/// the RISC-V supervisor-external trap; boards without a controller return false.
bool dispatch_external_interrupt(::register_frame* regs);

/// Enable or disable one mapped board interrupt source. CPU trap-cause ids
/// (timer/software traps) and boards without a controller are ignored.
void interrupt_set_source_enabled(unsigned int id, bool enabled);

/// Interrupt-manager id for the board's console UART, discovered from firmware.
/// Zero means the board has no routed UART interrupt description.
unsigned int console_uart_interrupt_id();

/// Bring up the board's kernel tick source. Called once by the boot CPU after
/// the interrupt manager is up and interrupts are enabled, since the timer
/// registers an interrupt handler and starts delivering ticks immediately.
void timer_init();

/// Signal test-harness exit through the board's debug-exit device (QEMU's
/// isa-debug-exit on pc, the sifive_test finisher on virt). Returns on
/// hardware that has no such device, so callers must halt afterwards.
void harness_exit(uint8_t code);

/// Cycles per second for kernel::arch::timestamp(), 0 if not yet established
/// (readers must handle 0). The counter is a CPU register; its rate is a
/// property of the board -- a fixed timebase on virt, calibrated against the
/// board's tick source on pc.
uint64_t timestamp_hz();

/// Establish timestamp_hz(). Requires a ticking kernel timer and interrupts
/// enabled; called once from late boot before the scheduler starts.
void timestamp_calibrate();

/// Arm the board's hardware watchdog, if it has one, and spawn the thread that
/// feeds it. Called once by the boot CPU after the scheduler is online. From
/// then on a kernel that stops scheduling hard-resets the board instead of
/// hanging forever; boards without a watchdog do nothing.
void watchdog_init();

/// Ask the firmware or board to reset the machine. Returns only if the board
/// has no reset path, so callers must handle coming back.
void reboot();

/// Write the CPU data cache back to physical memory over [vaddr, vaddr+bytes),
/// so a device that reads DRAM directly -- a non-coherent display or DMA engine
/// -- sees the CPU's writes instead of stale memory. A no-op on cache-coherent
/// machines (the QEMU targets). vaddr must lie in the physical map; the board
/// implementation derives the physical line addresses from it.
void dcache_clean_range(const void* vaddr, size_t bytes);

}  // namespace kernel::platform
