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

// A file the boot protocol loaded alongside the kernel. `role` is what the boot configuration
// tagged it with, not its filename, so the kernel asks for what it needs ("init") rather than
// where it happens to live -- renaming or moving the file cannot break boot.
//
// Module bytes are classified memory_kind::KERNEL, so they stay wired and remain readable for the
// life of the system. Nothing reclaims them yet; the address and length here are what a future
// initrd path needs to hand the page-aligned interior back to the PMM once it is done with it.
struct boot_module {
    const char* role;
    const void* data;
    size_t size;
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
    // Files the protocol loaded alongside the kernel. Empty when it loaded none or supports none.
    const boot_module* modules;
    size_t module_count;
    // True when the paging mode the port's paging code assumes is in effect
    // (Sv48 on riscv64; always true on x86_64, where long mode fixes the walk).
    bool paging_mode_ok;
    // CPUs reported by the boot protocol, densely indexed by list position.
    // Zero when the protocol supplied no CPU list. boot_cpu_index is the boot
    // CPU's position in that list, or SIZE_MAX if the list omits it; it is
    // meaningless when cpu_count is zero.
    size_t cpu_count;
    size_t boot_cpu_index;
};

// Implemented once per boot protocol under boot/<protocol>/, selected by the build's
// BOOT variable. Caches on first call, so callers may invoke it in any order and as
// often as they like. Never panics -- absent data is reported as zero or null fields.
const boot_info& collect();

// The module tagged with `role`, or null if the protocol loaded no such module. Protocol-neutral:
// a plain search over boot_info::modules.
const boot_module* find_module(const char* role);

// Hardware id (x86_64 LAPIC id, riscv64 hartid) of the CPU at dense list
// position `index` in the protocol's CPU list; requires index < cpu_count.
uint64_t cpu_hw_id(size_t index);

// Release the secondary CPU at dense list position `index` into
// entry(index, hw_id) on its own bootloader-provided stack. All CPUs share one
// entry function, and entry must not return. Never call this for the boot CPU.
void start_cpu(size_t index, void (*entry)(size_t core_index, uint64_t hw_id));

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
