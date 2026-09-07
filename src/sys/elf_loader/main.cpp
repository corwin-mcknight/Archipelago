#include <abi/syscall.h>
#include <elf/loader.h>
#include <elf/protocol.h>
#include <sys.h>

namespace {
constexpr uint64_t HANDLES_AT = 256;
constexpr uint64_t MESSAGE_AT = 512;
constexpr uint64_t STACK_SIZE = 4 * ABI_VM_PAGE_SIZE;
uint64_t self_task;
uint64_t thread_factory;

struct Work {
    uint64_t factory;
    uint64_t image;
    uint64_t image_size;
    uint64_t status;
    uint64_t handles[2];
    size_t name_size;
    char name[ABI_COORD_NAME_MAX];
    bool completed;
};
}  // namespace

extern "C" void loader_worker_entry();
extern "C" [[noreturn]] void loader_worker(char* ipc, size_t, Work* work) {
    // The CRT's default IPC pointer belongs to the main thread. Workers use their own entry
    // arguments explicitly; all wrappers below only marshal registers, without shared staging.
    work->status =
        elf::spawn(work->factory, work->image, work->image_size, work->name, work->name_size, HANDLES_AT, ipc);
    if (!sys_is_error(work->status)) { __builtin_memcpy(work->handles, ipc + HANDLES_AT, sizeof(work->handles)); }
    __atomic_store_n(&work->completed, true, __ATOMIC_RELEASE);
    sys_thread_exit();
}

namespace {
bool execute(Work& work) {
    bool healthy     = true;
    work.completed   = false;
    work.status      = static_cast<uint64_t>(ABI_ERR_INVALID_OPERATION);
    uint64_t backing = sys_vmo_create(STACK_SIZE);
    if (sys_is_error(backing)) {
        work.status = backing;
        return true;
    }
    uint64_t stack = sys_vmo_map(backing, 0, 0, STACK_SIZE, ABI_VM_PROT_READ | ABI_VM_PROT_WRITE);
    (void)sys_handle_close(backing);
    if (sys_is_error(stack)) {
        work.status = stack;
        return true;
    }
    uintptr_t top                  = stack + STACK_SIZE - 16;
    *reinterpret_cast<Work**>(top) = &work;
    uint64_t thread = sys_thread_start(thread_factory, reinterpret_cast<uintptr_t>(loader_worker_entry), top);
    if (sys_is_error(thread)) {
        work.status = thread;
    } else {
        uint64_t waited = sys_object_wait(thread, ABI_THREAD_SIGNAL_TERMINATED, 0);
        if (sys_is_error(waited)) {
            // The stack and Work must outlive the worker. A broken join is fatal to the loader.
            (void)sys_task_kill(self_task);
            sys_thread_exit();
        }
        (void)sys_handle_close(thread);
        if (!__atomic_load_n(&work.completed, __ATOMIC_ACQUIRE)) {
            healthy     = false;
            work.status = static_cast<uint64_t>(ABI_ERR_INVALID_OPERATION);
        }
    }
    (void)sys_vmo_unmap(stack);
    return healthy;
}

uint64_t receive() { return elf::receive(ABI_BOOTSTRAP_HANDLE, MESSAGE_AT, 256, HANDLES_AT, 4); }
}  // namespace

extern "C" int main() {
    uint64_t bootstrap = receive();
    if (sys_is_error(bootstrap) || (bootstrap >> 32) != 3) { return 1; }
    sys_copy_in(&self_task, HANDLES_AT, sizeof(self_task));
    sys_copy_in(&thread_factory, HANDLES_AT + 2 * sizeof(uint64_t), sizeof(thread_factory));
    uint64_t initial_thread;
    sys_copy_in(&initial_thread, HANDLES_AT + sizeof(uint64_t), sizeof(initial_thread));
    (void)sys_handle_close(initial_thread);
    uint64_t endowed = receive();
    abi_message_header header{};
    sys_copy_in(&header, MESSAGE_AT, sizeof(header));
    if (sys_is_error(endowed) || (endowed >> 32) != 1 || (endowed & 0xffffffff) != sizeof(header) ||
        header.opcode != ELF_LOADER_AUTHORITY) {
        return 2;
    }
    uint64_t factory;
    sys_copy_in(&factory, HANDLES_AT, sizeof(factory));
    for (;;) {
        uint64_t received = receive();
        if (sys_is_error(received)) { return 0; }
        size_t size            = received & 0xffffffff;
        constexpr size_t FIXED = sizeof(abi_message_header) + sizeof(abi_image_payload);
        if ((received >> 32) != 1 || size <= FIXED || size > FIXED + ABI_COORD_NAME_MAX) {
            sys_close_arrived(received, HANDLES_AT);
            continue;
        }
        sys_copy_in(&header, MESSAGE_AT, sizeof(header));
        if (header.opcode != ELF_LOADER_LOAD) {
            sys_close_arrived(received, HANDLES_AT);
            continue;
        }
        Work work{};
        work.factory = factory;
        sys_copy_in(&work.image, HANDLES_AT, sizeof(work.image));
        abi_image_payload payload;
        sys_copy_in(&payload, MESSAGE_AT + sizeof(header), sizeof(payload));
        work.image_size = payload.size_bytes;
        work.name_size  = size - FIXED;
        sys_copy_in(work.name, MESSAGE_AT + FIXED, work.name_size);
        bool healthy = execute(work);
        (void)sys_handle_close(work.image);
        header.status = static_cast<uint32_t>(work.status);
        sys_copy_out(0, &header, sizeof(header));
        uint64_t count  = sys_is_error(work.status) ? 0 : 2;
        uint64_t keeper = count ? sys_handle_duplicate(work.handles[0], ABI_RIGHT_WRITE) : UINT64_MAX;
        if (count && sys_is_error(keeper)) {
            (void)sys_task_kill(work.handles[0]);
            (void)sys_handle_close(work.handles[0]);
            (void)sys_handle_close(work.handles[1]);
            header.status = static_cast<uint32_t>(keeper);
            sys_copy_out(0, &header, sizeof(header));
            count = 0;
        }
        sys_copy_out(HANDLES_AT, work.handles, count * sizeof(uint64_t));
        uint64_t sent = sys_channel_send(ABI_BOOTSTRAP_HANDLE, 0, sizeof(header), HANDLES_AT, count);
        if (count && !sys_is_error(keeper)) {
            if (sys_is_error(sent)) { (void)sys_task_kill(keeper); }
            (void)sys_handle_close(keeper);
        }
        if (sys_is_error(sent)) { return 3; }
        // A faulted worker may have died between acquiring a task-wide handle/mapping and
        // recording it. Terminating the service reclaims those resources as well as its pending
        // construction. Loader restart/supervision is a separate coordinator policy.
        if (!healthy) { return 4; }
    }
}
