#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/console.h>
#include <kernel/obj/handle_table.h>
#include <kernel/platform.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/user_task.h>
#include <kernel/shell/shell.h>
#include <kernel/time.h>

#include <ktl/maybe>
#include <ktl/string_view>

#include "kernel/config.h"
#include "kernel/drivers/uart.h"
#include "kernel/log.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/slab_heap.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/panic.h"
#include "kernel/symbols.h"

kernel::driver::uart uart;

uintptr_t g_hhdm_offset = 0;

namespace kernel::boot {

namespace {

ktl::string_view kind_name(memory_kind kind) {
    switch (kind) {
        case memory_kind::USABLE: return "usable";
        case memory_kind::KERNEL: return "kernel";
        case memory_kind::OTHER: return "other";
        default: return "?";
    }
}

// Returns true if the space-delimited command line contains an exact-match token.
bool cmdline_has_token(ktl::string_view cmdline, ktl::string_view token) {
    size_t start = 0;
    while (start <= cmdline.size()) {
        size_t space = cmdline.find(' ', start);
        size_t end   = (space == ktl::string_view::npos) ? cmdline.size() : space;
        if (cmdline.substr(start, end - start) == token) { return true; }
        if (space == ktl::string_view::npos) { break; }
        start = space + 1;
    }
    return false;
}

// The three boot modes: hand the machine to the operator and hold the boot sequence (SHELL),
// run both -- shell prompt with the boot sequence proceeding underneath (SHELL_AND_BOOT), or
// boot straight through with no shell (BOOT).
enum class boot_mode : uint8_t { BOOT, SHELL, SHELL_AND_BOOT };

// A plain boot is the absence of a request: the shell is entered only when the command line asks
// for it with "shell" (hold the boot sequence) or "shell+boot" (boot with the prompt alongside),
// and only when it was compiled in (CONFIG_KERNEL_SHELL).
boot_mode resolve_boot_mode() {
    if (collect().cmdline == nullptr) { return boot_mode::BOOT; }
    ktl::string_view cmdline(collect().cmdline);
    g_log.info("boot: command line: \"{0}\"", cmdline);
    if (cmdline_has_token(cmdline, "shell+boot")) {
        g_log.info("boot: command line requested shell with background boot (shell+boot)");
        return boot_mode::SHELL_AND_BOOT;
    }
    if (cmdline_has_token(cmdline, "shell")) {
        g_log.info("boot: command line requested kernel shell boot (shell)");
        return boot_mode::SHELL;
    }
    return boot_mode::BOOT;
}

}  // namespace

const boot_module* find_module(const char* role) {
    if (role == nullptr) { return nullptr; }
    const boot_info& info = collect();
    for (size_t i = 0; i < info.module_count; i++) {
        if (ktl::string_view(info.modules[i].role) == ktl::string_view(role)) { return &info.modules[i]; }
    }
    return nullptr;
}

void resolve_hhdm() {
    g_hhdm_offset = collect().physmap_base;
    if (g_hhdm_offset == 0) { panic("bootloader supplied no physical map base -- direct map unavailable"); }
}

void snapshot_symbols() {
    // Snapshot the kernel ELF's symbol table before PMM reclaims bootloader memory.
    const boot_info& info = collect();
    if (info.kernel_elf == nullptr) {
        g_log.warn("symbols: bootloader did not supply the kernel image");
        return;
    }
    kernel::symbols::init(info.kernel_elf, info.kernel_elf_size);
    if (kernel::symbols::available()) {
        g_log.info("symbols: kernel symbol table loaded");
    } else {
        g_log.warn("symbols: kernel symbol table unavailable");
    }
}

void init_memory() {
    const boot_info& info = collect();
    if (info.memory_map_count == 0) { panic("bootloader reported no memory map"); }

    // Range lists for VMM init: usable ranges become FREE page descriptors,
    // kernel ranges stay WIRED. Fixed capacity -- boot memory maps are small;
    // overflow only costs descriptor precision, so warn and drop.
    constexpr size_t MAX_MEMMAP_RANGES = 48;
    kernel::mm::vm_page_region usable_ranges[MAX_MEMMAP_RANGES];
    kernel::mm::vm_page_region wired_ranges[MAX_MEMMAP_RANGES];
    size_t usable_range_count   = 0;
    size_t wired_range_count    = 0;

    uint64_t total_usable_pages = 0;
    for (size_t i = 0; i < info.memory_map_count; i++) {
        const memory_range& entry = info.memory_map[i];
        // Only USABLE and KERNEL entries feed the PMM/VMM; validating (and warning about) ranges
        // the kernel never consumes would just be noise.
        if (entry.kind != memory_kind::USABLE && entry.kind != memory_kind::KERNEL) { continue; }

        // A malformed entry must not corrupt the PMM: a misaligned base or non-page-multiple length
        // would hand the allocator a partial frame. Skip such regions with a warning rather than
        // truncating silently.
        if ((entry.base & (KERNEL_MINIMUM_PAGE_SIZE - 1)) != 0 ||
            (entry.length & (KERNEL_MINIMUM_PAGE_SIZE - 1)) != 0) {
            g_log.warn("pmm: skipping misaligned region base=0x{0:p} length=0x{1:p} kind={2}", entry.base, entry.length,
                       kind_name(entry.kind));
            continue;
        }
        // A range whose end wraps the address space would make every derived extent computation
        // (PMM region tails, descriptor coverage) wrap with it. Bootloader data is still input.
        if (entry.base + entry.length < entry.base) {
            g_log.warn("pmm: skipping wrapping region base=0x{0:p} length=0x{1:p}", entry.base, entry.length);
            continue;
        }
        size_t pages = entry.length / KERNEL_MINIMUM_PAGE_SIZE;

        if (entry.kind == memory_kind::USABLE) {
            if (pages == 0) {
                g_log.warn("pmm: skipping empty usable region base=0x{0:p}", entry.base);
                continue;
            }
            // A range the descriptor table cannot cover must not reach the PMM either: the heap
            // and VMM record per-frame truth in descriptors, so an allocatable-but-uncovered
            // frame would panic the first time something looks its descriptor up. Losing the
            // memory is the safe direction.
            if (usable_range_count == MAX_MEMMAP_RANGES) {
                g_log.warn("pmm: dropping usable range base=0x{0:p} pages={1} (descriptor range cap)", entry.base,
                           pages);
                continue;
            }
            g_log.info("pmm: adding region base=0x{0:p} pages={1}", entry.base, pages);
            kernel::mm::g_page_frame_allocator.add_region({.start = entry.base, .count = pages});
            total_usable_pages += pages;
            usable_ranges[usable_range_count++] = {.start = entry.base, .count = pages};
        } else if (entry.kind == memory_kind::KERNEL) {
            g_log.info("pmm: reserved region base=0x{0:p} pages={1} (kernel)", entry.base, pages);
            kernel::mm::g_page_frame_allocator.add_reserved(pages);
            if (wired_range_count < MAX_MEMMAP_RANGES) {
                wired_ranges[wired_range_count++] = {.start = entry.base, .count = pages};
            } else {
                g_log.warn("vmm: dropping kernel range base=0x{0:p} from descriptor coverage", entry.base);
            }
        }
    }
    if (total_usable_pages == 0) { panic("bootloader reported no usable memory"); }
    g_log.info("Memory subsystem initialized ({0} usable pages)", total_usable_pages);

    kernel::mm::vmm_init(usable_ranges, usable_range_count, wired_ranges, wired_range_count);

    kernel::mm::heap_activate();
    g_log.info("Slab heap active; early heap serves boot-lifetime allocations only");
}

bool continue_boot() {
    // Interrupts-off makes the test-and-set atomic on the single scheduling core, so a `boot
    // continue` racing late_boot cannot launch two coordinators.
    static bool s_continued = false;
    uint64_t flags          = kernel::arch::save_and_disable_interrupts();
    bool first              = !s_continued;
    s_continued             = true;
    kernel::arch::restore_interrupts(flags);
    if (!first) { return false; }

    auto launched = kernel::sched::launch_coordinator();
    if (launched.is_err()) {
        g_log.error("boot: coordinator launch failed");
    } else {
        g_log.info("boot: coordinator running (task id={0})", launched.unwrap()->id());
    }
    return true;
}

#if CONFIG_KERNEL_SHELL
static void shell_thread_main(void*) { kernel::shell::shell_main(); }
#endif

// Keeps the PMM's zeroed page supply topped up so alloc() skips its inline
// memset. Paced in small batches so the initial climb to the pre-zero target
// trickles out over tens of seconds instead of monopolizing the CPU.
[[noreturn]] static void zeroer_thread_main(void*) {
    constexpr size_t BATCH_PAGES    = 16;
    constexpr uint64_t PERIOD_TICKS = 50;  // 1 tick = 1 ms
    while (true) {
        for (size_t i = 0; i < BATCH_PAGES; ++i) {
            if (!kernel::mm::g_page_frame_allocator.zero_one_page()) { break; }
        }
        kernel::sched::sleep_ticks(PERIOD_TICKS);
    }
}

[[noreturn]] void late_boot(uint32_t boot_core_index) {
    if (const boot_info& info = collect(); info.framebuffer != nullptr) {
        g_log.info("fb: {0}x{1} bpp={2} pitch={3} at 0x{4:p}", info.fb_width, info.fb_height, info.fb_bpp,
                   info.fb_pitch, (uintptr_t)info.framebuffer);
    }

    kernel::obj::obj_init();
    g_log.info("Object subsystem initialized");

    kernel::platform::timestamp_calibrate();
    kernel::time::use_timestamp_clock();
    kernel::sched::init(boot_core_index);

    // The scheduler is up, so the framebuffer console can spawn its painter thread; from here
    // the log and the shell appear on the panel as well as the UART.
    kernel::console::init(collect());

    kernel::sched::spawn("zeroer", zeroer_thread_main, nullptr).expect("boot: zeroer spawn failed");

    kernel::platform::watchdog_init();

    boot_mode mode      = resolve_boot_mode();
    bool entering_shell = false;
#if CONFIG_KERNEL_SHELL
    if (mode != boot_mode::BOOT) {
        g_log.info("boot: starting kernel shell thread");
        kernel::sched::spawn("kshell", shell_thread_main, nullptr).expect("boot: shell spawn failed");
        entering_shell = true;
    }
#else
    if (mode != boot_mode::BOOT) { g_log.warn("boot: shell requested but not compiled in (CONFIG_KERNEL_SHELL=0)"); }
#endif

    // A SHELL boot holds the sequence here for the operator, who resumes it with `boot continue`
    // -- also what keeps test-harness boots quiet. The other modes (and a shell request the build
    // cannot honor) continue immediately, with the shell prompt live underneath in SHELL_AND_BOOT.
    if (mode != boot_mode::SHELL || !entering_shell) { continue_boot(); }
    g_log.info("boot: initialization complete");

    kernel::sched::idle_loop();
}

}  // namespace kernel::boot
