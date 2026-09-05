#include <abi/syscall.h>

#include "internal.h"

namespace kernel::syscalls {

uint64_t sys_handle_close(obj::HandleTable& table, uint64_t handle) {
    auto closed = table.close(obj::unpack_handle(handle));
    return closed.is_ok() ? 0 : errc_of(closed.unwrap_err());
}

uint64_t sys_handle_duplicate(obj::HandleTable& table, uint64_t handle, uint64_t rights) {
    auto duplicated = table.duplicate(obj::unpack_handle(handle), static_cast<obj::Rights>(rights));
    if (duplicated.is_err()) { return errc_of(duplicated.unwrap_err()); }
    return obj::pack_handle(duplicated.unwrap());
}

uint64_t sys_handle_restrict(obj::HandleTable& table, uint64_t handle, uint64_t rights, uint64_t mode) {
    namespace sys = abi::syscall;
    auto id       = obj::unpack_handle(handle);
    if (mode == sys::HANDLE_RESTRICT_REMOVE) {
        auto removed = table.remove_rights(id, static_cast<obj::Rights>(rights));
        return removed.is_ok() ? 0 : errc_of(removed.unwrap_err());
    }
    if (mode != sys::HANDLE_RESTRICT_RETAIN || rights > UINT32_MAX) {
        // The ABI reports an invalid handle before malformed rights or mode.
        if (!table.is_valid(id)) { return errc_of(ktl::errc::handle_invalid); }
        return errc_of(mode != sys::HANDLE_RESTRICT_RETAIN ? ktl::errc::invalid_operation
                                                           : ktl::errc::rights_violation);
    }
    auto restricted = table.restrict_rights(id, static_cast<obj::Rights>(rights));
    return restricted.is_ok() ? 0 : errc_of(restricted.unwrap_err());
}

uint64_t sys_object_info(obj::HandleTable& table, uint64_t handle) {
    auto verified = table.verify(obj::unpack_handle(handle), 0);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }
    auto found = verified.unwrap();
    return static_cast<uint64_t>(found.object->type_id()) | (static_cast<uint64_t>(found.rights) << 32);
}

}  // namespace kernel::syscalls
