#pragma once

#include <stdint.h>

#include <ktl/string_view>

namespace kernel::boot { struct boot_info; }

namespace kernel::console {

// The single text-output seam. Every byte still reaches the boot UART
// synchronously, exactly as before; once init() has brought up a framebuffer
// console the same byte is also queued for the console_framebuffer thread to
// paint. The log and the shell write through here so both land on the panel.
void write_byte(char c);
void write_string(ktl::string_view s);

// Holds the console for one line so writers on different cores cannot splice bytes into each
// other's output. Interrupts are off while held; re-entrant on the owning core, so a panic raised
// mid-line can still report.
class line_guard {
   public:
    line_guard();
    ~line_guard();
    line_guard(const line_guard&)            = delete;
    line_guard& operator=(const line_guard&) = delete;

   private:
    uint64_t m_flags;
};

// Bring up the framebuffer text console and start the console_framebuffer
// thread that drains queued bytes onto the display. A no-op unless the boot
// protocol handed over a 32-bpp linear framebuffer. Must run after the
// scheduler is up, since it spawns a thread.
void init(const kernel::boot::boot_info& info);

}  // namespace kernel::console
