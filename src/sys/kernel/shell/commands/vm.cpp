#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/mm/page_descriptor.h>
#include <kernel/mm/region.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/mm/vmo.h>
#include <kernel/shell/output.h>

#include <ktl/string_view>

namespace {

using namespace kernel::mm;

// "rwxu" with dashes for missing bits.
const char* prot_str(vm_prot_t prot) {
    static char buf[5];
    buf[0] = (prot & vm_prot::READ) ? 'r' : '-';
    buf[1] = (prot & vm_prot::WRITE) ? 'w' : '-';
    buf[2] = (prot & vm_prot::EXECUTE) ? 'x' : '-';
    buf[3] = (prot & vm_prot::USER) ? 'u' : '-';
    buf[4] = '\0';
    return buf;
}

const char* cache_str(vm_cache_mode mode) {
    switch (mode) {
        case vm_cache_mode::DEVICE: return "device";
        case vm_cache_mode::WRITE_COMBINING: return "wc";
        case vm_cache_mode::CACHED:
        default: return "cached";
    }
}

void dump_region(kernel::shell::ShellOutput& output, const Region& region, int depth) {
    for (int i = 0; i < depth; ++i) { output.print("  "); }
    output.print("region [0x{0:p}, 0x{1:p}) max={2}\n", region.base(), region.base() + region.size(),
                 prot_str(region.max_prot()));
    region.for_each_child([&](const region_child& slot) {
        if (slot.is_binding()) {
            for (int i = 0; i < depth + 1; ++i) { output.print("  "); }
            output.print("map [0x{0:p}, 0x{1:p}) {2} {3}", slot.base, slot.base + slot.size, prot_str(slot.prot),
                         cache_str(slot.cache));
            if (slot.vmo_ref.get() != nullptr) {
                const vmo& v = *slot.vmo_ref;
                output.print(" vmo(id={0} pages={1} resident={2} fills={3})", v.id(), v.size_pages(),
                             v.resident_pages(), v.fill_count());
            }
            output.print("\n");
        } else {
            dump_region(output, *slot.child, depth + 1);
        }
    });
}

void vm_handler(int argc, const ktl::string_view argv[], kernel::shell::ShellOutput& output) {
    if (argc < 2) {
        output.print("usage: vm aspace|stats\n");
        return;
    }
    if (argv[1] == "aspace") {
        vm_aspace& aspace = kernel_aspace();
        if (!aspace.has_root()) {
            output.print("vm: kernel aspace not initialized\n");
            return;
        }
        output.print("kernel aspace (faults={0}):\n", aspace.fault_count());
        dump_region(output, aspace.root(), 1);
    } else if (argv[1] == "stats") {
        output.print("page descriptors ({0} frames covered):\n", g_page_descriptors.coverage_end() / 0x1000);
        output.print("  wired:    {0}\n", g_page_descriptors.count(page_state::WIRED));
        output.print("  free:     {0}\n", g_page_descriptors.count(page_state::FREE));
        output.print("  zeroed:   {0}\n", g_page_descriptors.count(page_state::ZEROED));
        output.print("  active:   {0}\n", g_page_descriptors.count(page_state::ACTIVE));
        output.print("  inactive: {0}\n", g_page_descriptors.count(page_state::INACTIVE));
        output.print("fault counters:\n");
        output.print("  kernel aspace: {0}\n", kernel_aspace().fault_count());
    } else {
        output.print("unknown subcommand: {0}\n", argv[1]);
    }
}

}  // namespace

KSHELL_COMMAND(vm, "vm", "Virtual memory inspection", vm_handler);

#endif  // CONFIG_KERNEL_SHELL
