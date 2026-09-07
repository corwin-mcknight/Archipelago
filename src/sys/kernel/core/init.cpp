#include <kernel/boot.h>
#include <kernel/config.h>
#include <kernel/init_image.h>
#include <kernel/log.h>
#include <kernel/mm/physmap.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/mm/vmo.h>
#include <kernel/sched/task_construction.h>
#include <kernel/sched/user_task.h>
#include <std/new.h>

#include <ktl/atomic>

namespace kernel::sched {
namespace {
constinit ktl::atomic<bool> init_attempted = false;
static_assert(init_image::PAGE == KERNEL_MINIMUM_PAGE_SIZE);

// Only launch_coordinator calls this helper. The layout and permissions are determined by the
// boot contract, not by a user-supplied segment table or a load-image syscall.
ktl::result<void> install_boot_region(mm::vm_aspace& space, uintptr_t address, const uint8_t* bytes, uint64_t file_size,
                                      uint64_t memory_size, mm::vm_prot_t permissions) {
    if (memory_size == 0) { return ktl::result<void>::ok(); }
    auto backing = mm::create_anonymous_vmo(memory_size / init_image::PAGE);
    if (!backing) { return ktl::err(ktl::errc::oom); }
    auto committed = backing->commit(0, memory_size / init_image::PAGE);
    if (committed.is_err()) { return ktl::err(committed.unwrap_err()); }
    for (size_t offset = 0; offset < file_size; offset += init_image::PAGE) {
        auto frame = backing->resident_frame(offset / init_image::PAGE);
        if (!frame.has_value()) { return ktl::err(ktl::errc::oom); }
        mm::copy_to_frame(*frame, 0, bytes + offset, init_image::PAGE);
    }
    auto mapped = space.root().map(address, memory_size, backing, 0, permissions | mm::vm_prot::USER);
    return mapped.is_ok() ? ktl::result<void>::ok() : ktl::err(mapped.unwrap_err());
}
}  // namespace

ktl::result<ktl::ref<Task>> launch_coordinator() {
    // Consume the boot operation even when malformed input or resource exhaustion prevents
    // launch. Init is not restartable through the kernel's boot path.
    if (init_attempted.exchange(true)) { return ktl::err(ktl::errc::invalid_operation); }
    const auto* module = boot::find_module("init");
    if (module == nullptr) { return ktl::err(ktl::errc::invalid_operation); }
#if defined(ARCH_X86_64)
    constexpr auto MACHINE = init_image::MACHINE_X86_64;
#elif defined(ARCH_RISCV64)
    constexpr auto MACHINE = init_image::MACHINE_RISCV64;
#endif
    init_image::Header header;
    if (!init_image::validate(module->data, module->size, MACHINE, header)) {
        g_log.warn("boot: invalid init image");
        return ktl::err(ktl::errc::invalid_operation);
    }
    auto* space = new (std::nothrow) mm::vm_aspace();
    if (space == nullptr || !space->init()) {
        delete space;
        return ktl::err(ktl::errc::oom);
    }
    auto* bytes                 = static_cast<const uint8_t*>(module->data) + sizeof(header);
    uintptr_t address           = init_image::BASE;
    const uint64_t files[]      = {header.text_size, header.ro_size, header.data_size, 0};
    const uint64_t memory[]     = {header.text_size, header.ro_size, header.data_size + header.bss_size,
                                   4 * init_image::PAGE};
    const mm::vm_prot_t perms[] = {mm::vm_prot::READ | mm::vm_prot::EXECUTE, mm::vm_prot::READ,
                                   mm::vm_prot::READ | mm::vm_prot::WRITE, mm::vm_prot::READ | mm::vm_prot::WRITE};
    for (size_t i = 0; i < 4; ++i) {
        if (i == 3) { address = init_image::LIMIT; }
        auto mapped = install_boot_region(*space, address, bytes, files[i], memory[i], perms[i]);
        if (mapped.is_err()) {
            delete space;
            return ktl::err(mapped.unwrap_err());
        }
        bytes += files[i];
        address += memory[i];
    }
    auto factory = ktl::make_ref<TaskFactory>();
    if (!factory) {
        delete space;
        return ktl::err(ktl::errc::oom);
    }
    auto created = start_prepared_user_task("init", space, header.entry, init_image::LIMIT + 4 * init_image::PAGE,
                                            nullptr, factory);
    if (created.is_err()) { return ktl::err(created.unwrap_err()); }
    auto task = created.unwrap();
    if (endow_boot_modules(task).is_err()) { g_log.warn("boot: coordinator endowment incomplete"); }
    return ktl::result<ktl::ref<Task>>::ok(ktl::move(task));
}
}  // namespace kernel::sched
