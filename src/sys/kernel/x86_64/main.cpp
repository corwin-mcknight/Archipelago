#include <kernel/obj/handle_table.h>
#include <kernel/shell/shell.h>
#include <kernel/testing/testing.h>

#include <ktl/atomic>
#include <ktl/circular_buffer>
#include <ktl/fixed_string>
#include <ktl/fmt>
#include <ktl/maybe>
#include <ktl/result>
#include <ktl/static_array>

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

extern "C" [[noreturn]] void _start(void) {
    g_early_heap.on_boot((uintptr_t)&_initial_heap_start, (uintptr_t)&_initial_heap_end);

    init_global_constructors_array();

    uart.init();
    g_log.devices.push_back(&uart);

    kernel::cpu_init_cores();

    g_log.info("Starting Archipelago ver. {0}", CONFIG_KERNEL_VERSION);

    // Snapshot the kernel ELF's symbol table before PMM reclaims bootloader memory.
    if (executable_file_request.response != nullptr && executable_file_request.response->executable_file != nullptr) {
        auto* f = executable_file_request.response->executable_file;
        kernel::symbols::init(f->address, f->size);
        if (kernel::symbols::available()) {
            g_log.info("symbols: kernel symbol table loaded");
        } else {
            g_log.warn("symbols: kernel symbol table unavailable");
        }
    } else {
        g_log.warn("symbols: executable_file request not honored");
    }

    if (hhdm_request.response == nullptr) { panic("Limine HHDM request failed"); }
    g_hhdm_offset = hhdm_request.response->offset;

    if (memmap_request.response == nullptr) { panic("Limine memmap request failed"); }
    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        auto* entry  = memmap_request.response->entries[i];
        size_t pages = entry->length / 0x1000;
        if (entry->type == LIMINE_MEMMAP_USABLE || entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            g_log.info("pmm: adding region base=0x{0:p} pages={1} type={2}", entry->base, pages, entry->type);
            kernel::mm::g_page_frame_allocator.add_region({.start = entry->base, .count = pages});
        } else if (entry->type == 6) {  // LIMINE_MEMMAP_KERNEL_AND_MODULES
            g_log.info("pmm: reserved region base=0x{0:p} pages={1} (kernel)", entry->base, pages);
            kernel::mm::g_page_frame_allocator.add_reserved(pages);
        }
    }
    g_log.info("Memory subsystem initialized");

    if (mp_request.response == nullptr) { panic("Limine MP request failed"); }

    g_log.info("Booting on cpu{0}. CPU has {1} cores", mp_request.response->bsp_lapic_id,
               mp_request.response->cpu_count);

    // The boot processor's dense logical index is its position in the bootloader CPU list, which is
    // not necessarily 0 nor equal to its LAPIC id. Locate it so the BP keys the per-core tables the
    // same way the APs (and the startup gate) do.
    uint32_t bsp_index = 0;
    bool found_bsp     = false;
    for (uint64_t i = 0; i < mp_request.response->cpu_count; i++) {
        if (mp_request.response->cpus[i]->lapic_id == mp_request.response->bsp_lapic_id) {
            bsp_index = (uint32_t)i;
            found_bsp = true;
            break;
        }
    }
    // Fail fast on a malformed response rather than silently keying the BP into slot 0 (which could
    // collide with whichever core occupies list position 0).
    assert(found_bsp, "Boot processor LAPIC id not present in bootloader CPU list");

    core_init(bsp_index, mp_request.response->bsp_lapic_id, /*is_boot_processor=*/true);
    kernel::cpu_start_cores();
    kernel::cpu_gate_wait_for_cores_started();

    kernel::obj::obj_init();
    g_log.info("Object subsystem initialized");

    auto evt_id =
        kernel::obj::g_handle_table.emplace<kernel::obj::Event>(kernel::obj::RIGHT_READ | kernel::obj::RIGHT_SIGNAL)
            .unwrap();
    kernel::obj::g_handle_table.get<kernel::obj::Event>(evt_id).unwrap()->signal_set(0x1);

#if CONFIG_KERNEL_SHELL
    kernel::shell::shell_main();
#endif

    panic("Boot processor exited early");
}
