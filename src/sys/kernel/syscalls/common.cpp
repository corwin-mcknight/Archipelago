#include <abi/syscall.h>
#include <kernel/panic.h>

#include "internal.h"

namespace kernel::syscalls {

static_assert(static_cast<int64_t>(ktl::errc::truncated) == abi::syscall::ERR_TRUNCATED);
static_assert(static_cast<int64_t>(ktl::errc::would_block) == abi::syscall::ERR_WOULD_BLOCK);
static_assert(static_cast<int64_t>(ktl::errc::peer_closed) == abi::syscall::ERR_PEER_CLOSED);
static_assert(static_cast<int64_t>(ktl::errc::timed_out) == abi::syscall::ERR_TIMED_OUT);

uint64_t install_pair(obj::HandleTable& table, ktl::ref<obj::Object> first, ktl::ref<obj::Object> second,
                      obj::Rights rights, const sched::IpcRange& output) {
    if (output.size() != 2 * sizeof(uint64_t)) { panic("pair output requires two handle slots"); }
    auto a = table.insert(ktl::move(first), rights);
    if (a.is_err()) { return errc_of(a.unwrap_err()); }
    auto b = table.insert(ktl::move(second), rights);
    if (b.is_err()) {
        (void)table.close(a.unwrap());
        return errc_of(b.unwrap_err());
    }
    uint64_t handles[2] = {obj::pack_handle(a.unwrap()), obj::pack_handle(b.unwrap())};
    output.write(handles, sizeof(handles));
    return 0;
}

}  // namespace kernel::syscalls
