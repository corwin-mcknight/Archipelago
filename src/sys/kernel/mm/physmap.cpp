#include <kernel/config.h>
#include <kernel/mm/page.h>
#include <kernel/mm/physmap.h>
#include <kernel/panic.h>
#include <string.h>

namespace kernel::mm {
namespace {

uintptr_t g_direct_map_base = 0;

uintptr_t checked_address(physical_address address) {
    if (g_direct_map_base == 0) { panic("direct map used before initialization"); }
    if (address.value() > UINTPTR_MAX - g_direct_map_base) { panic("direct map address overflow"); }
    return g_direct_map_base + address.value();
}

void check_frame_range(size_t offset, size_t length) {
    if (offset > KERNEL_MINIMUM_PAGE_SIZE || length > KERNEL_MINIMUM_PAGE_SIZE - offset) {
        panic("frame byte range exceeds one page");
    }
}

}  // namespace

void direct_map_initialize(uintptr_t base) {
    if (base == 0) { panic("bootloader supplied no physical map base -- direct map unavailable"); }
    if (g_direct_map_base != 0) { panic("direct map initialized more than once"); }
    g_direct_map_base = base;
}

bool direct_map_ready() { return g_direct_map_base != 0; }

uintptr_t direct_map_address(physical_address address) { return checked_address(address); }

physical_address direct_map_physical(const void* address) {
    uintptr_t value = reinterpret_cast<uintptr_t>(address);
    if (g_direct_map_base == 0) { panic("direct map used before initialization"); }
    if (value < g_direct_map_base) { panic("address is outside the direct map"); }
    return physical_address(value - g_direct_map_base);
}

void zero_frame(vm_paddr_t frame) {
    memset(reinterpret_cast<void*>(checked_address(physical_address(frame))), 0, KERNEL_MINIMUM_PAGE_SIZE);
}

void copy_to_frame(vm_paddr_t frame, size_t offset, const void* source, size_t length) {
    check_frame_range(offset, length);
    memcpy(reinterpret_cast<void*>(checked_address(physical_address(frame)) + offset), source, length);
}

void copy_from_frame(void* destination, vm_paddr_t frame, size_t offset, size_t length) {
    check_frame_range(offset, length);
    memcpy(destination, reinterpret_cast<const void*>(checked_address(physical_address(frame)) + offset), length);
}

namespace unsafe {

uintptr_t direct_map_address(physical_address address) { return checked_address(address); }
physical_address direct_map_physical(const void* address) { return kernel::mm::direct_map_physical(address); }

}  // namespace unsafe

}  // namespace kernel::mm
