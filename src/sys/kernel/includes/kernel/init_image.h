#pragma once

#include <stddef.h>
#include <stdint.h>

// Private boot contract, emitted by sys/init/init.ld. Not a userspace executable ABI.
namespace kernel::init_image {

constexpr uint64_t MAGIC           = 0x54494e4948435241;  // "ARCHINIT", little endian
constexpr uint64_t VERSION         = 1;
constexpr uint64_t BASE            = 0x400000;
constexpr uint64_t LIMIT           = 0x800000;  // the boot stack starts here
constexpr uint64_t PAGE            = 4096;
constexpr uint64_t MACHINE_X86_64  = 1;
constexpr uint64_t MACHINE_RISCV64 = 2;

struct Header {
    uint64_t magic;
    uint64_t version;
    uint64_t machine;
    uint64_t entry;
    uint64_t text_size;
    uint64_t ro_size;
    uint64_t data_size;
    uint64_t bss_size;
};
static_assert(sizeof(Header) == 64);

// Validate before allocating or mapping anything. Copies the header so unaligned boot bytes
// never become misaligned typed accesses. All regions have fixed, implicit permissions.
inline bool validate(const void* bytes, size_t size, uint64_t machine, Header& out) {
    if (bytes == nullptr || size < sizeof(Header)) { return false; }
    Header h;
    __builtin_memcpy(&h, bytes, sizeof(h));
    if (h.magic != MAGIC || h.version != VERSION || h.machine != machine || h.text_size == 0) { return false; }
    uint64_t extent        = 0;
    const uint64_t sizes[] = {h.text_size, h.ro_size, h.data_size, h.bss_size};
    for (uint64_t length : sizes) {
        if (length % PAGE != 0 || length > LIMIT - BASE - extent) { return false; }
        extent += length;
    }
    const uint64_t file_size = sizeof(Header) + extent - h.bss_size;
    if (file_size != size || h.entry < BASE || h.entry - BASE >= h.text_size) { return false; }
    if (machine == MACHINE_RISCV64 && (h.entry & 1) != 0) { return false; }
    out = h;
    return true;
}

}  // namespace kernel::init_image
