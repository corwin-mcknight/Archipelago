#include <kernel/drivers/ps2_keyboard.h>
#include <kernel/platform.h>
#include <kernel/x86/ioport.h>

namespace kernel::platform {
namespace {

constexpr uint16_t DATA_PORT       = 0x60;
constexpr uint16_t STATUS_PORT     = 0x64;
constexpr uint8_t OUTPUT_FULL      = 1u << 0;
constexpr uint8_t MOUSE_DATA       = 1u << 5;
constexpr unsigned int DRAIN_LIMIT = 32;

kernel::driver::ps2_keyboard_decoder g_keyboard;
char g_pending[2];
uint8_t g_pending_begin = 0;
uint8_t g_pending_end   = 0;

int emit_special(int key) {
    char final = 0;
    switch (key) {
        case kernel::driver::ps2_keyboard_decoder::arrow_up: final = 'A'; break;
        case kernel::driver::ps2_keyboard_decoder::arrow_down: final = 'B'; break;
        case kernel::driver::ps2_keyboard_decoder::arrow_right: final = 'C'; break;
        case kernel::driver::ps2_keyboard_decoder::arrow_left: final = 'D'; break;
        case kernel::driver::ps2_keyboard_decoder::home: final = 'H'; break;
        case kernel::driver::ps2_keyboard_decoder::end: final = 'F'; break;
        case kernel::driver::ps2_keyboard_decoder::delete_key:
            g_pending[0]    = '3';
            g_pending[1]    = '~';
            g_pending_begin = 0;
            g_pending_end   = 2;
            return 0x1b;
        default: return -1;
    }
    g_pending[0]    = '[';
    g_pending[1]    = final;
    g_pending_begin = 0;
    g_pending_end   = 2;
    return 0x1b;
}

}  // namespace

int console_input_read() {
    if (g_pending_begin != g_pending_end) { return g_pending[g_pending_begin++]; }

    // Consume only bytes already waiting in the controller. No controller
    // commands are issued: that preserves firmware USB-legacy emulation and is
    // harmless on machines without an i8042 (whose status port reads 0xff).
    for (unsigned int drained = 0; drained < DRAIN_LIMIT; ++drained) {
        const uint8_t status = inb(STATUS_PORT);
        if (status == 0xff || (status & OUTPUT_FULL) == 0) { return -1; }
        const uint8_t data = inb(DATA_PORT);
        if ((status & MOUSE_DATA) != 0) { continue; }
        const int decoded = g_keyboard.feed(data);
        if (decoded >= 0 && decoded <= 0xff) { return decoded; }
        if (decoded > 0xff) { return emit_special(decoded); }
    }
    return -1;
}

}  // namespace kernel::platform
