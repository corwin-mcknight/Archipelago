#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/maybe>
#include <ktl/span>

namespace kernel::symbols {

// Parse a kernel ELF blob and snapshot function symbols into a kernel-owned
// table. Safe to call before PMM init. Idempotent: subsequent calls are no-ops.
// `elf_data`/`elf_size` may come from Limine's executable_file response; the
// blob does not need to outlive this call (the entries we care about are
// copied out).
void init(const void* elf_data, size_t elf_size);

// True if init() succeeded and lookup() can return a name.
bool available();

// Identifies the function symbol covering a looked-up address.
struct symbol {
    const char* name;  // NUL-terminated, owned by the symbol table
    size_t offset;     // byte offset of the queried address from the symbol's start
};

// Look up the function containing `addr`. Returns nothing if no symbol covers
// the address.
ktl::maybe<symbol> lookup(uintptr_t addr);

// Best-effort Itanium C++ ABI demangler. Handles `_ZN<len><name>...E` nested
// names and the bare `_Z<len><name>` form; substitutes `_GLOBAL__N_1` for
// `(anonymous namespace)`. Appends `()` to indicate a function. Anything more
// exotic (templates, substitutions, operators, ctors/dtors) is rejected with
// false so callers can fall back to the mangled string.
bool demangle(const char* mangled, ktl::span<char> out);

}  // namespace kernel::symbols
