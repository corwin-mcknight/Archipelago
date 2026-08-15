#pragma once

namespace kernel::platform::pc {
/// Performs a busy wait on the PIT for ms. Used for calibration. returns false if PIT is not present.
bool pit_poll_wait_ms(unsigned int ms);

}  // namespace kernel::platform::pc
