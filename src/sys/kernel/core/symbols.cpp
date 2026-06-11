#include "kernel/symbols.h"

#include <stddef.h>
#include <stdint.h>

#include <ktl/algorithm>
#include <ktl/maybe>
#include <ktl/string_view>

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

// Bounded output buffer for the demangler: writes silently stop once the buffer
// is full, and `finish()` reports whether the whole name fit. This collapses the
// repeated `if (!put(...)) return false;` checks into a single trailing test.
struct sym_writer {
    char* out;
    size_t cap;
    size_t pos = 0;
    bool ok    = true;

    void put(char c) {
        if (pos + 1 >= cap) {
            ok = false;
            return;
        }
        out[pos++] = c;
    }

    void put(string_view s) {
        for (size_t i = 0; i < s.size(); i++) { put(s[i]); }
    }

    bool finish() {
        if (ok) { out[pos] = '\0'; }
        return ok;
    }
};

// The validated symbol/string tables init() ingests, isolating the ELF parsing
// from the ingestion loop.
struct symbol_tables {
    const Elf64_Sym* syms;
    size_t count;
    const char* strtab;
    size_t strtab_size;
};

// Validate the ELF header, then locate .symtab and its associated string table.
// Every malformed or out-of-bounds field short-circuits to nothing.
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

    const auto* sections = reinterpret_cast<const Elf64_Shdr*>(base + hdr->e_shoff);
    auto sym_sh =
        ktl::find_if(sections, sections + hdr->e_shnum, [](const Elf64_Shdr& sh) { return sh.sh_type == kShtSymtab; });
    if (!sym_sh) { return nothing; }
    if (sym_sh->sh_entsize < sizeof(Elf64_Sym)) { return nothing; }
    if (!region_in_bounds(sym_sh->sh_offset, sym_sh->sh_size, elf_size)) { return nothing; }
    if (sym_sh->sh_link >= hdr->e_shnum) { return nothing; }

    const Elf64_Shdr& str_sh = sections[sym_sh->sh_link];
    if (!region_in_bounds(str_sh.sh_offset, str_sh.sh_size, elf_size)) { return nothing; }

    return symbol_tables{
        .syms        = reinterpret_cast<const Elf64_Sym*>(base + sym_sh->sh_offset),
        .count       = static_cast<size_t>(sym_sh->sh_size / sym_sh->sh_entsize),
        .strtab      = reinterpret_cast<const char*>(base + str_sh.sh_offset),
        .strtab_size = static_cast<size_t>(str_sh.sh_size),
    };
}

// Parse one Itanium `<decimal-length><identifier>` source-name, advancing `p`
// past it. Returns nothing on a missing or zero-length prefix.
maybe<string_view> read_source_name(const char*& p) {
    if (*p < '0' || *p > '9') { return nothing; }

    size_t len = 0;
    while (*p >= '0' && *p <= '9') {
        len = len * 10 + static_cast<size_t>(*p - '0');
        p++;
    }
    if (len == 0) { return nothing; }

    string_view name(p, len);
    p += len;
    return name;
}

}  // namespace

void init(const void* elf_data, size_t elf_size) {
    if (g_initialized) { return; }

    auto tables = locate_symbol_tables(elf_data, elf_size);
    if (!tables) { return; }

    const char* strtab_end = tables->strtab + tables->strtab_size;
    for (size_t i = 0; i < tables->count && g_entry_count < kMaxSymbols; i++) {
        const Elf64_Sym& s = tables->syms[i];
        if ((s.st_info & 0xf) != kSttFunc) { continue; }
        if (s.st_size == 0) { continue; }
        if (s.st_name >= tables->strtab_size) { continue; }

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

bool demangle(const char* mangled, char* out, size_t out_size) {
    if (mangled == nullptr || out == nullptr || out_size < 4) { return false; }
    if (!string_view(mangled).starts_with("_Z")) { return false; }

    const char* p = mangled + 2;
    sym_writer w{out, out_size};

    bool nested = (*p == 'N');
    if (nested) { p++; }

    bool first = true;
    while (true) {
        if (nested && *p == 'E') { break; }
        if (!nested && !first) { break; }

        auto component = read_source_name(p);
        if (!component) { return false; }

        if (!first) { w.put(string_view("::")); }
        first = false;

        w.put(*component == "_GLOBAL__N_1" ? string_view("(anonymous namespace)") : *component);
    }

    w.put(string_view("()"));
    return w.finish();
}

maybe<symbol> lookup(uintptr_t addr) {
    if (!g_initialized) { return nothing; }
    return find_entry(addr).map([&](const func_entry& e) { return symbol{&g_string_pool[e.name_off], addr - e.addr}; });
}

}  // namespace kernel::symbols
