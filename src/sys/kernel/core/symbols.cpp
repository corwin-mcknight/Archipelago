#include "kernel/symbols.h"

#include <stddef.h>
#include <stdint.h>

namespace kernel::symbols {

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

constexpr uint32_t kShtSymtab = 2;
constexpr uint8_t kSttFunc    = 2;

constexpr uint8_t kElfMag0    = 0x7f;
constexpr uint8_t kElfMag1    = 'E';
constexpr uint8_t kElfMag2    = 'L';
constexpr uint8_t kElfMag3    = 'F';
constexpr uint8_t kElfClass64 = 2;

uint8_t st_type(uint8_t info) { return info & 0xf; }

size_t str_len(const char* s, const char* end) {
    size_t n = 0;
    while (s + n < end && s[n] != '\0') { n++; }
    return n;
}

void mem_copy(char* dst, const char* src, size_t n) {
    for (size_t i = 0; i < n; i++) { dst[i] = src[i]; }
}

bool intern_string(const char* s, const char* str_end, uint32_t& out_off) {
    size_t len = str_len(s, str_end);
    if (g_string_used + len + 1 > kStringPoolBytes) { return false; }
    out_off = static_cast<uint32_t>(g_string_used);
    mem_copy(&g_string_pool[g_string_used], s, len);
    g_string_pool[g_string_used + len] = '\0';
    g_string_used += len + 1;
    return true;
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

}  // namespace

void init(const void* elf_data, size_t elf_size) {
    if (g_initialized) { return; }
    if (elf_data == nullptr || elf_size < sizeof(Elf64_Ehdr)) { return; }

    const auto* base = static_cast<const uint8_t*>(elf_data);
    const auto* hdr  = reinterpret_cast<const Elf64_Ehdr*>(base);

    if (hdr->e_ident[0] != kElfMag0 || hdr->e_ident[1] != kElfMag1 || hdr->e_ident[2] != kElfMag2 ||
        hdr->e_ident[3] != kElfMag3) {
        return;
    }
    if (hdr->e_ident[4] != kElfClass64) { return; }
    if (hdr->e_shoff == 0 || hdr->e_shentsize < sizeof(Elf64_Shdr)) { return; }
    if (hdr->e_shoff + static_cast<uint64_t>(hdr->e_shnum) * hdr->e_shentsize > elf_size) { return; }

    const auto* sections     = reinterpret_cast<const Elf64_Shdr*>(base + hdr->e_shoff);

    // Locate .symtab (and via its sh_link, the associated string table).
    const Elf64_Shdr* sym_sh = nullptr;
    for (uint16_t i = 0; i < hdr->e_shnum; i++) {
        if (sections[i].sh_type == kShtSymtab) {
            sym_sh = &sections[i];
            break;
        }
    }
    if (sym_sh == nullptr) { return; }
    if (sym_sh->sh_entsize < sizeof(Elf64_Sym)) { return; }
    if (sym_sh->sh_offset + sym_sh->sh_size > elf_size) { return; }
    if (sym_sh->sh_link >= hdr->e_shnum) { return; }

    const Elf64_Shdr& str_sh = sections[sym_sh->sh_link];
    if (str_sh.sh_offset + str_sh.sh_size > elf_size) { return; }

    const auto* syms       = reinterpret_cast<const Elf64_Sym*>(base + sym_sh->sh_offset);
    const size_t sym_count = static_cast<size_t>(sym_sh->sh_size / sym_sh->sh_entsize);
    const char* strtab     = reinterpret_cast<const char*>(base + str_sh.sh_offset);
    const char* strtab_end = strtab + str_sh.sh_size;

    for (size_t i = 0; i < sym_count && g_entry_count < kMaxSymbols; i++) {
        const Elf64_Sym& s = syms[i];
        if (st_type(s.st_info) != kSttFunc) { continue; }
        if (s.st_size == 0) { continue; }
        if (s.st_name >= str_sh.sh_size) { continue; }

        const char* name = strtab + s.st_name;
        if (*name == '\0') { continue; }

        uint32_t name_off;
        if (!intern_string(name, strtab_end, name_off)) { break; }

        g_entries[g_entry_count++] = {
            .addr     = static_cast<uintptr_t>(s.st_value),
            .size     = static_cast<uint32_t>(s.st_size),
            .name_off = name_off,
        };
    }

    sort_entries();
    g_initialized = (g_entry_count > 0);
}

bool available() { return g_initialized; }

bool demangle(const char* mangled, char* out, size_t out_size) {
    if (mangled == nullptr || out == nullptr || out_size < 4) { return false; }
    if (mangled[0] != '_' || mangled[1] != 'Z') { return false; }

    const char* p = mangled + 2;
    size_t pos    = 0;

    auto put_char = [&](char c) -> bool {
        if (pos + 1 >= out_size) { return false; }
        out[pos++] = c;
        return true;
    };
    auto put_str = [&](const char* s) -> bool {
        while (*s != '\0') {
            if (!put_char(*s++)) { return false; }
        }
        return true;
    };
    auto put_n = [&](const char* s, size_t n) -> bool {
        for (size_t i = 0; i < n; i++) {
            if (!put_char(s[i])) { return false; }
        }
        return true;
    };

    bool nested = false;
    if (*p == 'N') {
        nested = true;
        p++;
    }

    bool first = true;
    while (true) {
        if (nested && *p == 'E') { break; }
        if (!nested && !first) { break; }
        if (*p < '0' || *p > '9') { return false; }

        size_t len = 0;
        while (*p >= '0' && *p <= '9') {
            len = len * 10 + static_cast<size_t>(*p - '0');
            p++;
        }
        if (len == 0) { return false; }

        if (!first) {
            if (!put_str("::")) { return false; }
        }
        first = false;

        static constexpr char kAnonTag[] = "_GLOBAL__N_1";
        constexpr size_t kAnonTagLen     = sizeof(kAnonTag) - 1;
        bool is_anon                     = (len == kAnonTagLen);
        for (size_t i = 0; is_anon && i < kAnonTagLen; i++) {
            if (p[i] != kAnonTag[i]) { is_anon = false; }
        }

        if (is_anon) {
            if (!put_str("(anonymous namespace)")) { return false; }
        } else {
            if (!put_n(p, len)) { return false; }
        }
        p += len;
    }

    if (!put_str("()")) { return false; }
    out[pos] = '\0';
    return true;
}

const char* lookup(uintptr_t addr, size_t* offset_out) {
    if (!g_initialized) { return nullptr; }

    // Binary search for the greatest entry with addr <= target.
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
    if (lo == 0) { return nullptr; }

    const func_entry& e = g_entries[lo - 1];
    if (addr >= e.addr + e.size) { return nullptr; }

    if (offset_out != nullptr) { *offset_out = addr - e.addr; }
    return &g_string_pool[e.name_off];
}

}  // namespace kernel::symbols
