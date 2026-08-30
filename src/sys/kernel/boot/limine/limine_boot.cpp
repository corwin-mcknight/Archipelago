#include <kernel/boot.h>

#include "kernel/assert.h"
#include "kernel/log.h"
#include "kernel/platform.h"
#include "vendor/limine.h"

// The Limine boot protocol behind kernel::boot::collect(). Everything that knows
// a Limine type lives here; the rest of the kernel sees only boot_info and the
// cpu_hw_id()/start_cpu() accessors.

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

__attribute__((used, section(".limine_requests"))) static volatile struct limine_module_request module_request = {
    .id                    = LIMINE_MODULE_REQUEST,
    .revision              = 0,
    .response              = nullptr,
    .internal_module_count = 0,
    .internal_modules      = nullptr};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST, .revision = 0, .response = nullptr, .flags = 0};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_dtb_request dtb_request = {
    .id = LIMINE_DTB_REQUEST, .revision = 0, .response = nullptr};

__attribute__((used,
               section(".limine_requests"))) static volatile struct limine_date_at_boot_request date_at_boot_request = {
    .id = LIMINE_DATE_AT_BOOT_REQUEST, .revision = 0, .response = nullptr};

__attribute__((used,
               section(".limine_requests"))) static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0, .response = nullptr};

#if defined(__riscv)
// Pin the paging mode: the riscv64 paging code assumes a 3-level Sv39 walk and
// a mode-8 satp, so Sv48/Sv57 would silently corrupt every table access. Sv39
// is universal (the JH7110's U74 cores implement nothing deeper), so QEMU and
// real boards boot with identical translation.
__attribute__((used,
               section(".limine_requests"))) static volatile struct limine_paging_mode_request paging_mode_request = {
    .id       = LIMINE_PAGING_MODE_REQUEST,
    .revision = 0,
    .response = nullptr,
    .mode     = LIMINE_PAGING_MODE_RISCV_SV39,
    .max_mode = LIMINE_PAGING_MODE_RISCV_SV39,
    .min_mode = LIMINE_PAGING_MODE_RISCV_SV39};
#endif

namespace kernel::boot {

namespace {

// The translated map lives in a fixed array rather than costing an allocator this
// early in boot. Adjacent same-kind entries are coalesced during translation, which
// collapses the fragmented 100+-entry maps real UEFI firmware produces to a handful
// of ranges; overflow past the coalesced cap only loses coverage of the tail, so it
// warns rather than panicking.
constexpr size_t MAX_MEMORY_RANGES = 128;

// Modules are named in the boot configuration, so the count is bounded by what a human wrote.
// Overflow drops the tail with a warning rather than panicking, matching the memmap.
constexpr size_t MAX_MODULES       = 8;

memory_range g_ranges[MAX_MEMORY_RANGES];
boot_module g_modules[MAX_MODULES];
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

size_t g_range_count = 0;

void push_range(uint64_t base, uint64_t length, memory_kind kind) {
    if (length > UINT64_MAX - base) {
        g_log.warn("boot: ignoring wrapping memmap range base=0x{0:p} length=0x{1:p}", base, length);
        return;
    }
    // Limine reports the memmap sorted by base, so contiguous same-kind entries merge
    // into the previous range instead of costing an array slot.
    if (g_range_count > 0 && g_ranges[g_range_count - 1].kind == kind && base >= g_ranges[g_range_count - 1].base &&
        g_ranges[g_range_count - 1].length == base - g_ranges[g_range_count - 1].base) {
        g_ranges[g_range_count - 1].length += length;
        return;
    }
    if (g_range_count == MAX_MEMORY_RANGES) {
        g_log.warn("boot: memmap exceeds {0} coalesced entries; ignoring the remainder", MAX_MEMORY_RANGES);
        return;
    }
    g_ranges[g_range_count++] = {.base = base, .length = length, .kind = kind};
}

void translate_memmap() {
    if (memmap_request.response == nullptr) { return; }

    // Firmware can fence off DRAM it never offers to anyone (the JH7110's U-Boot and
    // everything above its ram_top clamp), which reaches here as reclaimable. The part
    // above the board's fence holds nothing of Limine's, so it goes straight to the
    // page pool; the part below keeps the usual hands-off treatment.
    uint64_t fence = platform::firmware_fenced_memory_base();
    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        const auto* entry = memmap_request.response->entries[i];
        if (entry == nullptr || entry->length > UINT64_MAX - entry->base) {
            if (entry != nullptr) {
                g_log.warn("boot: ignoring wrapping memmap range base=0x{0:p} length=0x{1:p}", entry->base,
                           entry->length);
            }
            continue;
        }
        uint64_t end = entry->base + entry->length;
        if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE && fence != 0 && end > fence) {
            uint64_t split = entry->base > fence ? entry->base : fence;
            if (split > entry->base) { push_range(entry->base, split - entry->base, memory_kind::OTHER); }
            push_range(split, end - split, memory_kind::USABLE);
            g_log.info("boot: reclaiming {0} MiB of firmware-fenced memory at 0x{1:p}", (end - split) >> 20, split);
            continue;
        }
        push_range(entry->base, entry->length, classify(entry->type));
    }

    g_info.memory_map       = g_ranges;
    g_info.memory_map_count = g_range_count;
}

// Limine's module strings and paths live in bootloader-reclaimable memory, which classify() sends
// to memory_kind::OTHER and never hands the PMM -- so the pointers stay valid and are stored rather
// than copied, exactly as the command line is.
void translate_modules() {
    if (module_request.response == nullptr) { return; }

    size_t count = 0;
    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        const auto* file = module_request.response->modules[i];
        if (file == nullptr || file->address == nullptr) { continue; }
        if (count == MAX_MODULES) {
            g_log.warn("boot: more than {0} modules; ignoring the remainder", MAX_MODULES);
            break;
        }
        // An untagged module has no role to answer to, so it is carried with an empty role and
        // simply never matches a lookup.
        g_modules[count++] = {
            .role = file->string != nullptr ? file->string : "",
            .data = file->address,
            .size = static_cast<size_t>(file->size),
        };
    }

    g_info.modules      = g_modules;
    g_info.module_count = count;
}

// The MP info struct names its hardware id per architecture (lapic_id, hartid).
uint64_t hw_id_of(const struct limine_mp_info* cpu) {
#if defined(__x86_64__)
    return cpu->lapic_id;
#elif defined(__riscv)
    return cpu->hartid;
#endif
}

uint64_t boot_hw_id(const struct limine_mp_response* mp) {
#if defined(__x86_64__)
    return mp->bsp_lapic_id;
#elif defined(__riscv)
    return mp->bsp_hartid;
#endif
}

// All secondary CPUs share one kernel entry function; stashing it in a global
// (published before the goto_address release below) keeps limine_mp_info out of
// the arch-facing signature.
void (*g_secondary_entry)(size_t core_index, uint64_t hw_id);

void mp_trampoline(struct limine_mp_info* info) {
    // extra_argument carries the dense CPU-list index published by start_cpu()
    // before this CPU was released. entry never returns.
    g_secondary_entry((size_t)info->extra_argument, hw_id_of(info));
}

}  // namespace

const boot_info& collect() {
    if (g_info_cached) { return g_info; }
    g_info_cached = true;

    if (hhdm_request.response != nullptr) { g_info.physmap_base = hhdm_request.response->offset; }

    translate_memmap();
    translate_modules();

    if (executable_file_request.response != nullptr && executable_file_request.response->executable_file != nullptr) {
        g_info.kernel_elf      = executable_file_request.response->executable_file->address;
        g_info.kernel_elf_size = executable_file_request.response->executable_file->size;
    }

    if (executable_cmdline_request.response != nullptr) {
        g_info.cmdline = executable_cmdline_request.response->cmdline;
    }

    if (dtb_request.response != nullptr) { g_info.dtb = dtb_request.response->dtb_ptr; }

    if (date_at_boot_request.response != nullptr) {
        g_info.boot_epoch_seconds = date_at_boot_request.response->timestamp;
    }

    if (framebuffer_request.response != nullptr && framebuffer_request.response->framebuffer_count > 0) {
        const auto* fb = framebuffer_request.response->framebuffers[0];
        // Some firmware publishes a placeholder framebuffer when no display is
        // connected. Do not turn that into a present framebuffer: consumers use
        // a non-null address as the availability signal.
        if (fb != nullptr && fb->address != nullptr && fb->width != 0 && fb->height != 0 && fb->pitch != 0 &&
            fb->bpp != 0) {
            g_info.framebuffer    = fb->address;
            g_info.fb_width       = fb->width;
            g_info.fb_height      = fb->height;
            g_info.fb_pitch       = fb->pitch;
            g_info.fb_bpp         = fb->bpp;
            g_info.fb_red_shift   = fb->red_mask_shift;
            g_info.fb_green_shift = fb->green_mask_shift;
            g_info.fb_blue_shift  = fb->blue_mask_shift;
        }
    }

#if defined(__riscv)
    g_info.paging_mode_ok =
        paging_mode_request.response != nullptr && paging_mode_request.response->mode == LIMINE_PAGING_MODE_RISCV_SV39;
#else
    // x86_64 makes no paging-mode request: long mode is always the 4-level walk
    // the paging code assumes (5-level must be opted into, and never is).
    g_info.paging_mode_ok = true;
#endif

    if (mp_request.response != nullptr) {
        const auto* mp        = mp_request.response;
        g_info.cpu_count      = mp->cpu_count;
        g_info.boot_cpu_index = SIZE_MAX;
        for (uint64_t i = 0; i < mp->cpu_count; i++) {
            if (hw_id_of(mp->cpus[i]) == boot_hw_id(mp)) {
                g_info.boot_cpu_index = i;
                break;
            }
        }
    }

    return g_info;
}

uint64_t cpu_hw_id(size_t index) {
    assert(mp_request.response != nullptr && index < mp_request.response->cpu_count,
           "cpu_hw_id: index outside the boot protocol CPU list");
    return hw_id_of(mp_request.response->cpus[index]);
}

void start_cpu(size_t index, void (*entry)(size_t core_index, uint64_t hw_id)) {
    assert(mp_request.response != nullptr && index < mp_request.response->cpu_count,
           "start_cpu: index outside the boot protocol CPU list");
    g_secondary_entry   = entry;
    auto* cpu           = mp_request.response->cpus[index];
    // Publish this CPU's dense index before releasing it; the SEQ_CST store of
    // goto_address is the release that makes it (and g_secondary_entry) visible.
    cpu->extra_argument = index;
    __atomic_store_n(reinterpret_cast<void**>(&cpu->goto_address), (void*)mp_trampoline, __ATOMIC_SEQ_CST);
}

}  // namespace kernel::boot
