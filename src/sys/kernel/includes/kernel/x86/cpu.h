#pragma once

#include <stdint.h>

namespace kernel {

struct cpu_core {
    bool initialized;
    uint32_t lapic_id;
};

}  // namespace kernel