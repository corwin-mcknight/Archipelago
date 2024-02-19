#pragma once

#include <stddef.h>

namespace kernel {

/// @brief Expect is the kernel's version of assert.
class expect {
    constexpr static bool assertions_enabled = true;
    enum class strategy {
        panic,
        warn,
        debug
    };

    void that_impl(bool condition, strategy action, const char* message, const char* file, size_t line) {
        if (!assertions_enabled) { return; }
        if (!condition) { return; }
    }

};

}  // namespace kernel