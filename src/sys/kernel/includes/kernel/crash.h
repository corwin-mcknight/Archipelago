#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/registers.h"

namespace kernel::crash {

namespace arch {
// True if vaddr is mapped (present at every level of a software walk of the
// current CR3, honoring huge pages) and thus safe for the crash dumper to
// read. Safe on garbage input: canonical check first, bounded 4-level loop.
bool probe_readable(uintptr_t vaddr);
}  // namespace arch

enum class trigger_kind : unsigned {
    panic     = 1,
    exception = 2,
    assertion = 3,
    watchdog  = 4,
};

[[noreturn]] void dispatch(trigger_kind kind, register_frame_t* regs, const char* message = nullptr,
                           const char* file = nullptr, int line = 0);

void set_harness_enabled(bool enabled);
void set_test_name(const char* name);

void crash_write(const char* s);
void crash_write_n(const char* s, size_t n);

}  // namespace kernel::crash
