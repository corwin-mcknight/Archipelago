#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <stdint.h>

#include <ktl/maybe>
#include <ktl/string_view>

// ppeek/ppoke: raw physical-address read/write through the physmap, for board
// bring-up and hardware experiments from the console. No range checking by
// design -- the operator is trusted, and a bad address can hang the bus.

extern uintptr_t g_hhdm_offset;

namespace {

// Hex parse ("0x" prefix optional); parse_u64 in shell.h is decimal-only.
ktl::maybe<uint64_t> parse_hex(ktl::string_view sv) {
    if (sv.size() >= 2 && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X')) { sv = sv.substr(2); }
    if (sv.size() == 0 || sv.size() > 16) { return ktl::nothing; }
    uint64_t v = 0;
    for (size_t i = 0; i < sv.size(); ++i) {
        char c = sv[i];
        uint64_t d;
        if (c >= '0' && c <= '9') {
            d = static_cast<uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<uint64_t>(c - 'A' + 10);
        } else {
            return ktl::nothing;
        }
        v = (v << 4) | d;
    }
    return v;
}

// Same I/O ordering pattern as the UART driver: fence after reads, before writes.
template <typename T> T mmio_read(uintptr_t paddr) {
    T value = *reinterpret_cast<volatile T*>(g_hhdm_offset + paddr);
#if defined(__riscv)
    asm volatile("fence i,r" ::: "memory");
#endif
    return value;
}

template <typename T> void mmio_write(uintptr_t paddr, T value) {
#if defined(__riscv)
    asm volatile("fence w,o" ::: "memory");
#endif
    *reinterpret_cast<volatile T*>(g_hhdm_offset + paddr) = value;
}

void ppeek_handler(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: ppeek <hex paddr> [word count]\n");
        return;
    }
    auto addr = parse_hex(argv[1]);
    if (!addr.has_value()) {
        output.print("ppeek: bad address\n");
        return;
    }
    uint64_t count = 1;
    if (argc >= 3) {
        auto c = kernel::shell::parse_u64(argv[2]);
        if (!c.has_value() || c.value() == 0 || c.value() > 1024) {
            output.print("ppeek: bad count (1..1024)\n");
            return;
        }
        count = c.value();
    }
    for (uint64_t i = 0; i < count; ++i) {
        uintptr_t a = addr.value() + i * 4;
        if (i % 4 == 0) { output.print("{0:08p}:", a); }
        output.print(" {0:08x}", mmio_read<uint32_t>(a));
        if (i % 4 == 3 || i == count - 1) { output.print("\n"); }
    }
}

void ppoke_handler(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    if (argc < 3) {
        output.print("usage: ppoke <hex paddr> <hex value> [width 1|2|4|8]\n");
        return;
    }
    auto addr  = parse_hex(argv[1]);
    auto value = parse_hex(argv[2]);
    if (!addr.has_value() || !value.has_value()) {
        output.print("ppoke: bad address or value\n");
        return;
    }
    uint64_t width = 4;
    if (argc >= 4) {
        auto w = kernel::shell::parse_u64(argv[3]);
        if (!w.has_value() || (w.value() != 1 && w.value() != 2 && w.value() != 4 && w.value() != 8)) {
            output.print("ppoke: width must be 1, 2, 4, or 8\n");
            return;
        }
        width = w.value();
    }
    switch (width) {
        case 1: mmio_write<uint8_t>(addr.value(), (uint8_t)value.value()); break;
        case 2: mmio_write<uint16_t>(addr.value(), (uint16_t)value.value()); break;
        case 4: mmio_write<uint32_t>(addr.value(), (uint32_t)value.value()); break;
        case 8: mmio_write<uint64_t>(addr.value(), value.value()); break;
        default: return;  // validated above
    }
    output.print("ok\n");
}

}  // namespace

KSHELL_COMMAND(ppeek, "ppeek", "Read physical memory (32-bit words)", ppeek_handler);
KSHELL_COMMAND(ppoke, "ppoke", "Write physical memory", ppoke_handler);

#endif  // CONFIG_KERNEL_SHELL
