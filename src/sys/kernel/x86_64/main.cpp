#include <kernel/boot.h>

#include <ktl/algorithm>
#include <ktl/atomic>
#include <ktl/maybe>

#include "kernel/arch.h"
#include "kernel/assert.h"
#include "kernel/config.h"
#include "kernel/cpu.h"
#include "kernel/drivers/uart.h"
#include "kernel/interrupt.h"
#include "kernel/log.h"
#include "kernel/mm/early_heap.h"
#include "kernel/panic.h"
#include "kernel/synchronization/execution_context.h"
#include "kernel/x86/cpu.h"
#include "kernel/x86/descriptor_tables.h"
#include "kernel/x86/drivers/pit.h"
#include "vendor/limine.h"

extern kernel::driver::uart uart;
kernel::x86::drivers::pit_timer timer;

extern "C" void init_global_constructors_array(void);

// Enable EFER.NXE so a present PTE may carry the no-execute bit (bit 63).
// Must run on every CPU before any NX mapping is installed; with NXE clear that
// bit is reserved and faults. EFER is MSR 0xC0000080, NXE is bit 11.
static void enable_nxe() {
    constexpr uint32_t MSR_EFER = 0xC0000080;
    kernel::x86::wrmsr(MSR_EFER, kernel::x86::rdmsr(MSR_EFER) | (1u << 11));
}

__attribute__((used, section(".limine_requests"))) volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST, .revision = 0, .response = nullptr, .flags = 0};

// Bring a single CPU online. core_index is a dense logical index in [0, CONFIG_MAX_CORES) used to
// subscript the per-core tables (g_cpu_cores[], gdts[]); it is derived from the bootloader CPU-list
// position, never from the hardware LAPIC id (which may be sparse or exceed CONFIG_MAX_CORES).
// lapic_id is stored as data only. is_boot_processor selects the one-time global init the BP performs.
void core_init(uint32_t core_index, uint32_t lapic_id, bool is_boot_processor) {
    assert(core_index < CONFIG_MAX_CORES, "Logical core index exceeds maximum cores");
    g_cpu_cores[core_index].lapic_id = lapic_id;
    kernel::synchronization::init_execution_context(core_index);

    enable_nxe();

    // Start basic hardware initialisation
    g_log.debug("cpu{0} (lapic {1}): Initializing", core_index, lapic_id);
    kernel::x86::init_gdt((int)core_index);
    kernel::x86::init_idt();
    kernel::arch::syscall_init();

    if (is_boot_processor) { g_interrupt_manager.initialize(); }

    kernel::arch::enable_interrupts();
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

extern "C" [[noreturn]] void _start(void) {
    g_early_heap.on_boot((uintptr_t)&_initial_heap_start, (uintptr_t)&_initial_heap_end);

    init_global_constructors_array();

    uart.init();
    g_log.devices.push_back(&uart);

    kernel::cpu_init_cores();

    g_log.info("Starting Archipelago ver. {0}", CONFIG_KERNEL_VERSION);

    kernel::boot::snapshot_symbols();
    kernel::boot::resolve_hhdm();
    kernel::boot::init_memory();

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

    kernel::boot::late_boot(bsp_index);
}
