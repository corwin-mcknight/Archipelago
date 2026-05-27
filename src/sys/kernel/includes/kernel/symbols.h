#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::symbols {

// Parse a kernel ELF blob and snapshot function symbols into a kernel-owned
// table. Safe to call before PMM init. Idempotent: subsequent calls are no-ops.
// `elf_data`/`elf_size` may come from Limine's executable_file response; the
// blob does not need to outlive this call (the entries we care about are
// copied out).
void init(const void* elf_data, size_t elf_size);

// True if init() succeeded and lookup() can return a name.
bool available();

// Look up the function containing `addr`. Returns nullptr if no symbol covers
// the address. When non-null, `*offset_out` (if provided) is the byte offset
// from the symbol's start.
const char* lookup(uintptr_t addr, size_t* offset_out = nullptr);

// Best-effort Itanium C++ ABI demangler. Handles `_ZN<len><name>...E` nested
// names and the bare `_Z<len><name>` form; substitutes `_GLOBAL__N_1` for
// `(anonymous namespace)`. Appends `()` to indicate a function. Anything more
// exotic (templates, substitutions, operators, ctors/dtors) is rejected with
// false so callers can fall back to the mangled string.
bool demangle(const char* mangled, char* out, size_t out_size);

}  // namespace kernel::symbols
