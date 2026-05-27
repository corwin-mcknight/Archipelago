#pragma once

#include <stddef.h>

#include "kernel/registers.h"

namespace kernel::crash {

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
