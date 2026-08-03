#include <kernel/boot.h>

#include <ktl/atomic>

#include "kernel/arch.h"
#include "kernel/assert.h"
#include "kernel/config.h"
#include "kernel/cpu.h"
#include "kernel/interrupt.h"
#include "kernel/log.h"
#include "kernel/mm/early_heap.h"
#include "kernel/panic.h"
#include "kernel/platform.h"
#include "kernel/synchronization/execution_context.h"
#include "kernel/x86/cpu.h"
#include "kernel/x86/descriptor_tables.h"

extern "C" void init_global_constructors_array(void);

// Enable EFER.NXE so a present PTE may carry the no-execute bit (bit 63).
// Must run on every CPU before any NX mapping is installed; with NXE clear that
// bit is reserved and faults. EFER is MSR 0xC0000080, NXE is bit 11.
static void enable_nxe() {
    constexpr uint32_t MSR_EFER = 0xC0000080;
    kernel::x86::wrmsr(MSR_EFER, kernel::x86::rdmsr(MSR_EFER) | (1u << 11));
}

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

    if (is_boot_processor) {
        g_interrupt_manager.initialize();
        kernel::platform::interrupt_init();
    }

    kernel::arch::enable_interrupts();
    g_log.debug("cpu{0}: Interrupts Enabled", core_index);

    // Boot Processor is responsible for initializing the time
    if (is_boot_processor) { kernel::platform::timer_init(); }

    g_log.debug("cpu{0}: Now running", core_index);
    g_cpu_cores[core_index].initialized.store(true, ktl::memory_order::release);
}

// Secondary-CPU entry, released by cpu_start_cores() via kernel::boot::start_cpu(). core_index is
// the dense CPU-list index; the hardware LAPIC id is stored as data, never used as a subscript.
[[noreturn]] void ap_entry(size_t core_index, uint64_t hw_id) {
    g_log.info("cpu{0}: Starting (lapic {1})", core_index, hw_id);
    core_init((uint32_t)core_index, (uint32_t)hw_id, /*is_boot_processor=*/false);
    while (true) { __asm__ volatile("hlt"); }
}

extern uintptr_t _initial_heap_start;
extern uintptr_t _initial_heap_end;

extern "C" [[noreturn]] void _start(void) {
    g_early_heap.on_boot((uintptr_t)&_initial_heap_start, (uintptr_t)&_initial_heap_end);

    init_global_constructors_array();

    kernel::platform::console_init();

    kernel::cpu_init_cores();

    g_log.info("Starting Archipelago ver. {0}", CONFIG_KERNEL_VERSION);

    kernel::boot::snapshot_symbols();
    kernel::boot::resolve_hhdm();
    kernel::boot::init_memory();

    const auto& boot_info = kernel::boot::collect();
    if (boot_info.cpu_count == 0) { panic("Boot protocol reported no CPUs"); }

    // The BP's dense logical index keys the per-core tables the same way the APs (and the startup
    // gate) are keyed. Fail fast on a malformed response rather than silently keying the BP into
    // slot 0 (which could collide with whichever core occupies list position 0).
    if (boot_info.boot_cpu_index == SIZE_MAX) { panic("Boot processor not present in bootloader CPU list"); }
    // cpu_start_cores() clamps secondary CPUs past CONFIG_MAX_CORES by ignoring them, but the boot
    // processor cannot be ignored and its dense index keys per-core tables everywhere -- so a BP
    // past the limit is a deliberate, explained panic rather than a tripped assert in core_init.
    if (boot_info.boot_cpu_index >= CONFIG_MAX_CORES) {
        panic("Boot processor sits past CONFIG_MAX_CORES in the bootloader CPU list; raise CONFIG_MAX_CORES");
    }
    uint32_t bsp_index = (uint32_t)boot_info.boot_cpu_index;

    g_log.info("Booting on cpu{0}. CPU has {1} cores", kernel::boot::cpu_hw_id(bsp_index), boot_info.cpu_count);

    core_init(bsp_index, (uint32_t)kernel::boot::cpu_hw_id(bsp_index), /*is_boot_processor=*/true);
    kernel::cpu_start_cores();
    kernel::cpu_gate_wait_for_cores_started();

    kernel::boot::late_boot(bsp_index);
}
