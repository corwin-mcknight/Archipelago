#include "kernel/symbols.h"

#include <stddef.h>
#include <stdint.h>

#include <ktl/algorithm>
#include <ktl/maybe>
#include <ktl/ranges>
#include <ktl/span>
#include <ktl/string_view>

#include "kernel/elf_symbols.h"

namespace kernel::symbols {

using ktl::maybe;
using ktl::nothing;
using ktl::string_view;

namespace {

constexpr size_t kMaxSymbols      = 2048;
constexpr size_t kStringPoolBytes = 48 * 1024;

struct func_entry {
    uintptr_t addr;
    uint32_t size;
    uint32_t name_off;
};

func_entry g_entries[kMaxSymbols];
char g_string_pool[kStringPoolBytes];

size_t g_entry_count = 0;
size_t g_string_used = 0;
bool g_initialized   = false;

// The ELF64 types, constants, and the pure locate_symbol_tables() parser now live in
// <kernel/elf_symbols.h> (namespace detail) so the host fuzz lane can drive the parser directly.
using detail::Elf64_Sym;
using detail::symbol_tables;

// True when the half-open region [offset, offset + size) lies fully inside an
// `elf_size`-byte blob, computed without overflow.
bool region_in_bounds(uint64_t offset, uint64_t size, size_t elf_size) {
    return offset <= elf_size && size <= elf_size - offset;
}

// View a NUL-terminated string starting at `s`, bounded by `end` so a missing
// terminator can never run off the string table. Replaces a hand-rolled strlen.
string_view bounded_view(const char* s, const char* end) {
    string_view full(s, static_cast<size_t>(end - s));
    size_t nul = full.find('\0');
    return full.substr(0, nul == string_view::npos ? full.size() : nul);
}

// Copy `name` into the string pool, returning its offset or nothing if the pool
// is full.
maybe<uint32_t> intern(string_view name) {
    size_t len = name.size();
    if (g_string_used + len + 1 > kStringPoolBytes) { return nothing; }
    uint32_t off = static_cast<uint32_t>(g_string_used);
    name.copy(&g_string_pool[g_string_used], len);
    g_string_pool[g_string_used + len] = '\0';
    g_string_used += len + 1;
    return off;
}

void sort_entries() {
    // Simple insertion sort by address. ~544 entries, cold init path.
    for (size_t i = 1; i < g_entry_count; i++) {
        func_entry x = g_entries[i];
        size_t j     = i;
        while (j > 0 && g_entries[j - 1].addr > x.addr) {
            g_entries[j] = g_entries[j - 1];
            j--;
        }
        g_entries[j] = x;
    }
}

// Greatest entry whose start address is <= `addr` and whose extent covers it.
maybe<const func_entry&> find_entry(uintptr_t addr) {
    size_t lo = 0;
    size_t hi = g_entry_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_entries[mid].addr <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) { return nothing; }

    const func_entry& e = g_entries[lo - 1];
    if (addr >= e.addr + e.size) { return nothing; }
    return e;
}

}  // namespace

// Validate the ELF header, then locate .symtab and its associated string table. Every malformed or
// out-of-bounds field short-circuits to nothing. Declared in <kernel/elf_symbols.h>; uses the
// file-static region_in_bounds above.
namespace detail {
maybe<symbol_tables> locate_symbol_tables(const void* elf_data, size_t elf_size) {
    if (elf_data == nullptr || elf_size < sizeof(Elf64_Ehdr)) { return nothing; }

    const auto* base = static_cast<const uint8_t*>(elf_data);
    const auto* hdr  = reinterpret_cast<const Elf64_Ehdr*>(base);

    if (string_view(reinterpret_cast<const char*>(hdr->e_ident), sizeof(kElfMagic)) !=
        string_view(reinterpret_cast<const char*>(kElfMagic), sizeof(kElfMagic))) {
        return nothing;
    }
    if (hdr->e_ident[4] != kElfClass64) { return nothing; }
    if (hdr->e_shoff == 0 || hdr->e_shentsize < sizeof(Elf64_Shdr)) { return nothing; }
    if (!region_in_bounds(hdr->e_shoff, static_cast<uint64_t>(hdr->e_shnum) * hdr->e_shentsize, elf_size)) {
        return nothing;
    }

    // The section/symbol arrays are read as aligned Elf64 structs; a misaligned offset would bind a
    // reference to misaligned storage (UB, and a fault on strict-alignment targets), so reject it.
    const uint8_t* sections_bytes = base + hdr->e_shoff;
    if (reinterpret_cast<uintptr_t>(sections_bytes) % alignof(Elf64_Shdr) != 0) { return nothing; }
    const auto* sections = reinterpret_cast<const Elf64_Shdr*>(sections_bytes);
    auto sym_sh =
        ktl::find_if(sections, sections + hdr->e_shnum, [](const Elf64_Shdr& sh) { return sh.sh_type == kShtSymtab; });
    if (!sym_sh) { return nothing; }
    if (sym_sh->sh_entsize < sizeof(Elf64_Sym)) { return nothing; }
    if (!region_in_bounds(sym_sh->sh_offset, sym_sh->sh_size, elf_size)) { return nothing; }
    if (sym_sh->sh_link >= hdr->e_shnum) { return nothing; }

    const uint8_t* syms_bytes = base + sym_sh->sh_offset;
    if (reinterpret_cast<uintptr_t>(syms_bytes) % alignof(Elf64_Sym) != 0) { return nothing; }

    const Elf64_Shdr& str_sh = sections[sym_sh->sh_link];
    if (!region_in_bounds(str_sh.sh_offset, str_sh.sh_size, elf_size)) { return nothing; }

    return symbol_tables{
        .syms        = reinterpret_cast<const Elf64_Sym*>(syms_bytes),
        .count       = static_cast<size_t>(sym_sh->sh_size / sym_sh->sh_entsize),
        .strtab      = reinterpret_cast<const char*>(base + str_sh.sh_offset),
        .strtab_size = static_cast<size_t>(str_sh.sh_size),
    };
}
}  // namespace detail

void init(const void* elf_data, size_t elf_size) {
    if (g_initialized) { return; }

    auto tables = detail::locate_symbol_tables(elf_data, elf_size);
    if (!tables) { return; }

    const char* strtab_end  = tables->strtab + tables->strtab_size;

    // A usable function symbol: function-typed, non-empty extent, in-bounds name.
    auto is_function_symbol = [&](const Elf64_Sym& s) {
        return (s.st_info & 0xf) == detail::kSttFunc && s.st_size != 0 && s.st_name < tables->strtab_size;
    };

    for (const Elf64_Sym& s : ktl::span(tables->syms, tables->count) | ktl::views::filter(is_function_symbol)) {
        if (g_entry_count >= kMaxSymbols) { break; }

        string_view name = bounded_view(tables->strtab + s.st_name, strtab_end);
        if (name.empty()) { continue; }

        auto name_off = intern(name);
        if (!name_off) { break; }

        g_entries[g_entry_count++] = {
            .addr     = static_cast<uintptr_t>(s.st_value),
            .size     = static_cast<uint32_t>(s.st_size),
            .name_off = *name_off,
        };
    }

    sort_entries();
    g_initialized = (g_entry_count > 0);
}

bool available() { return g_initialized; }

maybe<symbol> lookup(uintptr_t addr) {
    if (!g_initialized) { return nothing; }
    return find_entry(addr).map([&](const func_entry& e) { return symbol{&g_string_pool[e.name_off], addr - e.addr}; });
}

}  // namespace kernel::symbols
