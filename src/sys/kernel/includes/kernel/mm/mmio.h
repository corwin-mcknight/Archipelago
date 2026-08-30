#pragma once

#include <kernel/mm/page.h>
#include <stddef.h>
#include <stdint.h>

namespace kernel::mm {

// A bounded device-register window. It intentionally has no pointer conversion: clients state the
// width and offset of every access, while this class owns bounds, alignment, and ordering checks.
class mmio_region {
   public:
    constexpr mmio_region() = default;

    constexpr bool valid() const { return m_base != 0; }
    constexpr size_t size() const { return m_size; }

    uint8_t read8(size_t offset) const;
    uint16_t read16(size_t offset) const;
    uint32_t read32(size_t offset) const;
    uint64_t read64(size_t offset) const;

    void write8(size_t offset, uint8_t value) const;
    void write16(size_t offset, uint16_t value) const;
    void write32(size_t offset, uint32_t value) const;
    void write64(size_t offset, uint64_t value) const;

   private:
    constexpr mmio_region(uintptr_t base, size_t size) : m_base(base), m_size(size) {}
    uintptr_t checked(size_t offset, size_t width) const;

    uintptr_t m_base = 0;
    size_t m_size    = 0;

    friend mmio_region map_mmio(physical_range range);
};

// This currently uses the bootloader direct map. The construction seam permits a later dedicated,
// cache-correct kernel device mapping without changing drivers.
mmio_region map_mmio(physical_range range);

}  // namespace kernel::mm
