#include <kernel/boot.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/platform.h>

extern uintptr_t g_hhdm_offset;

namespace kernel::platform {
namespace {

constexpr uint32_t FDT_MAGIC           = 0xd00dfeed;
constexpr uint32_t FDT_BEGIN_NODE      = 1;
constexpr uint32_t FDT_END_NODE        = 2;
constexpr uint32_t FDT_PROP            = 3;
constexpr uint32_t FDT_NOP             = 4;
constexpr uint32_t FDT_END             = 9;
constexpr uint32_t SUPERVISOR_EXTERNAL = 9;
constexpr uint64_t UART0_PHYSICAL_BASE = 0x10000000;

struct FdtHeader {
    uint32_t magic;
    uint32_t total_size;
    uint32_t off_struct;
    uint32_t off_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_compatible_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_strings;
    uint32_t size_struct;
};

struct NodeState {
    bool is_cpu;
    bool boot_cpu;
    bool parent_boot_cpu;
    bool interrupt_controller;
    bool is_plic;
    bool is_uart;
    uint32_t phandle;
    const void* reg;
    uint32_t reg_length;
    const void* interrupts_extended;
    uint32_t interrupts_extended_length;
    const void* interrupts;
    uint32_t interrupts_length;
    uint32_t source_count;
};

struct PlicState {
    volatile uint32_t* priorities;
    volatile uint32_t* enables;
    volatile uint32_t* threshold;
    volatile uint32_t* claim;
    uint32_t source_count;
    bool ready;
};

PlicState g_plic                 = {};
unsigned int g_uart_interrupt_id = 0;

uint32_t read_be32(const void* value) {
    const auto* bytes = static_cast<const uint8_t*>(value);
    return static_cast<uint32_t>(bytes[0]) << 24 | static_cast<uint32_t>(bytes[1]) << 16 |
           static_cast<uint32_t>(bytes[2]) << 8 | bytes[3];
}

uint64_t read_cells(const void* value, uint32_t cells) {
    uint64_t result   = 0;
    const auto* bytes = static_cast<const uint8_t*>(value);
    for (uint32_t i = 0; i < cells; ++i) { result = result << 32 | read_be32(bytes + i * 4); }
    return result;
}

size_t bounded_length(const char* value, const char* end) {
    const char* cursor = value;
    while (cursor < end && *cursor != '\0') { ++cursor; }
    return static_cast<size_t>(cursor - value);
}

bool name_equals(const char* value, const char* end, const char* wanted) {
    size_t wanted_length = strlen(wanted);
    return bounded_length(value, end) == wanted_length && memcmp(value, wanted, wanted_length) == 0;
}

bool string_list_contains(const void* value, uint32_t length, const char* wanted) {
    const char* cursor = static_cast<const char*>(value);
    const char* end    = cursor + length;
    while (cursor < end) {
        size_t item_length   = bounded_length(cursor, end);
        size_t wanted_length = strlen(wanted);
        if (item_length == wanted_length && memcmp(cursor, wanted, wanted_length) == 0) { return true; }
        if (cursor + item_length == end) { return false; }
        cursor += item_length + 1;
    }
    return false;
}

bool discover(uint64_t boot_hart) {
    const auto* header = static_cast<const FdtHeader*>(boot::collect().dtb);
    if (header == nullptr || read_be32(&header->magic) != FDT_MAGIC) { return false; }
    if (boot_hart == UINT64_MAX) { boot_hart = read_be32(&header->boot_cpuid_phys); }

    uint32_t total_size     = read_be32(&header->total_size);
    uint32_t struct_offset  = read_be32(&header->off_struct);
    uint32_t struct_size    = read_be32(&header->size_struct);
    uint32_t strings_offset = read_be32(&header->off_strings);
    uint32_t strings_size   = read_be32(&header->size_strings);
    if (total_size < sizeof(FdtHeader) || struct_offset > total_size || struct_size > total_size - struct_offset ||
        strings_offset > total_size || strings_size > total_size - strings_offset) {
        return false;
    }

    const auto* blob             = reinterpret_cast<const uint8_t*>(header);
    const uint8_t* cursor        = blob + struct_offset;
    const uint8_t* structure_end = cursor + struct_size;
    const char* strings          = reinterpret_cast<const char*>(blob + strings_offset);
    const char* strings_end      = strings + strings_size;
    NodeState nodes[32]          = {};
    size_t depth                 = 0;
    uint32_t boot_intc_phandle   = 0;
    NodeState plic               = {};
    uint32_t uart_source         = 0;

    while (cursor + 4 <= structure_end) {
        uint32_t token = read_be32(cursor);
        cursor += 4;
        if (token == FDT_BEGIN_NODE) {
            if (depth == 32) { return false; }
            const char* node_name = reinterpret_cast<const char*>(cursor);
            size_t length         = bounded_length(node_name, reinterpret_cast<const char*>(structure_end));
            if (cursor + length == structure_end) { return false; }
            cursor += (length + 4) & ~size_t(3);
            nodes[depth]                 = {};
            nodes[depth].parent_boot_cpu = depth != 0 && nodes[depth - 1].boot_cpu;
            ++depth;
            continue;
        }
        if (token == FDT_END_NODE) {
            if (depth == 0) { return false; }
            NodeState& node = nodes[depth - 1];
            if (node.parent_boot_cpu && node.interrupt_controller && node.phandle != 0) {
                boot_intc_phandle = node.phandle;
            }
            if (node.is_plic) { plic = node; }
            uint32_t address_cells = node.reg_length >= 16 ? 2 : 1;
            if (node.is_uart && node.reg != nullptr && node.reg_length >= address_cells * 4 &&
                read_cells(node.reg, address_cells) == UART0_PHYSICAL_BASE && node.interrupts != nullptr &&
                node.interrupts_length >= 4) {
                uart_source = read_be32(node.interrupts);
            }
            --depth;
            continue;
        }
        if (token == FDT_NOP) { continue; }
        if (token == FDT_END) { break; }
        if (token != FDT_PROP || depth == 0 || cursor + 8 > structure_end) { return false; }

        uint32_t length      = read_be32(cursor);
        uint32_t name_offset = read_be32(cursor + 4);
        cursor += 8;
        if (cursor + length > structure_end || name_offset >= strings_size) { return false; }
        const char* name = strings + name_offset;
        if (bounded_length(name, strings_end) == static_cast<size_t>(strings_end - name)) { return false; }
        NodeState& node = nodes[depth - 1];
        if (name_equals(name, strings_end, "device_type") && string_list_contains(cursor, length, "cpu")) {
            node.is_cpu = true;
        } else if (name_equals(name, strings_end, "reg")) {
            node.reg        = cursor;
            node.reg_length = length;
        } else if (name_equals(name, strings_end, "interrupt-controller")) {
            node.interrupt_controller = true;
        } else if ((name_equals(name, strings_end, "phandle") || name_equals(name, strings_end, "linux,phandle")) &&
                   length == 4) {
            node.phandle = read_be32(cursor);
        } else if (name_equals(name, strings_end, "compatible") &&
                   string_list_contains(cursor, length, "riscv,plic0")) {
            node.is_plic = true;
        } else if (name_equals(name, strings_end, "compatible") &&
                   string_list_contains(cursor, length, "snps,dw-apb-uart")) {
            node.is_uart = true;
        } else if (name_equals(name, strings_end, "interrupts-extended")) {
            node.interrupts_extended        = cursor;
            node.interrupts_extended_length = length;
        } else if (name_equals(name, strings_end, "interrupts")) {
            node.interrupts        = cursor;
            node.interrupts_length = length;
        } else if (name_equals(name, strings_end, "riscv,ndev") && length == 4) {
            node.source_count = read_be32(cursor);
        }
        cursor += (length + 3) & ~size_t(3);

        if (node.is_cpu && node.reg != nullptr && node.reg_length != 0 && node.reg_length <= 8) {
            node.boot_cpu = read_cells(node.reg, node.reg_length / 4) == boot_hart;
        }
    }

    if (boot_intc_phandle == 0 || !plic.is_plic || plic.reg == nullptr || plic.reg_length < 8 ||
        plic.interrupts_extended == nullptr || plic.interrupts_extended_length % 8 != 0) {
        return false;
    }

    uint32_t context       = UINT32_MAX;
    const auto* interrupts = static_cast<const uint8_t*>(plic.interrupts_extended);
    for (uint32_t offset = 0; offset < plic.interrupts_extended_length; offset += 8) {
        if (read_be32(interrupts + offset) == boot_intc_phandle &&
            read_be32(interrupts + offset + 4) == SUPERVISOR_EXTERNAL) {
            context = offset / 8;
            break;
        }
    }
    if (context == UINT32_MAX) { return false; }

    // JH7110 uses two address cells and two size cells for this node. Accept a
    // one-cell synthetic tree as well, which keeps the parser testable in QEMU.
    uint32_t address_cells = plic.reg_length >= 16 ? 2 : 1;
    uint64_t physical_base = read_cells(plic.reg, address_cells);
    uintptr_t virtual_base = g_hhdm_offset + physical_base;
    g_plic.priorities      = reinterpret_cast<volatile uint32_t*>(virtual_base);
    g_plic.enables         = reinterpret_cast<volatile uint32_t*>(virtual_base + 0x2000 + context * 0x80);
    g_plic.threshold       = reinterpret_cast<volatile uint32_t*>(virtual_base + 0x200000 + context * 0x1000);
    g_plic.claim           = g_plic.threshold + 1;
    g_plic.source_count    = plic.source_count != 0 ? plic.source_count : 127;
    if (g_plic.source_count >= IM_MAX_HANDLERS - BOARD_INTERRUPT_BASE) { return false; }
    if (uart_source != 0 && uart_source <= g_plic.source_count) {
        g_uart_interrupt_id = BOARD_INTERRUPT_BASE + uart_source;
    }
    return true;
}

}  // namespace

void interrupt_init() {
    const auto& info = boot::collect();
    uint64_t boot_hart =
        info.cpu_count != 0 && info.boot_cpu_index != SIZE_MAX ? boot::cpu_hw_id(info.boot_cpu_index) : UINT64_MAX;
    if (!discover(boot_hart)) {
        g_log.warn("jh7110: no usable PLIC description in DTB; external interrupts disabled");
        return;
    }
    for (uint32_t source = 1; source <= g_plic.source_count; ++source) {
        g_plic.priorities[source] = 1;
        g_plic.enables[source / 32] &= ~(1u << (source % 32));
    }
    *g_plic.threshold = 0;
    asm volatile("csrs sie, %0" : : "r"(1ull << SUPERVISOR_EXTERNAL));
    g_plic.ready = true;
    g_log.info("jh7110: PLIC ready ({0} sources)", g_plic.source_count);
}

void interrupt_set_source_enabled(unsigned int id, bool enabled) {
    if (!g_plic.ready || id <= BOARD_INTERRUPT_BASE) { return; }
    unsigned int source = id - BOARD_INTERRUPT_BASE;
    if (source > g_plic.source_count) { return; }
    volatile uint32_t& word = g_plic.enables[source / 32];
    uint32_t mask           = 1u << (source % 32);
    word                    = enabled ? word | mask : word & ~mask;
}

unsigned int console_uart_interrupt_id() { return g_uart_interrupt_id; }

bool dispatch_external_interrupt(::register_frame* regs) {
    if (!g_plic.ready) { return false; }
    uint32_t source = *g_plic.claim;
    if (source == 0) { return true; }
    g_interrupt_manager.dispatch_interrupt(BOARD_INTERRUPT_BASE + source, regs);
    *g_plic.claim = source;
    return true;
}

}  // namespace kernel::platform
