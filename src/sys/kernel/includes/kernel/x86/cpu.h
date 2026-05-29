#pragma once

#include <stdint.h>

#include <ktl/atomic>

namespace kernel {

struct cpu_core {
    ktl::atomic<bool> initialized;
    uint32_t lapic_id;
};

}  // namespace kernel