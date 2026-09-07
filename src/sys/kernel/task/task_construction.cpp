#include <kernel/config.h>
#include <kernel/mm/physmap.h>
#include <kernel/sched/task_construction.h>
#include <kernel/synchronization/guard.h>
#include <std/new.h>

namespace kernel::sched {

TaskConstruction::~TaskConstruction() { delete m_space; }

ktl::result<void> TaskConstruction::init(const char* name, size_t length) {
    if (m_space || name == nullptr || length == 0 || length >= NAME_CAPACITY) {
        return ktl::err(ktl::errc::invalid_operation);
    }
    // Debug names are owned, not pointers into a loader's mutable or temporary IPC buffer.
    for (size_t i = 0; i < length; ++i) {
        if (name[i] < 32 || name[i] > 126) { return ktl::err(ktl::errc::invalid_operation); }
        m_name[i] = name[i];
    }
    m_name[length] = 0;
    auto* space    = new (std::nothrow) mm::vm_aspace();
    if (space == nullptr || !space->init()) {
        delete space;
        return ktl::err(ktl::errc::oom);
    }
    m_space = space;
    return ktl::result<void>::ok();
}

ktl::result<void> TaskConstruction::map(mm::vmo& source, uint64_t offset, uintptr_t address, size_t size,
                                        mm::vm_prot_t prot) {
    constexpr size_t PAGE = KERNEL_MINIMUM_PAGE_SIZE;
    constexpr auto R      = mm::vm_prot::READ;
    constexpr auto W      = mm::vm_prot::WRITE;
    constexpr auto X      = mm::vm_prot::EXECUTE;
    if (!m_space || (prot != R && prot != (R | W) && prot != (R | X)) || size == 0 || size % PAGE != 0 ||
        offset % PAGE != 0 || address % PAGE != 0 || address < PAGE || address >= mm::vm_aspace::low_limit() ||
        size > mm::vm_aspace::low_limit() - address || size > MAX_BYTES - m_bytes ||
        offset / PAGE > source.size_pages() || size / PAGE > source.size_pages() - offset / PAGE) {
        return ktl::err(ktl::errc::invalid_operation);
    }
    auto backing = mm::create_anonymous_vmo(size / PAGE);
    if (!backing) { return ktl::err(ktl::errc::oom); }
    auto committed = backing->commit(0, size / PAGE);
    if (committed.is_err()) { return ktl::err(committed.unwrap_err()); }
    auto populated = source.commit(offset / PAGE, size / PAGE);
    if (populated.is_err()) { return ktl::err(populated.unwrap_err()); }
    for (size_t page = 0; page < size / PAGE; ++page) {
        auto src = source.resident_frame(offset / PAGE + page);
        auto dst = backing->resident_frame(page);
        if (!src || !dst) { return ktl::err(ktl::errc::oom); }
        auto* bytes = reinterpret_cast<const void*>(mm::direct_map_address(mm::physical_address(*src)));
        mm::copy_to_frame(*dst, 0, bytes, PAGE);
    }
    auto mapped = m_space->root().map(address, size, backing, 0, prot | mm::vm_prot::USER);
    if (mapped.is_err()) { return ktl::err(mapped.unwrap_err()); }
    m_bytes += size;
    return ktl::result<void>::ok();
}

bool valid_user_start(mm::vm_aspace& space, uintptr_t entry, uintptr_t stack) {
    if (stack == 0 || (stack & 15) != 0 || stack > mm::vm_aspace::low_limit()) { return false; }
#if defined(ARCH_RISCV64)
    if ((entry & 1) != 0) { return false; }
#endif
    synchronization::critical_irq_lock_guard guard(mm::g_vmm_lock);
    auto* code  = space.root().find_binding(entry);
    auto* frame = space.root().find_binding(stack - 1);
    return code && frame && (code->prot & mm::vm_prot::EXECUTE) != 0 && (frame->prot & mm::vm_prot::WRITE) != 0 &&
           stack - frame->base >= 16;
}

bool TaskConstruction::valid_start(uintptr_t entry, uintptr_t stack) const {
    return m_space && valid_user_start(*m_space, entry, stack);
}

mm::vm_aspace* TaskConstruction::release() {
    auto* space = m_space;
    m_space     = nullptr;
    return space;
}

}  // namespace kernel::sched
