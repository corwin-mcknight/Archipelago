#include <abi/syscall.h>
#include <kernel/obj/handle_dispatch.h>

namespace kernel::obj {

namespace {

namespace sys = ::abi::syscall;

// Everything a handler may act on. The handle is already verified by the time a handler runs;
// id stays available because close and duplicate are table operations, not object operations.
struct op_context {
    HandleTable& table;
    HandleId id;
    VerifiedHandle verified;
    uint64_t arg;
};

uint64_t errc_of(ktl::errc error) { return static_cast<uint64_t>(error); }

uint64_t op_close(op_context& ctx) {
    auto closed = ctx.table.close(ctx.id);
    return closed.is_ok() ? 0 : errc_of(closed.unwrap_err());
}

uint64_t op_duplicate(op_context& ctx) {
    auto dup = ctx.table.duplicate(ctx.id, static_cast<Rights>(ctx.arg));
    if (dup.is_err()) { return errc_of(dup.unwrap_err()); }
    return pack_handle(dup.unwrap());
}

uint64_t op_info(op_context& ctx) {
    return static_cast<uint64_t>(ctx.verified.object->type_id()) | (static_cast<uint64_t>(ctx.verified.rights) << 32);
}

// One row per handle syscall: the operation cannot run without passing the pipeline with exactly
// these requirements. expected_type INVALID means any type -- every operation so far is generic,
// but the column is what a task- or thread-specific operation will fill in.
struct op_spec {
    uint64_t nr;
    TypeId expected_type;
    Rights required_rights;
    uint64_t (*handler)(op_context&);
};

constexpr op_spec OPS[] = {
    {sys::SYS_HANDLE_CLOSE, type_ids::INVALID, 0, op_close},
    {sys::SYS_HANDLE_DUPLICATE, type_ids::INVALID, RIGHT_DUPLICATE, op_duplicate},
    {sys::SYS_OBJ_INFO, type_ids::INVALID, 0, op_info},
};

}  // namespace

uint64_t dispatch_handle_op(HandleTable& table, uint64_t nr, uint64_t handle, uint64_t arg) {
    for (const op_spec& op : OPS) {
        if (op.nr != nr) { continue; }
        auto verified = table.verify(unpack_handle(handle), op.required_rights, op.expected_type);
        if (verified.is_err()) { return errc_of(verified.unwrap_err()); }
        op_context ctx{table, unpack_handle(handle), verified.unwrap(), arg};
        return op.handler(ctx);
    }
    return errc_of(ktl::errc::invalid_operation);
}

}  // namespace kernel::obj
