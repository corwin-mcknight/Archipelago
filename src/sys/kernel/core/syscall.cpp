#include <kernel/log.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/mm/vmo.h>
#include <kernel/obj/channel.h>
#include <kernel/obj/handle_dispatch.h>
#include <kernel/obj/port.h>
#include <kernel/obj/socket.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>
#include <kernel/sched/user_task.h>
#include <kernel/synchronization/execution_context.h>
#include <kernel/syscall.h>

namespace {

// ponytail: one global line buffer; per-task buffers when concurrent user tasks interleave output.
constexpr size_t k_debug_line_max = 120;
char g_debug_line[k_debug_line_max + 1];
size_t g_debug_len = 0;

void log_flush_line() {
    g_debug_line[g_debug_len] = '\0';
    g_log.info("user: {0}", static_cast<const char*>(g_debug_line));
    g_debug_len = 0;
}

// Every exit leaves g_debug_len < k_debug_line_max, so the append below is always in bounds even
// when two threads' writes interleave at a flush boundary.
void log_putc(char c) {
    if (c == '\n') {
        log_flush_line();
        return;
    }
    g_debug_line[g_debug_len++] = c;
    if (g_debug_len == k_debug_line_max) { log_flush_line(); }
}

// Emit [offset, offset + length) of the calling thread's IPC buffer. Output goes through the log
// rather than straight at the UART so it stays serialized against kernel log output and the test
// harness's protocol lines, which share the device.
//
// No user pointer is involved: the buffer's frames were resolved when the thread was created, so
// this validates a range against a size the kernel already knows and then reads its own physmap.
uint64_t sys_write(uint64_t offset, uint64_t length) {
    auto self = kernel::sched::current();
    if (!self) { return static_cast<uint64_t>(ktl::errc::invalid_operation); }

    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, length)) { return static_cast<uint64_t>(ktl::errc::out_of_range); }

    // Page by page: the backing frames are not physically contiguous, so a multi-page buffer is
    // several runs. A one-page buffer is a single pass.
    uint64_t written = 0;
    while (written < length) {
        size_t run         = 0;
        const char* chunk  = reinterpret_cast<const char*>(buffer.kernel_at(offset + written, run));
        uint64_t remaining = length - written;
        uint64_t take      = run < remaining ? run : remaining;
        for (uint64_t i = 0; i < take; ++i) { log_putc(chunk[i]); }
        written += take;
    }
    return written;
}

// A handle names an entry in the calling task's table, so resolve the caller before entering the
// pipeline. Kernel threads land on task zero's table, which is what lets kernel-context tests
// drive the real path end to end.
ktl::ref<kernel::sched::Task> calling_task(ktl::ref<kernel::sched::Thread>& self) {
    auto task = ktl::static_ref_cast<kernel::sched::Task>(self->owner());
    if (!task) { task = kernel::sched::kernel_task(); }
    return task;
}

uint64_t handle_syscall(uint64_t nr, uint64_t a0, uint64_t a1) {
    auto self = kernel::sched::current();
    if (!self) { return static_cast<uint64_t>(ktl::errc::invalid_operation); }
    auto task = calling_task(self);
    return kernel::obj::dispatch_handle_op(task->handles(), nr, a0, a1);
}

uint64_t errc_of(ktl::errc error) { return static_cast<uint64_t>(error); }

// The installed error codes in <abi/syscall.h> are spelled as literals there; pin each to its
// kernel-internal value so the two cannot drift.
static_assert(static_cast<int64_t>(ktl::errc::truncated) == ::abi::syscall::ERR_TRUNCATED);
static_assert(static_cast<int64_t>(ktl::errc::would_block) == ::abi::syscall::ERR_WOULD_BLOCK);
static_assert(static_cast<int64_t>(ktl::errc::peer_closed) == ::abi::syscall::ERR_PEER_CLOSED);
static_assert(static_cast<int64_t>(ktl::errc::timed_out) == ::abi::syscall::ERR_TIMED_OUT);

// Copy between a pre-validated IPC buffer range and contiguous kernel memory, page run by page
// run -- the backing frames are not physically contiguous. Callers check contains() first.
void buffer_write(const kernel::sched::ipc_buffer& buffer, uint64_t offset, const void* src, size_t length) {
    size_t done = 0;
    while (done < length) {
        size_t run  = 0;
        void* to    = reinterpret_cast<void*>(buffer.kernel_at(offset + done, run));
        size_t take = run < length - done ? run : length - done;
        __builtin_memcpy(to, static_cast<const uint8_t*>(src) + done, take);
        done += take;
    }
}

void buffer_read(const kernel::sched::ipc_buffer& buffer, uint64_t offset, void* dst, size_t length) {
    size_t done = 0;
    while (done < length) {
        size_t run       = 0;
        const void* from = reinterpret_cast<const void*>(buffer.kernel_at(offset + done, run));
        size_t take      = run < length - done ? run : length - done;
        __builtin_memcpy(static_cast<uint8_t*>(dst) + done, from, take);
        done += take;
    }
}

// Channel syscalls take up to five arguments, more than the declarative handle-op table carries,
// and need the calling thread's IPC buffer, which that pipeline is kept free of. They run the
// same HandleTable::verify checks; only the dispatch is hand-rolled.

uint64_t sys_channel_create(uint64_t offset) {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, 2 * sizeof(uint64_t))) { return errc_of(ktl::errc::out_of_range); }

    auto created = kernel::obj::Channel::create();
    if (created.is_err()) { return errc_of(created.unwrap_err()); }
    auto pair  = created.unwrap();

    auto task  = calling_task(self);
    auto first = task->handles().insert(pair.first, kernel::obj::Channel::DEFAULT_RIGHTS);
    if (first.is_err()) { return errc_of(first.unwrap_err()); }
    auto second = task->handles().insert(pair.second, kernel::obj::Channel::DEFAULT_RIGHTS);
    if (second.is_err()) {
        (void)task->handles().close(first.unwrap());
        return errc_of(second.unwrap_err());
    }

    uint64_t handles[2] = {kernel::obj::pack_handle(first.unwrap()), kernel::obj::pack_handle(second.unwrap())};
    buffer_write(buffer, offset, handles, sizeof(handles));
    return 0;
}

uint64_t sys_channel_send(uint64_t handle, uint64_t offset, uint64_t length, uint64_t handles_offset,
                          uint64_t handle_count) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, length)) { return errc_of(ktl::errc::out_of_range); }
    if (length > Channel::MAX_MESSAGE_BYTES) { return errc_of(ktl::errc::out_of_range); }
    if (handle_count > MessageBuffer::MAX_HANDLES) { return errc_of(ktl::errc::out_of_range); }
    if (handle_count != 0 && !buffer.contains(handles_offset, handle_count * sizeof(uint64_t))) {
        return errc_of(ktl::errc::out_of_range);
    }

    // Sending handles is gated by the transfer right on the channel handle itself.
    auto task     = calling_task(self);
    Rights needed = RIGHT_WRITE | (handle_count != 0 ? RIGHT_TRANSFER : 0);
    auto verified = task->handles().verify(unpack_handle(handle), needed, type_ids::CHANNEL);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }
    auto channel = ktl::static_ref_cast<Channel>(verified.unwrap().object);

    // Once escrow begins, the handles belong to the message: any later failure closes them (see
    // <abi/syscall.h>). So fail the ordinary flow-control cases -- full queue, dead peer -- before
    // consuming anything, by the same signals a waiting sender uses. A racing writer on this same
    // endpoint can still fill the queue between this check and the write; that residual race is
    // what the close-on-failure rule is for.
    if (handle_count != 0) {
        uint32_t signals = channel->signals();
        if ((signals & Channel::SIGNAL_PEER_CLOSED) != 0) { return errc_of(ktl::errc::peer_closed); }
        if ((signals & Channel::SIGNAL_WRITABLE) == 0) { return errc_of(ktl::errc::capacity_exhausted); }
    }

    // The message page is contiguous, so staging is one memcpy per source page run.
    auto created = MessageBuffer::create(length);
    if (created.is_err()) { return errc_of(created.unwrap_err()); }
    auto message = created.unwrap();
    if (length != 0) { buffer_read(buffer, offset, message.data(), length); }

    uint64_t handle_values[MessageBuffer::MAX_HANDLES];
    if (handle_count != 0) { buffer_read(buffer, handles_offset, handle_values, handle_count * sizeof(uint64_t)); }
    for (uint64_t i = 0; i < handle_count; i++) {
        auto taken = task->handles().take(unpack_handle(handle_values[i]));
        if (taken.is_err()) { return errc_of(taken.unwrap_err()); }
        auto moved = taken.unwrap();
        // The channel handle itself is the one cycle cheap to refuse: escrowed on its own queue it
        // could never be read again, and the escrow reference would keep the queue alive forever.
        if (moved.object.get() == channel.get()) { return errc_of(ktl::errc::invalid_operation); }
        auto escrowed = kernel::sched::kernel_task()->handles().insert(ktl::move(moved.object), moved.rights);
        if (escrowed.is_err()) { return errc_of(escrowed.unwrap_err()); }
        message.attach_handle(escrowed.unwrap());
    }

    auto sent = channel->write(ktl::move(message));
    return sent.is_ok() ? 0 : errc_of(sent.unwrap_err());
}

// Wait (nonzero mask) or poll (zero mask) on any object's signals; see <abi/syscall.h>. Lives
// beside the channel syscalls rather than in the op table because it blocks, which the
// scheduler-free dispatch pipeline must never do. The verified ref pins the object for the whole
// wait, so a concurrent close of the handle cannot free it out from under the sleeping thread.
uint64_t sys_object_wait(uint64_t handle, uint64_t mask, uint64_t timeout_ns) {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    if ((mask >> 32) != 0) { return errc_of(ktl::errc::out_of_range); }

    auto task     = calling_task(self);
    auto verified = task->handles().verify(kernel::obj::unpack_handle(handle), kernel::obj::RIGHT_WAIT);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }

    auto object = verified.unwrap().object;
    if (mask == 0) { return object->signals(); }
    if (timeout_ns == 0) { return object->wait_signals(static_cast<uint32_t>(mask)); }

    // The timeout rounds up to ticks and the deadline saturates: a huge value waits forever
    // rather than wrapping to a deadline already in the past.
    ktime_t now      = kernel::time::now();
    ktime_t ticks    = kernel::time::ns_to_ticks_ceil(timeout_ns);
    ktime_t deadline = (now + ticks < now) ? UINT64_MAX : now + ticks;
    uint32_t got     = object->wait_signals_deadline(static_cast<uint32_t>(mask), deadline);
    if ((got & mask) == 0) { return errc_of(ktl::errc::timed_out); }
    return got;
}

uint64_t sys_channel_recv(uint64_t handle, uint64_t offset, uint64_t capacity, uint64_t handles_offset,
                          uint64_t handle_capacity) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, capacity)) { return errc_of(ktl::errc::out_of_range); }
    if (handle_capacity > MessageBuffer::MAX_HANDLES) { return errc_of(ktl::errc::out_of_range); }
    if (handle_capacity != 0 && !buffer.contains(handles_offset, handle_capacity * sizeof(uint64_t))) {
        return errc_of(ktl::errc::out_of_range);
    }

    auto task     = calling_task(self);
    auto verified = task->handles().verify(unpack_handle(handle), RIGHT_READ, type_ids::CHANNEL);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }

    auto channel  = ktl::static_ref_cast<Channel>(verified.unwrap().object);
    auto received = channel->read(capacity, handle_capacity);
    if (received.is_err()) { return errc_of(received.unwrap_err()); }
    auto message = received.unwrap();
    if (message.size() != 0) { buffer_write(buffer, offset, message.data(), message.size()); }

    // Move each handle out of escrow into the caller's table. An insert the receiver's table
    // cannot make (OOM) closes that handle -- the message is already dequeued, so the returned
    // count is how the caller learns what actually arrived.
    HandleId escrowed[MessageBuffer::MAX_HANDLES];
    size_t count = message.detach_handles(escrowed);
    uint64_t handle_values[MessageBuffer::MAX_HANDLES];
    uint64_t delivered = 0;
    for (size_t i = 0; i < count; i++) {
        auto taken = kernel::sched::kernel_task()->handles().take(escrowed[i]);
        if (taken.is_err()) { continue; }
        auto moved    = taken.unwrap();
        auto inserted = task->handles().insert(ktl::move(moved.object), moved.rights);
        if (inserted.is_err()) { continue; }
        handle_values[delivered++] = pack_handle(inserted.unwrap());
    }
    if (delivered != 0) { buffer_write(buffer, handles_offset, handle_values, delivered * sizeof(uint64_t)); }
    return (delivered << 32) | message.size();
}

// Ports follow the channel syscalls' shape: hand-rolled dispatch, the same verify pipeline, and
// the IPC buffer as the only memory crossing the boundary.

uint64_t sys_port_create() {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    auto task    = calling_task(self);
    auto created = task->handles().emplace<kernel::obj::Port>(kernel::obj::Port::DEFAULT_RIGHTS);
    if (created.is_err()) { return errc_of(created.unwrap_err()); }
    return kernel::obj::pack_handle(created.unwrap());
}

uint64_t sys_port_bind(uint64_t port_handle, uint64_t object_handle, uint64_t key, uint64_t mask) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    if (mask == 0 || (mask >> 32) != 0) { return errc_of(ktl::errc::out_of_range); }

    auto task = calling_task(self);
    auto port = task->handles().get<Port>(unpack_handle(port_handle), RIGHT_WRITE);
    if (port.is_err()) { return errc_of(port.unwrap_err()); }
    // The wait right on the object gates the subscription, same as a direct wait would need.
    auto object = task->handles().verify(unpack_handle(object_handle), RIGHT_WAIT);
    if (object.is_err()) { return errc_of(object.unwrap_err()); }

    auto bound = port.unwrap()->bind(ktl::move(object.unwrap().object), key, static_cast<uint32_t>(mask));
    return bound.is_ok() ? 0 : errc_of(bound.unwrap_err());
}

uint64_t sys_port_unbind(uint64_t port_handle, uint64_t key) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    auto task = calling_task(self);
    auto port = task->handles().get<Port>(unpack_handle(port_handle), RIGHT_WRITE);
    if (port.is_err()) { return errc_of(port.unwrap_err()); }
    return port.unwrap()->unbind(key);
}

uint64_t sys_port_wait(uint64_t port_handle, uint64_t offset, uint64_t timeout_ns) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, 2 * sizeof(uint64_t))) { return errc_of(ktl::errc::out_of_range); }

    auto task    = calling_task(self);
    auto claimed = task->handles().get<Port>(unpack_handle(port_handle), RIGHT_READ | RIGHT_WAIT);
    if (claimed.is_err()) { return errc_of(claimed.unwrap_err()); }
    auto port        = claimed.unwrap();

    ktime_t deadline = 0;
    if (timeout_ns != 0) {
        ktime_t now   = kernel::time::now();
        ktime_t ticks = kernel::time::ns_to_ticks_ceil(timeout_ns);
        deadline      = (now + ticks < now) ? UINT64_MAX : now + ticks;
    }

    // Dequeue-then-wait loop: READABLE can flicker high on a drained queue (see Port::dequeue),
    // so an empty dequeue after a wake just waits again with the same deadline. A killed thread
    // must leave the loop -- its waits return immediately without the signal, and the dispatcher's
    // boundary check exits it as soon as this returns.
    Port::Packet packet;
    while (!port->dequeue(packet)) {
        if (self->killed()) { return errc_of(ktl::errc::timed_out); }
        if (timeout_ns == 0) {
            (void)port->wait_signals(Port::SIGNAL_READABLE);
            continue;
        }
        uint32_t got = port->wait_signals_deadline(Port::SIGNAL_READABLE, deadline);
        if ((got & Port::SIGNAL_READABLE) == 0) { return errc_of(ktl::errc::timed_out); }
    }
    uint64_t out[2] = {packet.key, packet.signals};
    buffer_write(buffer, offset, out, sizeof(out));
    return 0;
}

// Spawn rides beside the channel syscalls for the same reason they do: it returns two handles
// through the IPC buffer, which the declarative op table is kept free of. task_spawn is the
// buffer-free core; this wrapper is only verification and copy-out.
uint64_t sys_task_spawn(uint64_t handle, uint64_t offset) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, 2 * sizeof(uint64_t))) { return errc_of(ktl::errc::out_of_range); }

    auto task     = calling_task(self);
    auto verified = task->handles().verify(unpack_handle(handle), RIGHT_READ, type_ids::VMO);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }
    auto image   = ktl::static_ref_cast<kernel::mm::vmo>(verified.unwrap().object);

    auto spawned = kernel::sched::task_spawn(*task, ktl::move(image));
    if (spawned.is_err()) { return errc_of(spawned.unwrap_err()); }

    uint64_t handles[2] = {pack_handle(spawned.unwrap().task), pack_handle(spawned.unwrap().mailbox)};
    buffer_write(buffer, offset, handles, sizeof(handles));
    return 0;
}

// Socket syscalls follow the channel shape: hand-rolled dispatch, the same verify pipeline, the
// IPC buffer as the only memory crossing the boundary. Bytes move ring-to-buffer in page runs,
// so a partial result mid-walk is returned as the count, never rewound.

uint64_t sys_socket_create(uint64_t offset) {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, 2 * sizeof(uint64_t))) { return errc_of(ktl::errc::out_of_range); }

    auto created = kernel::obj::Socket::create();
    if (created.is_err()) { return errc_of(created.unwrap_err()); }
    auto pair  = created.unwrap();

    auto task  = calling_task(self);
    auto first = task->handles().insert(pair.first, kernel::obj::Socket::DEFAULT_RIGHTS);
    if (first.is_err()) { return errc_of(first.unwrap_err()); }
    auto second = task->handles().insert(pair.second, kernel::obj::Socket::DEFAULT_RIGHTS);
    if (second.is_err()) {
        (void)task->handles().close(first.unwrap());
        return errc_of(second.unwrap_err());
    }

    uint64_t handles[2] = {kernel::obj::pack_handle(first.unwrap()), kernel::obj::pack_handle(second.unwrap())};
    buffer_write(buffer, offset, handles, sizeof(handles));
    return 0;
}

uint64_t sys_socket_write(uint64_t handle, uint64_t offset, uint64_t length) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, length)) { return errc_of(ktl::errc::out_of_range); }

    auto task     = calling_task(self);
    auto verified = task->handles().verify(unpack_handle(handle), RIGHT_WRITE, type_ids::SOCKET);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }
    auto socket   = ktl::static_ref_cast<Socket>(verified.unwrap().object);

    // Page run by page run; a short acceptance mid-walk is backpressure, reported as the count.
    // An error with bytes already accepted is likewise the count -- those bytes are delivered.
    uint64_t done = 0;
    while (done < length) {
        size_t run       = 0;
        const void* from = reinterpret_cast<const void*>(buffer.kernel_at(offset + done, run));
        size_t attempt   = run < length - done ? run : length - done;
        auto wrote       = socket->write(from, attempt);
        if (wrote.is_err()) { return done != 0 ? done : errc_of(wrote.unwrap_err()); }
        done += wrote.unwrap();
        if (wrote.unwrap() < attempt) { break; }
    }
    return done;
}

uint64_t sys_socket_read(uint64_t handle, uint64_t offset, uint64_t capacity) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, capacity)) { return errc_of(ktl::errc::out_of_range); }

    auto task     = calling_task(self);
    auto verified = task->handles().verify(unpack_handle(handle), RIGHT_READ, type_ids::SOCKET);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }
    auto socket   = ktl::static_ref_cast<Socket>(verified.unwrap().object);

    uint64_t done = 0;
    while (done < capacity) {
        size_t run   = 0;
        void* to     = reinterpret_cast<void*>(buffer.kernel_at(offset + done, run));
        size_t space = run < capacity - done ? run : capacity - done;
        auto got     = socket->read(to, space);
        if (got.is_err()) { return done != 0 ? done : errc_of(got.unwrap_err()); }
        done += got.unwrap();
        if (got.unwrap() < space) { break; }
    }
    return done;
}

// User-controlled memory (<abi/syscall.h>): the mm layer is already task-shaped -- per-task
// region trees, VMO lifetime by ref -- so these are verification plus dispatch. Mapping into the
// root region with no reserved ranges is deliberate: a task that unmaps its own stack or text
// only faults itself, and the IPC buffer's frames are pinned by the ipc_buffer's own ref.

// Where the kernel-picked search starts: clear of where images load, the stack, and the
// IPC-buffer window. A hint, not a reservation -- explicit maps may land below it.
constexpr uintptr_t USER_MAP_FLOOR = 0x20000000;

static_assert(::abi::syscall::VM_PAGE_SIZE == KERNEL_MINIMUM_PAGE_SIZE);
static_assert(::abi::syscall::VM_PROT_READ == kernel::mm::vm_prot::READ &&
              ::abi::syscall::VM_PROT_WRITE == kernel::mm::vm_prot::WRITE);

uint64_t sys_vmo_create(uint64_t size) {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    if (size == 0 || (size % KERNEL_MINIMUM_PAGE_SIZE) != 0) { return errc_of(ktl::errc::invalid_operation); }

    auto object = kernel::mm::create_anonymous_vmo(size / KERNEL_MINIMUM_PAGE_SIZE);
    if (object.get() == nullptr) { return errc_of(ktl::errc::oom); }

    auto task     = calling_task(self);
    auto inserted = task->handles().insert(ktl::move(object), kernel::obj::RIGHT_READ | kernel::obj::RIGHT_WRITE);
    if (inserted.is_err()) { return errc_of(inserted.unwrap_err()); }
    return kernel::obj::pack_handle(inserted.unwrap());
}

uint64_t sys_vmo_map(uint64_t handle, uint64_t vaddr, uint64_t vmo_offset, uint64_t length, uint64_t prot) {
    using namespace kernel::obj;
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    // EXEC (or any unknown bit) is rejected outright: user-minted executable mappings wait for
    // the userspace-loader milestone, which is what keeps W^X trivially true.
    constexpr uint64_t MAPPABLE = ::abi::syscall::VM_PROT_READ | ::abi::syscall::VM_PROT_WRITE;
    if (prot == 0 || (prot & ~MAPPABLE) != 0) { return errc_of(ktl::errc::invalid_operation); }

    auto task    = calling_task(self);
    auto* aspace = task->aspace();
    if (aspace == nullptr) { return errc_of(ktl::errc::invalid_operation); }

    // Each requested protection needs the matching right on the handle.
    Rights needed = ((prot & ::abi::syscall::VM_PROT_READ) != 0 ? RIGHT_READ : 0) |
                    ((prot & ::abi::syscall::VM_PROT_WRITE) != 0 ? RIGHT_WRITE : 0);
    auto verified = task->handles().verify(unpack_handle(handle), needed, type_ids::VMO);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }
    auto object = ktl::static_ref_cast<kernel::mm::vmo>(verified.unwrap().object);

    auto kprot  = static_cast<kernel::mm::vm_prot_t>(prot) | kernel::mm::vm_prot::USER;
    if (vaddr == 0) {
        auto mapped = aspace->root().map_anywhere(USER_MAP_FLOOR, length, ktl::move(object), vmo_offset, kprot);
        if (mapped.is_err()) { return errc_of(mapped.unwrap_err()); }
        return mapped.unwrap();
    }
    auto mapped = aspace->root().map(vaddr, length, ktl::move(object), vmo_offset, kprot);
    if (mapped.is_err()) { return errc_of(mapped.unwrap_err()); }
    return vaddr;
}

uint64_t sys_vmo_unmap(uint64_t vaddr) {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    auto task    = calling_task(self);
    auto* aspace = task->aspace();
    if (aspace == nullptr) { return errc_of(ktl::errc::invalid_operation); }

    auto removed = aspace->root().unmap_binding(vaddr);
    return removed.is_ok() ? 0 : errc_of(removed.unwrap_err());
}

}  // namespace

extern "C" uint64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                     uint64_t a5) {
    // a5 is carried by the entry paths but no operation reads past a4 yet.
    (void)a5;
    kernel::synchronization::syscall_enter();
    uint64_t ret = 0;
    switch (nr) {
        case kernel::syscall::SYS_EXIT: {
            // Record the status before the thread is unreachable. Task zero records nothing: a
            // kernel-context test driving SYS_EXIT is exiting a thread, not ending the kernel.
            // The refs are scoped: exit_current() abandons this stack without unwinding, so a
            // ref still live here would leak the Thread and pin its Task forever.
            {
                auto self = kernel::sched::current();
                if (self) {
                    auto task = calling_task(self);
                    if (task.get() != kernel::sched::kernel_task().get()) {
                        task->record_exit(kernel::syscall::TASK_EXIT_EXITED, static_cast<uint32_t>(a0));
                    }
                }
            }
            kernel::synchronization::syscall_exit();
            kernel::sched::exit_current();
            break;
        }
        case kernel::syscall::SYS_YIELD: kernel::sched::yield(); break;
        case kernel::syscall::SYS_SLEEP: kernel::sched::sleep_ticks(a0); break;
        case kernel::syscall::SYS_WRITE: ret = sys_write(a0, a1); break;
        case kernel::syscall::SYS_HANDLE_CLOSE:
        case kernel::syscall::SYS_HANDLE_DUPLICATE:
        case kernel::syscall::SYS_OBJ_INFO:
        case kernel::syscall::SYS_TASK_KILL:
        case kernel::syscall::SYS_TASK_STATUS: ret = handle_syscall(nr, a0, a1); break;
        case kernel::syscall::SYS_CHANNEL_CREATE: ret = sys_channel_create(a0); break;
        case kernel::syscall::SYS_CHANNEL_SEND: ret = sys_channel_send(a0, a1, a2, a3, a4); break;
        case kernel::syscall::SYS_CHANNEL_RECV: ret = sys_channel_recv(a0, a1, a2, a3, a4); break;
        case kernel::syscall::SYS_OBJECT_WAIT: ret = sys_object_wait(a0, a1, a2); break;
        case kernel::syscall::SYS_PORT_CREATE: ret = sys_port_create(); break;
        case kernel::syscall::SYS_PORT_BIND: ret = sys_port_bind(a0, a1, a2, a3); break;
        case kernel::syscall::SYS_PORT_UNBIND: ret = sys_port_unbind(a0, a1); break;
        case kernel::syscall::SYS_PORT_WAIT: ret = sys_port_wait(a0, a1, a2); break;
        case kernel::syscall::SYS_TASK_SPAWN: ret = sys_task_spawn(a0, a1); break;
        case kernel::syscall::SYS_VMO_CREATE: ret = sys_vmo_create(a0); break;
        case kernel::syscall::SYS_VMO_MAP: ret = sys_vmo_map(a0, a1, a2, a3, a4); break;
        case kernel::syscall::SYS_VMO_UNMAP: ret = sys_vmo_unmap(a0); break;
        case kernel::syscall::SYS_SOCKET_CREATE: ret = sys_socket_create(a0); break;
        case kernel::syscall::SYS_SOCKET_WRITE: ret = sys_socket_write(a0, a1, a2); break;
        case kernel::syscall::SYS_SOCKET_READ: ret = sys_socket_read(a0, a1, a2); break;
        // Unknown numbers fall through to the kill boundary like every other exit path -- an
        // early return here would let a killed thread slip back to user code.
        default: ret = static_cast<uint64_t>(-1); break;
    }
    // The kill boundary: a thread marked while it was in (or entering) this syscall exits here
    // instead of returning to user code. Every kernel lock is released by now, which is what makes
    // this the one safe place to exit a thread that was interrupted mid-operation. The ref is
    // dropped before exit_current() abandons this stack (see SYS_EXIT above).
    {
        bool exit_now = false;
        {
            auto self = kernel::sched::current();
            exit_now  = self && self->killed();
        }
        if (exit_now) {
            kernel::synchronization::syscall_exit();
            kernel::sched::exit_current();
        }
    }
    kernel::synchronization::syscall_exit();
    return ret;
}
