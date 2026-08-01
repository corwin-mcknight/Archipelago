#pragma once

#include <stddef.h>
#include <stdint.h>

// The ELF64 on-disk format, shared by every consumer that reads an ELF image: the kernel's own
// symbol-table snapshot (<kernel/elf_symbols.h>) and the user-binary loader (<kernel/elf_loader.h>).
// Structure layout only -- no policy about which images are acceptable, which is each consumer's own
// business.

namespace kernel::elf {

struct Elf64_Ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct Elf64_Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

struct Elf64_Sym {
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};

// e_ident indices and the values this kernel accepts.
constexpr size_t EI_CLASS      = 4;
constexpr size_t EI_DATA       = 5;
constexpr size_t EI_VERSION    = 6;
constexpr uint8_t ELF_MAGIC[4] = {0x7f, 'E', 'L', 'F'};
constexpr uint8_t ELFCLASS64   = 2;
constexpr uint8_t ELFDATA2LSB  = 1;
constexpr uint8_t EV_CURRENT   = 1;

constexpr uint16_t ET_REL      = 1;
constexpr uint16_t ET_EXEC     = 2;
constexpr uint16_t ET_DYN      = 3;
constexpr uint16_t ET_CORE     = 4;

constexpr uint16_t EM_X86_64   = 62;
constexpr uint16_t EM_RISCV    = 243;

// The machine this kernel was built for; an image reporting anything else is not ours to run.
#if defined(__x86_64__)
constexpr uint16_t EM_NATIVE = EM_X86_64;
#elif defined(__riscv)
constexpr uint16_t EM_NATIVE = EM_RISCV;
#else
// Host tiers (test runner, fuzzers) build for whatever the developer's machine is. They drive the
// parser on synthetic images, so they pin the target explicitly rather than inheriting the host's.
constexpr uint16_t EM_NATIVE = EM_X86_64;
#endif

constexpr uint32_t PT_LOAD    = 1;
constexpr uint32_t PT_DYNAMIC = 2;
constexpr uint32_t PT_INTERP  = 3;

constexpr uint32_t PF_X       = 1u << 0;
constexpr uint32_t PF_W       = 1u << 1;
constexpr uint32_t PF_R       = 1u << 2;

constexpr uint32_t SHT_SYMTAB = 2;
constexpr uint8_t STT_FUNC    = 2;

// True when the half-open region [offset, offset + size) lies fully inside an `elf_size`-byte blob,
// computed without overflow. Every offset/size pair read out of an image goes through this.
constexpr bool region_in_bounds(uint64_t offset, uint64_t size, size_t elf_size) {
    return offset <= elf_size && size <= elf_size - offset;
}

// The header of an ELF64 little-endian image, or null if `data` is not one. Checks only what every
// consumer must agree on -- size, alignment, magic, class, encoding, version. Object type, machine,
// and segment policy are the caller's to enforce.
//
// The alignment check matters: the header and every table reached through it are read as Elf64
// structs, and binding a reference to misaligned storage is UB (and a fault on strict-alignment
// targets).
inline const Elf64_Ehdr* header_of(const void* data, size_t size) {
    if (data == nullptr || size < sizeof(Elf64_Ehdr)) { return nullptr; }
    if (reinterpret_cast<uintptr_t>(data) % alignof(Elf64_Ehdr) != 0) { return nullptr; }

    const auto* hdr = static_cast<const Elf64_Ehdr*>(data);
    for (size_t i = 0; i < sizeof(ELF_MAGIC); i++) {
        if (hdr->e_ident[i] != ELF_MAGIC[i]) { return nullptr; }
    }
    if (hdr->e_ident[EI_CLASS] != ELFCLASS64) { return nullptr; }
    if (hdr->e_ident[EI_DATA] != ELFDATA2LSB) { return nullptr; }
    if (hdr->e_ident[EI_VERSION] != EV_CURRENT) { return nullptr; }
    return hdr;
}

}  // namespace kernel::elf
