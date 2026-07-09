#pragma once

#include <stdint.h>

// Arch-neutral boot sequence (core/boot.cpp). Each arch's _start does its own
// bring-up (heap, ctors, UART, CPUs, traps) and calls these in order; every
// function panics on an unusable bootloader response.
namespace kernel::boot {

// Publish g_hhdm_offset from the Limine HHDM response. Must run before any
// MMIO device (including the riscv64 UART) is touched.
void resolve_hhdm();

// Snapshot the kernel ELF's symbol table before bootloader memory is
// recycled; degrades to a warning if the request was not honored.
void snapshot_symbols();

// Feed the Limine memmap to the PMM and bring up the VMM.
void init_memory();

// Object system, scheduler bring-up, boot-mode resolution (Limine cmdline "shell"/"noshell"),
// then the kernel shell thread or a normal boot; never returns (falls into the idle loop).
[[noreturn]] void late_boot(uint32_t boot_core_index);

}  // namespace kernel::boot
