#pragma once

#include <abi/syscall.h>

namespace kernel::obj {

using ObjectId = uint64_t;
using TypeId   = uint32_t;
using Rights   = uint32_t;

namespace type_ids {
constexpr TypeId INVALID = 0;
constexpr TypeId EVENT   = 1;
// 2 was COUNTER, removed.
constexpr TypeId REGION  = 3;
constexpr TypeId VMO     = 4;
constexpr TypeId THREAD  = 5;
constexpr TypeId TASK    = 6;
// 7 was SEMAPHORE, removed.
constexpr TypeId CHANNEL = 8;
constexpr TypeId PORT    = 9;
constexpr TypeId SOCKET  = 10;
}  // namespace type_ids

constexpr Rights RIGHT_READ      = ABI_RIGHT_READ;
constexpr Rights RIGHT_WRITE     = ABI_RIGHT_WRITE;
constexpr Rights RIGHT_DUPLICATE = ABI_RIGHT_DUPLICATE;
constexpr Rights RIGHT_TRANSFER  = ABI_RIGHT_TRANSFER;
constexpr Rights RIGHT_SIGNAL    = ABI_RIGHT_SIGNAL;
constexpr Rights RIGHT_WAIT      = ABI_RIGHT_WAIT;
constexpr Rights RIGHTS_ALL      = 0x3F;

}  // namespace kernel::obj
