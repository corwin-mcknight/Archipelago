#include <kernel/obj/handle_table.h>
#include <kernel/shell/shell.h>
#include <kernel/testing/testing.h>

#include <ktl/algorithm>
#include <ktl/atomic>
#include <ktl/fixed_string>
#include <ktl/fmt>
#include <ktl/maybe>
#include <ktl/result>

#include "kernel/assert.h"
#include "kernel/config.h"
#include "kernel/cpu.h"
#include "kernel/drivers/uart.h"
#include "kernel/interrupt.h"
#include "kernel/log.h"
#include "kernel/mm/early_heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/obj/event.h"
#include "kernel/panic.h"
#include "kernel/symbols.h"
#include "kernel/x86/descriptor_tables.h"
#include "kernel/x86/drivers/pit.h"
#include "vendor/limine.h"

kernel::driver::uart uart;
kernel::x86::drivers::pit_timer timer;

extern "C" void init_global_constructors_array(void);

__attribute__((used, section(".limine_requests"))) volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST, .revision = 0, .response = nullptr, .flags = 0};

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

uintptr_t g_hhdm_offset = 0;

// Bring a single CPU online. core_index is a dense logical index in [0, CONFIG_MAX_CORES) used to
// subscript the per-core tables (g_cpu_cores[], gdts[]); it is derived from the bootloader CPU-list
// position, never from the hardware LAPIC id (which may be sparse or exceed CONFIG_MAX_CORES).
// lapic_id is stored as data only. is_boot_processor selects the one-time global init the BP performs.
void core_init(uint32_t core_index, uint32_t lapic_id, bool is_boot_processor) {
    assert(core_index < CONFIG_MAX_CORES, "Logical core index exceeds maximum cores");
    g_cpu_cores[core_index].lapic_id = lapic_id;

    // Start basic hardware initialisation
    g_log.debug("cpu{0} (lapic {1}): Initializing", core_index, lapic_id);
    kernel::x86::init_gdt((int)core_index);
    kernel::x86::init_idt();

    if (is_boot_processor) { g_interrupt_manager.initialize(); }

    kernel::x86::enable_interrupts();
    g_log.debug("cpu{0}: Interrupts Enabled", core_index);

    // Boot Processor is responsible for initializing the time
    if (is_boot_processor) {
        timer.init();
        g_log.info("Time subsystem initialized");
    }

    g_log.debug("cpu{0}: Now running", core_index);
    g_cpu_cores[core_index].initialized.store(true, ktl::memory_order::release);
}

extern "C" [[noreturn]] void ap_startup(struct limine_mp_info* info) {
    // The dense logical index was published in extra_argument by cpu_start_cores() before this AP was
    // released; the hardware LAPIC id is reported separately and is never used as an array subscript.
    g_log.info("cpu{0}: Starting (lapic {1})", info->extra_argument, info->lapic_id);
    core_init((uint32_t)info->extra_argument, info->lapic_id, /*is_boot_processor=*/false);
    while (true) { __asm__ volatile("hlt"); }
}

extern uintptr_t _initial_heap_start;
extern uintptr_t _initial_heap_end;

// The boot processor's dense logical index is its position in the bootloader CPU list, which is
// not necessarily 0 nor equal to its LAPIC id. A malformed response may omit the BP entirely.
static ktl::maybe<uint32_t> find_bsp_index(const limine_mp_response& mp) {
    return ktl::find_index_if(mp.cpus, mp.cpus + mp.cpu_count,
                              [&](const limine_mp_info* cpu) { return cpu->lapic_id == mp.bsp_lapic_id; })
        .map([](size_t i) { return (uint32_t)i; });
}

// Returns true if the space-delimited Limine command line contains an exact-match token.
static bool cmdline_has_token(ktl::string_view cmdline, ktl::string_view token) {
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
static bool resolve_shell_boot() {
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

extern "C" [[noreturn]] void _start(void) {
    g_early_heap.on_boot((uintptr_t)&_initial_heap_start, (uintptr_t)&_initial_heap_end);

    init_global_constructors_array();

    uart.init();
    g_log.devices.push_back(&uart);

    kernel::cpu_init_cores();

    g_log.info("Starting Archipelago ver. {0}", CONFIG_KERNEL_VERSION);

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

    if (hhdm_request.response == nullptr) { panic("Limine HHDM request failed"); }
    g_hhdm_offset = hhdm_request.response->offset;
    if (g_hhdm_offset == 0) { panic("Limine HHDM offset is zero -- higher-half direct map unavailable"); }

    if (memmap_request.response == nullptr) { panic("Limine memmap request failed"); }
    if (memmap_request.response->entry_count == 0) { panic("Limine memmap is empty -- no memory regions reported"); }

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

        if (entry->type == LIMINE_MEMMAP_USABLE || entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            if (pages == 0) {
                g_log.warn("pmm: skipping empty usable region base=0x{0:p}", entry->base);
                continue;
            }
            g_log.info("pmm: adding region base=0x{0:p} pages={1} type={2}", entry->base, pages, entry->type);
            kernel::mm::g_page_frame_allocator.add_region({.start = entry->base, .count = pages});
            total_usable_pages += pages;
        } else if (entry->type == 6) {  // LIMINE_MEMMAP_KERNEL_AND_MODULES / EXECUTABLE_AND_MODULES
            g_log.info("pmm: reserved region base=0x{0:p} pages={1} (kernel)", entry->base, pages);
            kernel::mm::g_page_frame_allocator.add_reserved(pages);
        }
    }
    if (total_usable_pages == 0) { panic("Limine memmap reported no usable memory"); }
    g_log.info("Memory subsystem initialized ({0} usable pages)", total_usable_pages);

    if (mp_request.response == nullptr) { panic("Limine MP request failed"); }
    if (mp_request.response->cpu_count == 0) { panic("Limine MP response reports zero CPUs"); }

    g_log.info("Booting on cpu{0}. CPU has {1} cores", mp_request.response->bsp_lapic_id,
               mp_request.response->cpu_count);

    // Locate the BP's dense logical index so it keys the per-core tables the same way the APs (and
    // the startup gate) do. Fail fast on a malformed response rather than silently keying the BP
    // into slot 0 (which could collide with whichever core occupies list position 0).
    uint32_t bsp_index =
        find_bsp_index(*mp_request.response).expect("Boot processor LAPIC id not present in bootloader CPU list");

    core_init(bsp_index, mp_request.response->bsp_lapic_id, /*is_boot_processor=*/true);
    kernel::cpu_start_cores();
    kernel::cpu_gate_wait_for_cores_started();

    kernel::obj::obj_init();
    g_log.info("Object subsystem initialized");

    auto evt_id =
        kernel::obj::g_handle_table.emplace<kernel::obj::Event>(kernel::obj::RIGHT_READ | kernel::obj::RIGHT_SIGNAL)
            .unwrap();
    kernel::obj::g_handle_table.get<kernel::obj::Event>(evt_id).unwrap()->signal_set(0x1);

    bool shell_boot = resolve_shell_boot();
#if CONFIG_KERNEL_SHELL
    if (shell_boot) {
        g_log.info("boot: kernel shell boot -- handing control to interactive shell");
        kernel::shell::shell_main();
    } else {
        g_log.info("boot: normal boot -- initialization complete");
    }
#else
    if (shell_boot) { g_log.warn("boot: shell requested but not compiled in (CONFIG_KERNEL_SHELL=0)"); }
    g_log.info("boot: normal boot -- initialization complete");
#endif

    panic("Boot processor exited early");
}
