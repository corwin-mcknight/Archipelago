#pragma once

#include <kernel/elf.h>
#include <stddef.h>
#include <stdint.h>

#include <ktl/maybe>

namespace kernel::symbols::detail {

// The validated symbol/string tables init() ingests, isolating the ELF parsing from the ingestion
// loop.
struct symbol_tables {
    const elf::Elf64_Sym* syms;
    size_t count;
    const char* strtab;
    size_t strtab_size;
};

// Validate the ELF header, then locate .symtab and its associated string table. Every malformed or
// out-of-bounds field short-circuits to nothing. Pure (touches no global state), so the host fuzz
// lane can drive it directly on arbitrary bytes.
ktl::maybe<symbol_tables> locate_symbol_tables(const void* elf_data, size_t elf_size);

}  // namespace kernel::symbols::detail
