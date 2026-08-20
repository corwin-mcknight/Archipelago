#pragma once

#include <ktl/string_view>

namespace kernel::boot { struct boot_info; }

namespace kernel::console {

// The single text-output seam. Every byte still reaches the boot UART
// synchronously, exactly as before; once init() has brought up a framebuffer
// console the same byte is also queued for the console_framebuffer thread to
// paint. The log and the shell write through here so both land on the panel.
void write_byte(char c);
void write_string(ktl::string_view s);

// Bring up the framebuffer text console and start the console_framebuffer
// thread that drains queued bytes onto the display. A no-op unless the boot
// protocol handed over a 32-bpp linear framebuffer. Must run after the
// scheduler is up, since it spawns a thread.
void init(const kernel::boot::boot_info& info);

}  // namespace kernel::console
