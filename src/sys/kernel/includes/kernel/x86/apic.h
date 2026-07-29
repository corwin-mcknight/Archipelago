#pragma once

#include <stdint.h>

namespace kernel::x86 {

/// Maps the LAPIC register window through the direct map and software-enables the calling core's
/// LAPIC. Must run before any other lapic_* call delivers or acknowledges an interrupt.
void lapic_init();

/// Signals end-of-interrupt to the calling core's LAPIC. A no-op before lapic_init().
void lapic_eoi();

/// LAPIC timer, fixed divide-by-16. start_counting() begins a masked free run so the caller can
/// measure counts consumed against an external reference via elapsed(). start_periodic() begins
/// periodic delivery of `vector` every `initial_count` counts.
void lapic_timer_start_counting();
uint32_t lapic_timer_elapsed();
void lapic_timer_start_periodic(uint8_t vector, uint32_t initial_count);

}  // namespace kernel::x86
