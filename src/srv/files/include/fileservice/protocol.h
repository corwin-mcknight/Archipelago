#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fileservice {
inline constexpr char NAME[]          = "files";
inline constexpr size_t MESSAGE_BYTES = 2048;
inline constexpr uint32_t OPEN = 1, STAT = 2, READ = 3, READ_DIR = 4;
inline constexpr uint32_t NOT_FOUND = 1, INVALID_PATH = 2, WRONG_KIND = 3, READ_ONLY = 4, UNSUPPORTED = 5,
                          RESOURCE_EXHAUSTED = 6, INVALID_CURSOR = 7;
inline constexpr uint32_t FILE = 1, DIRECTORY = 2, WRITE_ACCESS = 1;

// Private init-to-server bootstrap exchange. The request carries abi_image_payload and
// one read-only archive VMO. The server registers its name when the archive is ready.
inline constexpr uint32_t ARCHIVE            = 0x46494c45;
inline constexpr uint64_t ARCHIVE_TXID       = 1;
inline constexpr uint64_t STARTUP_TIMEOUT_NS = 5'000'000'000;
}  // namespace fileservice
