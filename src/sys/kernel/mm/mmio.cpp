#include <kernel/mm/mmio.h>
#include <kernel/mm/physmap.h>
#include <kernel/panic.h>

namespace kernel::mm {
namespace {

void read_barrier() {
#if defined(__riscv)
    asm volatile("fence i,r" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}

void write_barrier() {
#if defined(__riscv)
    asm volatile("fence w,o" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}

template <typename T> T read_at(uintptr_t address) {
    T value = *reinterpret_cast<volatile T*>(address);
    read_barrier();
    return value;
}

template <typename T> void write_at(uintptr_t address, T value) {
    write_barrier();
    *reinterpret_cast<volatile T*>(address) = value;
}

}  // namespace

uintptr_t mmio_region::checked(size_t offset, size_t width) const {
    if (!valid()) { panic("MMIO access through an invalid region"); }
    if (offset > m_size || width > m_size - offset) { panic("MMIO access outside device region"); }
    if ((offset & (width - 1)) != 0) { panic("unaligned MMIO access"); }
    return m_base + offset;
}

uint8_t mmio_region::read8(size_t offset) const { return read_at<uint8_t>(checked(offset, sizeof(uint8_t))); }
uint16_t mmio_region::read16(size_t offset) const { return read_at<uint16_t>(checked(offset, sizeof(uint16_t))); }
uint32_t mmio_region::read32(size_t offset) const { return read_at<uint32_t>(checked(offset, sizeof(uint32_t))); }
uint64_t mmio_region::read64(size_t offset) const { return read_at<uint64_t>(checked(offset, sizeof(uint64_t))); }

void mmio_region::write8(size_t offset, uint8_t value) const { write_at(checked(offset, sizeof(value)), value); }
void mmio_region::write16(size_t offset, uint16_t value) const { write_at(checked(offset, sizeof(value)), value); }
void mmio_region::write32(size_t offset, uint32_t value) const { write_at(checked(offset, sizeof(value)), value); }
void mmio_region::write64(size_t offset, uint64_t value) const { write_at(checked(offset, sizeof(value)), value); }

mmio_region map_mmio(physical_range range) {
    if (range.size == 0) { panic("cannot map an empty MMIO region"); }
    uintptr_t base = direct_map_address(range.base);
    if (range.size - 1 > UINTPTR_MAX - base) { panic("MMIO region address overflow"); }
    return mmio_region(base, range.size);
}

}  // namespace kernel::mm
