#include <kernel/boot.h>
#include <kernel/obj/handle_table.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/shell/shell.h>

#include <ktl/maybe>
#include <ktl/string_view>

#include "kernel/config.h"
#include "kernel/drivers/uart.h"
#include "kernel/log.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/obj/event.h"
#include "kernel/panic.h"
#include "kernel/symbols.h"
#include "vendor/limine.h"

kernel::driver::uart uart;

uintptr_t g_hhdm_offset                                                                             = 0;

// The bootloader requests every architecture needs. Arch-specific requests
// (paging mode, MP) stay in the arch's main.cpp.
__attribute__((used, section(".limine_requests"))) volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0, .response = nullptr};

__attribute__((used, section(".limine_requests"))) volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0, .response = nullptr};

__attribute__((used,
               section(".limine_requests"))) volatile struct limine_executable_file_request executable_file_request = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST, .revision = 0, .response = nullptr};

__attribute__((
    used, section(".limine_requests"))) volatile struct limine_executable_cmdline_request executable_cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST, .revision = 0, .response = nullptr};

namespace kernel::boot {

namespace {

// Returns true if the space-delimited Limine command line contains an exact-match token.
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

// Boot mode is a compile-time default that the Limine command line can override at runtime: the
// "shell" token forces the interactive shell, "noshell" forces a normal boot. The shell can only
// be entered when it was compiled in (CONFIG_KERNEL_SHELL), so the runtime override only narrows or
// confirms within that capability.
bool resolve_shell_boot() {
    bool shell_boot = CONFIG_KERNEL_SHELL;
    if (executable_cmdline_request.response == nullptr || executable_cmdline_request.response->cmdline == nullptr) {
        g_log.info("boot: no Limine command line; using compile-time boot mode");
        return shell_boot;
    }
    ktl::string_view cmdline(executable_cmdline_request.response->cmdline);
    g_log.info("boot: Limine command line: \"{0}\"", cmdline);
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

void resolve_hhdm() {
    if (hhdm_request.response == nullptr) { panic("Limine HHDM request failed"); }
    g_hhdm_offset = hhdm_request.response->offset;
    if (g_hhdm_offset == 0) { panic("Limine HHDM offset is zero -- higher-half direct map unavailable"); }
}

void snapshot_symbols() {
    // Snapshot the kernel ELF's symbol table before PMM reclaims bootloader memory. The bootloader
    // response pointers are optional, so they flow as maybes instead of nested null checks.
    // Side-effect-only pipeline: the trailing maybe is intentionally discarded.
    (void)ktl::from_ptr(executable_file_request.response)
        .and_then([](limine_executable_file_response& resp) { return ktl::from_ptr(resp.executable_file); })
        .inspect([](limine_file& f) {
            kernel::symbols::init(f.address, f.size);
            if (kernel::symbols::available()) {
                g_log.info("symbols: kernel symbol table loaded");
            } else {
                g_log.warn("symbols: kernel symbol table unavailable");
            }
        })
        .or_else([]() -> ktl::maybe<limine_file&> {
            g_log.warn("symbols: executable_file request not honored");
            return ktl::nothing;
        });
}

void init_memory() {
    if (memmap_request.response == nullptr) { panic("Limine memmap request failed"); }
    if (memmap_request.response->entry_count == 0) { panic("Limine memmap is empty -- no memory regions reported"); }

    // Range lists for VMM init: usable ranges become FREE page descriptors,
    // kernel ranges stay WIRED. Fixed capacity -- Limine memmaps are small;
    // overflow only costs descriptor precision, so warn and drop.
    constexpr size_t MAX_MEMMAP_RANGES = 48;
    kernel::mm::vm_page_region usable_ranges[MAX_MEMMAP_RANGES];
    kernel::mm::vm_page_region wired_ranges[MAX_MEMMAP_RANGES];
    size_t usable_range_count   = 0;
    size_t wired_range_count    = 0;

    uint64_t total_usable_pages = 0;
    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        auto* entry = memmap_request.response->entries[i];

        // A malformed entry must not corrupt the PMM: a misaligned base or non-page-multiple length
        // would hand the allocator a partial frame. Skip such regions with a warning rather than
        // truncating silently.
        if ((entry->base & 0xFFF) != 0 || (entry->length & 0xFFF) != 0) {
            g_log.warn("pmm: skipping misaligned region base=0x{0:p} length=0x{1:p} type={2}", entry->base,
                       entry->length, entry->type);
            continue;
        }
        size_t pages = entry->length / 0x1000;

        // RECLAIMABLE regions still hold live Limine responses (memmap, cmdline)
        // that boot code reads after this loop, so they are left out of the pool.
        // Leaks ~20MB of reclaimable memory; copy-out-then-reclaim when it matters.
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            if (pages == 0) {
                g_log.warn("pmm: skipping empty usable region base=0x{0:p}", entry->base);
                continue;
            }
            g_log.info("pmm: adding region base=0x{0:p} pages={1} type={2}", entry->base, pages, entry->type);
            kernel::mm::g_page_frame_allocator.add_region({.start = entry->base, .count = pages});
            total_usable_pages += pages;
            if (usable_range_count < MAX_MEMMAP_RANGES) {
                usable_ranges[usable_range_count++] = {.start = entry->base, .count = pages};
            } else {
                g_log.warn("vmm: dropping usable range base=0x{0:p} from descriptor coverage", entry->base);
            }
        } else if (entry->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
            g_log.info("pmm: reserved region base=0x{0:p} pages={1} (kernel)", entry->base, pages);
            kernel::mm::g_page_frame_allocator.add_reserved(pages);
            if (wired_range_count < MAX_MEMMAP_RANGES) {
                wired_ranges[wired_range_count++] = {.start = entry->base, .count = pages};
            } else {
                g_log.warn("vmm: dropping kernel range base=0x{0:p} from descriptor coverage", entry->base);
            }
        }
    }
    if (total_usable_pages == 0) { panic("Limine memmap reported no usable memory"); }
    g_log.info("Memory subsystem initialized ({0} usable pages)", total_usable_pages);

    kernel::mm::vmm_init(usable_ranges, usable_range_count, wired_ranges, wired_range_count);
}

#if CONFIG_KERNEL_SHELL
static void shell_thread_main(void*) { kernel::shell::shell_main(); }
#endif

[[noreturn]] void late_boot(uint32_t boot_core_index) {
    kernel::obj::obj_init();
    g_log.info("Object subsystem initialized");

    auto kernel_task = kernel::sched::kernel_task();
    auto evt_id      = kernel_task->handles()
                           .emplace<kernel::obj::Event>(kernel::obj::RIGHT_READ | kernel::obj::RIGHT_SIGNAL)
                           .unwrap();
    kernel_task->handles().get<kernel::obj::Event>(evt_id).unwrap()->signal_set(0x1);

    kernel::sched::init(boot_core_index);

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
