#pragma once

#include <stdint.h>

namespace kernel::driver {

// Stateful decoder for the translated scan-code set 1 stream produced by a PC
// i8042 controller. Hardware access stays in the PC platform; keeping the
// decoder pure makes modifier and release behavior host-testable.
class ps2_keyboard_decoder {
   public:
    enum special_key : int {
        none     = -1,
        arrow_up = 0x100,
        arrow_down,
        arrow_right,
        arrow_left,
        home,
        end,
        delete_key,
    };

    int feed(uint8_t scan_code) {
        if (m_pause_bytes != 0) {
            --m_pause_bytes;
            return none;
        }
        if (scan_code == 0xe0) {
            m_extended = true;
            return none;
        }
        // Pause/Break has a multi-byte E1 sequence. It is not useful to the
        // shell; discard the fixed five-byte tail without treating it as keys.
        if (scan_code == 0xe1) {
            m_pause_bytes = 5;
            m_extended    = false;
            return none;
        }

        const bool released = (scan_code & 0x80) != 0;
        const uint8_t code  = scan_code & 0x7f;

        if (!m_extended && (code == 0x2a || code == 0x36)) {
            m_shift = !released;
            return none;
        }
        if (!m_extended && code == 0x3a && !released) {
            m_caps_lock = !m_caps_lock;
            return none;
        }

        const bool extended = m_extended;
        m_extended          = false;
        if (released) { return none; }
        if (extended) { return decode_extended(code); }

        char normal  = 0;
        char shifted = 0;
        decode_printable(code, normal, shifted);
        if (normal == 0) { return none; }
        if (normal >= 'a' && normal <= 'z') { return (m_shift != m_caps_lock) ? normal - 'a' + 'A' : normal; }
        return m_shift ? shifted : normal;
    }

   private:
    static int decode_extended(uint8_t code) {
        switch (code) {
            case 0x47: return home;
            case 0x48: return arrow_up;
            case 0x4b: return arrow_left;
            case 0x4d: return arrow_right;
            case 0x4f: return end;
            case 0x50: return arrow_down;
            case 0x53: return delete_key;
            case 0x1c: return '\n';  // keypad Enter
            default: return none;
        }
    }

    static void decode_printable(uint8_t code, char& normal, char& shifted) {
        // US QWERTY, scan-code set 1. Umbra's original table supplied the
        // useful baseline; this version also handles modifiers and punctuation.
        static constexpr char normal_map[] = {
            0,    0x1b, '1', '2',  '3', '4', '5', '6', '7',  '8', '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
            't',  'y',  'u', 'i',  'o', 'p', '[', ']', '\n', 0,   'a', 's', 'd', 'f', 'g',  'h',  'j', 'k', 'l', ';',
            '\'', '`',  0,   '\\', 'z', 'x', 'c', 'v', 'b',  'n', 'm', ',', '.', '/', 0,    '*',  0,   ' ', 0,
        };
        static constexpr char shifted_map[] = {
            0,   0x1b, '!', '@', '#', '$', '%', '^', '&',  '*', '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R',
            'T', 'Y',  'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S', 'D', 'F', 'G',  'H',  'J', 'K', 'L', ':',
            '"', '~',  0,   '|', 'Z', 'X', 'C', 'V', 'B',  'N', 'M', '<', '>', '?', 0,    '*',  0,   ' ', 0,
        };
        if (code >= sizeof(normal_map)) { return; }
        normal  = normal_map[code];
        shifted = shifted_map[code];
    }

    bool m_extended       = false;
    bool m_shift          = false;
    bool m_caps_lock      = false;
    uint8_t m_pause_bytes = 0;
};

}  // namespace kernel::driver
