#pragma once

#include <stddef.h>
#include <stdint.h>

// Arch-neutral boot sequence (core/boot.cpp). Each arch's _start does its own
// bring-up (heap, ctors, UART, CPUs, traps) and calls these in order; every
// function panics on an unusable boot_info.
namespace kernel::boot {

// How the boot protocol classified a physical range. Only USABLE and KERNEL
// carry policy: USABLE becomes the page pool, KERNEL stays wired, and OTHER is
// everything the kernel neither allocates from nor tracks.
enum class memory_kind : uint8_t { USABLE, KERNEL, OTHER };

struct memory_range {
    uint64_t base;
    uint64_t length;
    memory_kind kind;
};

// Everything the arch-neutral boot path needs from the boot protocol. Fields a
// protocol could not supply are zero or null; the consumer decides whether that
// is fatal, which is why collect() itself never panics.
struct boot_info {
    // Virtual base of the direct map of physical memory (Limine calls this the HHDM offset).
    uintptr_t physmap_base;
    const memory_range* memory_map;
    size_t memory_map_count;
    // The kernel's own ELF image, for symbol snapshotting. Need not outlive snapshot_symbols().
    const void* kernel_elf;
    size_t kernel_elf_size;
    // Space-delimited kernel command line, or null if the protocol supplied none.
    const char* cmdline;
};

// Implemented once per boot protocol under boot/<protocol>/, selected by the build's
// BOOT variable. Caches on first call, so callers may invoke it in any order and as
// often as they like. Never panics -- absent data is reported as zero or null fields.
const boot_info& collect();

// Publish g_hhdm_offset from boot_info::physmap_base. Must run before any
// MMIO device (including the riscv64 UART) is touched.
void resolve_hhdm();

// Snapshot the kernel ELF's symbol table before bootloader memory is
// recycled; degrades to a warning if the protocol did not supply the image.
void snapshot_symbols();

// Feed the boot memory map to the PMM and bring up the VMM.
void init_memory();

// Object system, scheduler bring-up, boot-mode resolution (cmdline "shell"/"noshell"),
// then the kernel shell thread or a normal boot; never returns (falls into the idle loop).
[[noreturn]] void late_boot(uint32_t boot_core_index);

}  // namespace kernel::boot
