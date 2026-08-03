#include <kernel/platform.h>
#include <kernel/x86/ioport.h>

namespace kernel::platform {

// The legacy 8259 pair is a board fact: every PC inherits it, and a legacy-free board has none.
// The LAPIC delivers all interrupts, so both PICs are fully masked. They are remapped first so any
// spurious IRQ7/IRQ15 the masked PIC still emits lands on a stubbed vector, not an exception
// vector. Real 8259s need settling time between command writes, hence the io_wait() pacing.
void interrupt_init() {
    // ICW1: begin initialization, ICW4 present.
    outb(0x20, 0x11);
    io_wait();
    outb(0xA0, 0x11);
    io_wait();
    // ICW2: vector offsets 0x20 (master) and 0x28 (slave).
    outb(0x21, 0x20);
    io_wait();
    outb(0xA1, 0x28);
    io_wait();
    // ICW3: slave cascaded on the master's IRQ2 line.
    outb(0x21, 0x04);
    io_wait();
    outb(0xA1, 0x02);
    io_wait();
    // ICW4: 8086 mode.
    outb(0x21, 0x01);
    io_wait();
    outb(0xA1, 0x01);
    io_wait();
    // Mask every line on both PICs.
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

}  // namespace kernel::platform
