#include <kernel/log.h>
#include <kernel/obj/channel.h>
#include <kernel/obj/handle_dispatch.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>
#include <kernel/synchronization/execution_context.h>
#include <kernel/syscall.h>

namespace {

// ponytail: one global line buffer; per-task buffers when concurrent user tasks interleave output.
constexpr size_t k_debug_line_max = 120;
char g_debug_line[k_debug_line_max + 1];
size_t g_debug_len = 0;

void log_putc(char c) {
    if (c != '\n') {
        g_debug_line[g_debug_len++] = c;
        if (g_debug_len < k_debug_line_max) { return; }
    }
    g_debug_line[g_debug_len] = '\0';
    g_log.info("user: {0}", static_cast<const char*>(g_debug_line));
    g_debug_len = 0;
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

// Copy kernel bytes into a pre-validated IPC buffer range, page run by page run -- the backing
// frames are not physically contiguous. Callers check contains() first. This is the kernel-to-user
// copy-out direction; the user-to-kernel direction lives in sys_channel_send's staging loop.
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

// Channel syscalls take three arguments (handle, offset, length), one more than the declarative
// handle-op table carries, and need the calling thread's IPC buffer, which that pipeline is kept
// free of. They run the same HandleTable::verify checks; only the dispatch is hand-rolled.

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

uint64_t sys_channel_send(uint64_t handle, uint64_t offset, uint64_t length) {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, length)) { return errc_of(ktl::errc::out_of_range); }
    if (length > kernel::obj::Channel::MAX_MESSAGE_BYTES) { return errc_of(ktl::errc::out_of_range); }

    auto task     = calling_task(self);
    auto verified = task->handles().verify(kernel::obj::unpack_handle(handle), kernel::obj::RIGHT_WRITE,
                                           kernel::obj::type_ids::CHANNEL);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }

    // The message page is contiguous, so staging is one memcpy per source page run.
    auto created = kernel::obj::MessageBuffer::create(length);
    if (created.is_err()) { return errc_of(created.unwrap_err()); }
    auto message = created.unwrap();
    for (uint64_t done = 0; done < length;) {
        size_t run       = 0;
        const void* from = reinterpret_cast<const void*>(buffer.kernel_at(offset + done, run));
        uint64_t take    = run < length - done ? run : length - done;
        __builtin_memcpy(message.data() + done, from, take);
        done += take;
    }

    auto channel = ktl::static_ref_cast<kernel::obj::Channel>(verified.unwrap().object);
    auto sent    = channel->write(ktl::move(message));
    return sent.is_ok() ? 0 : errc_of(sent.unwrap_err());
}

// Wait (nonzero mask) or poll (zero mask) on any object's signals; see <abi/syscall.h>. Lives
// beside the channel syscalls rather than in the op table because it blocks, which the
// scheduler-free dispatch pipeline must never do. The verified ref pins the object for the whole
// wait, so a concurrent close of the handle cannot free it out from under the sleeping thread.
uint64_t sys_object_wait(uint64_t handle, uint64_t mask) {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    if ((mask >> 32) != 0) { return errc_of(ktl::errc::out_of_range); }

    auto task     = calling_task(self);
    auto verified = task->handles().verify(kernel::obj::unpack_handle(handle), kernel::obj::RIGHT_WAIT);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }

    auto object = verified.unwrap().object;
    if (mask == 0) { return object->signals(); }
    return object->wait_signals(static_cast<uint32_t>(mask));
}

uint64_t sys_channel_recv(uint64_t handle, uint64_t offset, uint64_t capacity) {
    auto self = kernel::sched::current();
    if (!self) { return errc_of(ktl::errc::invalid_operation); }
    const auto& buffer = self->ipc();
    if (!buffer.valid() || !buffer.contains(offset, capacity)) { return errc_of(ktl::errc::out_of_range); }

    auto task     = calling_task(self);
    auto verified = task->handles().verify(kernel::obj::unpack_handle(handle), kernel::obj::RIGHT_READ,
                                           kernel::obj::type_ids::CHANNEL);
    if (verified.is_err()) { return errc_of(verified.unwrap_err()); }

    auto channel  = ktl::static_ref_cast<kernel::obj::Channel>(verified.unwrap().object);
    auto received = channel->read(capacity);
    if (received.is_err()) { return errc_of(received.unwrap_err()); }
    auto message = received.unwrap();
    if (message.size() != 0) { buffer_write(buffer, offset, message.data(), message.size()); }
    return message.size();
}

}  // namespace

extern "C" uint64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                     uint64_t a5) {
    // a3..a5 are carried by the entry paths but no operation reads past a2 yet.
    (void)a3;
    (void)a4;
    (void)a5;
    kernel::synchronization::syscall_enter();
    uint64_t ret = 0;
    switch (nr) {
        case kernel::syscall::SYS_EXIT:
            kernel::synchronization::syscall_exit();
            kernel::sched::exit_current();
            break;
        case kernel::syscall::SYS_YIELD: kernel::sched::yield(); break;
        case kernel::syscall::SYS_SLEEP: kernel::sched::sleep_ticks(a0); break;
        case kernel::syscall::SYS_WRITE: ret = sys_write(a0, a1); break;
        case kernel::syscall::SYS_HANDLE_CLOSE:
        case kernel::syscall::SYS_HANDLE_DUPLICATE:
        case kernel::syscall::SYS_OBJ_INFO: ret = handle_syscall(nr, a0, a1); break;
        case kernel::syscall::SYS_CHANNEL_CREATE: ret = sys_channel_create(a0); break;
        case kernel::syscall::SYS_CHANNEL_SEND: ret = sys_channel_send(a0, a1, a2); break;
        case kernel::syscall::SYS_CHANNEL_RECV: ret = sys_channel_recv(a0, a1, a2); break;
        case kernel::syscall::SYS_OBJECT_WAIT: ret = sys_object_wait(a0, a1); break;
        default: kernel::synchronization::syscall_exit(); return static_cast<uint64_t>(-1);
    }
    kernel::synchronization::syscall_exit();
    return ret;
}
