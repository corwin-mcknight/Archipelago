#pragma once

#include <stdint.h>

namespace kernel::obj {

using ObjectId = uint64_t;
using TypeId   = uint32_t;
using Rights   = uint32_t;

namespace type_ids {
constexpr TypeId INVALID = 0;
constexpr TypeId EVENT   = 1;
constexpr TypeId COUNTER = 2;
}  // namespace type_ids

constexpr Rights RIGHT_READ      = 1 << 0;
constexpr Rights RIGHT_WRITE     = 1 << 1;
constexpr Rights RIGHT_DUPLICATE = 1 << 2;
constexpr Rights RIGHT_TRANSFER  = 1 << 3;
constexpr Rights RIGHT_SIGNAL    = 1 << 4;
constexpr Rights RIGHTS_ALL      = 0x1F;

}  // namespace kernel::obj
