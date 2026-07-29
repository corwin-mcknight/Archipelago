#pragma once

namespace kernel::platform::pc {

/// Busy-waits `ms` milliseconds by polling PIT channel 0. Reprograms the channel; the PIT serves
/// only as the fixed-frequency reference the LAPIC timer is calibrated against at boot. Returns
/// false without waiting the full interval if the PIT never counts (dead or absent hardware).
bool pit_poll_wait_ms(unsigned int ms);

}  // namespace kernel::platform::pc
