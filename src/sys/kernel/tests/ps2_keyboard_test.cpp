#include <kernel/drivers/ps2_keyboard.h>
#include <kernel/testing/expect.h>
#include <kernel/testing/testing.h>

using kernel::driver::ps2_keyboard_decoder;
using namespace kernel::testing;

KTEST_MODULE("drivers/ps2_keyboard");

KTEST_CASE(ps2_keyboard_decodes_text_and_modifiers) {
    ps2_keyboard_decoder decoder;
    EXPECT(decoder.feed(0x1e) == 'a');
    EXPECT(decoder.feed(0x9e) == ps2_keyboard_decoder::none);
    EXPECT(decoder.feed(0x2a) == ps2_keyboard_decoder::none);
    EXPECT(decoder.feed(0x1e) == 'A');
    EXPECT(decoder.feed(0xaa) == ps2_keyboard_decoder::none);
    EXPECT(decoder.feed(0x02) == '1');
    EXPECT(decoder.feed(0x2a) == ps2_keyboard_decoder::none);
    EXPECT(decoder.feed(0x02) == '!');
    EXPECT(decoder.feed(0xaa) == ps2_keyboard_decoder::none);
    EXPECT(decoder.feed(0x1c) == '\n');
    EXPECT(decoder.feed(0x0e) == '\b');
}

KTEST_CASE(ps2_keyboard_caps_lock_and_shift_compose) {
    ps2_keyboard_decoder decoder;
    EXPECT(decoder.feed(0x3a) == ps2_keyboard_decoder::none);
    EXPECT(decoder.feed(0x1e) == 'A');
    EXPECT(decoder.feed(0x2a) == ps2_keyboard_decoder::none);
    EXPECT(decoder.feed(0x1e) == 'a');
    EXPECT(decoder.feed(0xaa) == ps2_keyboard_decoder::none);
    EXPECT(decoder.feed(0xba) == ps2_keyboard_decoder::none);
    EXPECT(decoder.feed(0x1e) == 'A');
}

KTEST_CASE(ps2_keyboard_decodes_extended_arrows_and_ignores_releases) {
    ps2_keyboard_decoder decoder;
    EXPECT(decoder.feed(0xe0) == ps2_keyboard_decoder::none);
    EXPECT(decoder.feed(0x48) == ps2_keyboard_decoder::arrow_up);
    EXPECT(decoder.feed(0xe0) == ps2_keyboard_decoder::none);
    EXPECT(decoder.feed(0xc8) == ps2_keyboard_decoder::none);
    EXPECT(decoder.feed(0xe0) == ps2_keyboard_decoder::none);
    EXPECT(decoder.feed(0x50) == ps2_keyboard_decoder::arrow_down);
}

KTEST_CASE(ps2_keyboard_discards_pause_sequence) {
    ps2_keyboard_decoder decoder;
    constexpr uint8_t tail[] = {0x1d, 0x45, 0xe1, 0x9d, 0xc5};
    EXPECT(decoder.feed(0xe1) == ps2_keyboard_decoder::none);
    for (uint8_t byte : tail) { EXPECT(decoder.feed(byte) == ps2_keyboard_decoder::none); }
    EXPECT(decoder.feed(0x30) == 'b');
}
