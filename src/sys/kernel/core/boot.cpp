#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/obj/handle_table.h>
#include <kernel/platform.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
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
#include "kernel/obj/event.h"
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

// Boot mode is a compile-time default that the command line can override at runtime: the
// "shell" token forces the interactive shell, "noshell" forces a normal boot. The shell can only
// be entered when it was compiled in (CONFIG_KERNEL_SHELL), so the runtime override only narrows or
// confirms within that capability.
bool resolve_shell_boot() {
    bool shell_boot = CONFIG_KERNEL_SHELL;
    if (collect().cmdline == nullptr) {
        g_log.info("boot: no command line; using compile-time boot mode");
        return shell_boot;
    }
    ktl::string_view cmdline(collect().cmdline);
    g_log.info("boot: command line: \"{0}\"", cmdline);
    if (cmdline_has_token(cmdline, "noshell")) {
        g_log.info("boot: command line requested normal boot (noshell)");
        shell_boot = false;
    } else if (cmdline_has_token(cmdline, "shell")) {
        g_log.info("boot: command line requested kernel shell boot (shell)");
        shell_boot = true;
    }
    return shell_boot;
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
    kernel::obj::obj_init();
    g_log.info("Object subsystem initialized");

    auto kernel_task = kernel::sched::kernel_task();
    auto evt_id      = kernel_task->handles()
                           .emplace<kernel::obj::Event>(kernel::obj::RIGHT_READ | kernel::obj::RIGHT_SIGNAL)
                           .unwrap();
    kernel_task->handles().get<kernel::obj::Event>(evt_id).unwrap()->signal_set(0x1);

    kernel::platform::timestamp_calibrate();
    kernel::time::use_timestamp_clock();
    kernel::sched::init(boot_core_index);

    kernel::sched::spawn("zeroer", zeroer_thread_main, nullptr).expect("boot: zeroer spawn failed");

    bool shell_boot = resolve_shell_boot();
#if CONFIG_KERNEL_SHELL
    if (shell_boot) {
        g_log.info("boot: kernel shell boot -- starting shell thread");
        kernel::sched::spawn("kshell", shell_thread_main, nullptr).expect("boot: shell spawn failed");
    } else {
        g_log.info("boot: normal boot -- initialization complete");
    }
#else
    if (shell_boot) { g_log.warn("boot: shell requested but not compiled in (CONFIG_KERNEL_SHELL=0)"); }
    g_log.info("boot: normal boot -- initialization complete");
#endif

    kernel::sched::idle_loop();
}

}  // namespace kernel::boot
