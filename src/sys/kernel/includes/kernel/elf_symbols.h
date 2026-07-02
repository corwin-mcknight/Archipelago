#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/maybe>

namespace kernel::symbols::detail {

// Minimal ELF64 types -- avoids pulling in a vendored elf.h.
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

constexpr uint32_t kShtSymtab  = 2;
constexpr uint8_t kSttFunc     = 2;

// Magic bytes and class checked against e_ident before we trust any other field.
constexpr uint8_t kElfMagic[4] = {0x7f, 'E', 'L', 'F'};
constexpr uint8_t kElfClass64  = 2;

// The validated symbol/string tables init() ingests, isolating the ELF parsing from the ingestion
// loop.
struct symbol_tables {
    const Elf64_Sym* syms;
    size_t count;
    const char* strtab;
    size_t strtab_size;
};

// Validate the ELF header, then locate .symtab and its associated string table. Every malformed or
// out-of-bounds field short-circuits to nothing. Pure (touches no global state), so the host fuzz
// lane can drive it directly on arbitrary bytes.
ktl::maybe<symbol_tables> locate_symbol_tables(const void* elf_data, size_t elf_size);

}  // namespace kernel::symbols::detail
