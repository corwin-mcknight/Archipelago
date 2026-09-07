#include <kernel/sched/task_construction.h>
#include <kernel/sched/user_task.h>

#include "internal.h"

namespace kernel::syscalls {
namespace {
ktl::result<ktl::ref<sched::TaskFactory>> authority(sched::Thread& self, uint64_t handle) {
    return self.owner()->handles().get<sched::TaskFactory>(obj::unpack_handle(handle), obj::RIGHT_WRITE);
}
}  // namespace

uint64_t sys_task_build_create(sched::Thread& self, uint64_t factory, uint64_t name_offset, uint64_t name_size) {
    auto allowed = authority(self, factory);
    if (allowed.is_err()) { return errc_of(allowed.unwrap_err()); }
    if (self.construction() || name_size == 0 || name_size >= sched::TaskConstruction::NAME_CAPACITY) {
        return errc_of(ktl::errc::invalid_operation);
    }
    auto range = self.ipc().range(name_offset, name_size);
    if (range.is_err()) { return errc_of(range.unwrap_err()); }
    char name[sched::TaskConstruction::NAME_CAPACITY];
    range.unwrap().read(name, name_size);
    auto construction = ktl::make_ref<sched::TaskConstruction>();
    if (!construction) { return errc_of(ktl::errc::oom); }
    auto initialized = construction->init(name, name_size);
    if (initialized.is_err()) { return errc_of(initialized.unwrap_err()); }
    self.set_construction(ktl::move(construction));
    return 0;
}

uint64_t sys_task_build_map(sched::Thread& self, uint64_t factory, uint64_t vmo_handle, uint64_t descriptor_offset) {
    auto allowed = authority(self, factory);
    if (allowed.is_err()) { return errc_of(allowed.unwrap_err()); }
    auto construction = self.construction();
    if (!construction) { return errc_of(ktl::errc::invalid_operation); }
    auto range = self.ipc().range(descriptor_offset, sizeof(abi_task_build_mapping));
    if (range.is_err()) { return errc_of(range.unwrap_err()); }
    abi_task_build_mapping descriptor;
    range.unwrap().read(&descriptor, sizeof(descriptor));
    // Check before narrowing the ABI's 64-bit protection field.
    if (descriptor.prot != ABI_VM_PROT_READ && descriptor.prot != (ABI_VM_PROT_READ | ABI_VM_PROT_WRITE) &&
        descriptor.prot != (ABI_VM_PROT_READ | ABI_VM_PROT_EXEC)) {
        return errc_of(ktl::errc::invalid_operation);
    }
    auto source = self.owner()->handles().get<mm::vmo>(obj::unpack_handle(vmo_handle), obj::RIGHT_READ);
    if (source.is_err()) { return errc_of(source.unwrap_err()); }
    auto mapped = construction->map(*source.unwrap(), descriptor.offset, descriptor.address, descriptor.size,
                                    static_cast<mm::vm_prot_t>(descriptor.prot));
    return mapped.is_ok() ? 0 : errc_of(mapped.unwrap_err());
}

uint64_t sys_task_build_abort(sched::Thread& self, uint64_t factory) {
    auto allowed = authority(self, factory);
    if (allowed.is_err()) { return errc_of(allowed.unwrap_err()); }
    self.set_construction({});
    return 0;
}

uint64_t sys_task_build_start(sched::Thread& self, uint64_t factory, uint64_t entry, uint64_t stack,
                              uint64_t output_offset) {
    using namespace obj;
    auto allowed = authority(self, factory);
    if (allowed.is_err()) { return errc_of(allowed.unwrap_err()); }
    auto output = self.ipc().range(output_offset, 2 * sizeof(uint64_t));
    if (output.is_err()) { return errc_of(output.unwrap_err()); }
    auto construction = self.construction();
    if (!construction || !construction->valid_start(entry, stack)) { return errc_of(ktl::errc::invalid_operation); }

    // Consume the thread's construction before publishing a runnable child. No other thread
    // can access it, and any subsequent start on this thread sees no pending construction.
    self.set_construction({});
    ktl::ref<Channel> parent;
    auto created =
        sched::start_prepared_user_task(construction->name(), construction->release(), entry, stack, &parent);
    if (created.is_err()) { return errc_of(created.unwrap_err()); }
    auto task   = created.unwrap();
    auto& table = self.owner()->handles();
    auto child  = table.insert(task, RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_WAIT);
    if (child.is_err()) {
        (void)sched::task_kill(task);
        return errc_of(child.unwrap_err());
    }
    auto mailbox = table.insert(ktl::move(parent), Channel::DEFAULT_RIGHTS);
    if (mailbox.is_err()) {
        (void)table.close(child.unwrap());
        (void)sched::task_kill(task);
        return errc_of(mailbox.unwrap_err());
    }
    uint64_t handles[] = {pack_handle(child.unwrap()), pack_handle(mailbox.unwrap())};
    output.unwrap().write(handles, sizeof(handles));
    return 0;
}

uint64_t sys_thread_start(sched::Thread& self, uint64_t factory, uint64_t entry, uint64_t stack) {
    auto allowed = self.owner()->handles().get<sched::ThreadFactory>(obj::unpack_handle(factory), obj::RIGHT_WRITE);
    if (allowed.is_err()) { return errc_of(allowed.unwrap_err()); }
    if (!allowed.unwrap()->permits(self.owner()->id())) { return errc_of(ktl::errc::invalid_operation); }
    auto started = sched::start_user_thread(self.owner(), entry, stack);
    return started.is_ok() ? obj::pack_handle(started.unwrap()) : errc_of(started.unwrap_err());
}

}  // namespace kernel::syscalls
