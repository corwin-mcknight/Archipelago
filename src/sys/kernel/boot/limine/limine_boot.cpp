#include <kernel/boot.h>

#include "kernel/log.h"
#include "vendor/limine.h"

// The Limine boot protocol behind kernel::boot::collect(). Everything that knows
// a Limine type lives here; core/boot.cpp sees only boot_info. Arch-specific
// requests (x86_64 MP, riscv64 paging mode) stay in their arch's main.cpp because
// they gate arch bring-up, not the arch-neutral boot path.

__attribute__((used, section(".limine_requests_start"))) static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end"))) static volatile LIMINE_REQUESTS_END_MARKER;

__attribute__((used, section(".limine_requests"))) static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0, .response = nullptr};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0, .response = nullptr};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_executable_file_request executable_file_request = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST, .revision = 0, .response = nullptr};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_executable_cmdline_request
    executable_cmdline_request = {.id = LIMINE_EXECUTABLE_CMDLINE_REQUEST, .revision = 0, .response = nullptr};

namespace kernel::boot {

namespace {

// Limine memmaps are small (tens of entries on any real machine), so the translated
// map lives in a fixed array rather than costing an allocator this early in boot.
// Overflow only loses coverage of the tail, so it warns rather than panicking.
constexpr size_t MAX_MEMORY_RANGES = 64;

memory_range g_ranges[MAX_MEMORY_RANGES];
boot_info g_info   = {};
bool g_info_cached = false;

memory_kind classify(uint64_t limine_type) {
    switch (limine_type) {
        case LIMINE_MEMMAP_USABLE: return memory_kind::USABLE;
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return memory_kind::KERNEL;
        // RECLAIMABLE regions still hold live Limine responses (memmap, cmdline) that
        // boot code reads after collect() returns, so they are never handed to the PMM.
        // Leaks ~20MB of reclaimable memory; copy-out-then-reclaim when it matters.
        default: return memory_kind::OTHER;
    }
}

void translate_memmap() {
    if (memmap_request.response == nullptr) { return; }

    size_t count = 0;
    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        if (count == MAX_MEMORY_RANGES) {
            g_log.warn("boot: memmap has more than {0} entries; ignoring the remainder", MAX_MEMORY_RANGES);
            break;
        }
        const auto* entry = memmap_request.response->entries[i];
        g_ranges[count++] = {.base = entry->base, .length = entry->length, .kind = classify(entry->type)};
    }

    g_info.memory_map       = g_ranges;
    g_info.memory_map_count = count;
}

}  // namespace

const boot_info& collect() {
    if (g_info_cached) { return g_info; }
    g_info_cached = true;

    if (hhdm_request.response != nullptr) { g_info.physmap_base = hhdm_request.response->offset; }

    translate_memmap();

    if (executable_file_request.response != nullptr && executable_file_request.response->executable_file != nullptr) {
        g_info.kernel_elf      = executable_file_request.response->executable_file->address;
        g_info.kernel_elf_size = executable_file_request.response->executable_file->size;
    }

    if (executable_cmdline_request.response != nullptr) {
        g_info.cmdline = executable_cmdline_request.response->cmdline;
    }

    return g_info;
}

}  // namespace kernel::boot
