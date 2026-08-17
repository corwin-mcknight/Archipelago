#include <abi/syscall.h>
#include <sys.h>

// One assembly block for every syscall. Wrappers below pass explicit zeros for arguments they do
// not use, rather than leaving those registers holding whatever the last call put there -- the
// kernel ignores unread arguments today, but a syscall that starts reading one should not see
// leftovers from an unrelated call.
static uint64_t syscall6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t ret;
#if defined(__x86_64__)
    // SYSCALL itself destroys rcx and r11 (it parks the return address and flags there), so those
    // are gone regardless of what the kernel does. Every other caller-saved register survives: the
    // entry stub pushes and restores them so user mode never sees leftover kernel values. r10
    // carries the fourth argument because rcx is unavailable.
    register uint64_t rdi __asm__("rdi") = a0;
    register uint64_t rsi __asm__("rsi") = a1;
    register uint64_t rdx __asm__("rdx") = a2;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8")   = a4;
    register uint64_t r9 __asm__("r9")   = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
#elif defined(__riscv)
    register uint64_t r_a7 __asm__("a7") = nr;
    register uint64_t r_a0 __asm__("a0") = a0;
    register uint64_t r_a1 __asm__("a1") = a1;
    register uint64_t r_a2 __asm__("a2") = a2;
    register uint64_t r_a3 __asm__("a3") = a3;
    register uint64_t r_a4 __asm__("a4") = a4;
    register uint64_t r_a5 __asm__("a5") = a5;
    __asm__ volatile("ecall"
                     : "+r"(r_a0)
                     : "r"(r_a7), "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a4), "r"(r_a5)
                     : "memory");
    ret = r_a0;
#endif
    return ret;
}

static uint64_t syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    return syscall6(nr, a0, a1, a2, 0, 0, 0);
}
static uint64_t syscall2(uint64_t nr, uint64_t a0, uint64_t a1) { return syscall6(nr, a0, a1, 0, 0, 0, 0); }
static uint64_t syscall1(uint64_t nr, uint64_t a0) { return syscall6(nr, a0, 0, 0, 0, 0, 0); }

static char* g_ipc_base;
static size_t g_ipc_size;

char* sys_ipc_base(void) { return g_ipc_base; }
size_t sys_ipc_size(void) { return g_ipc_size; }

// Entry glue: _start lands here with the registers the kernel set at thread entry. Stash the IPC
// buffer before anything can want it, run the program, and exit with main's status when it
// returns -- widened as an unsigned int, so -1 reads back as 0xFFFFFFFF, not a kernel error.
extern int main(void);
void __crt_start(char* ipc_base, size_t ipc_size);
void __crt_start(char* ipc_base, size_t ipc_size) {
    g_ipc_base = ipc_base;
    g_ipc_size = ipc_size;
    sys_exit((unsigned int)main());
}

void sys_exit(uint64_t status) { syscall1(ABI_SYS_EXIT, status); }
void sys_yield(void) { syscall1(ABI_SYS_YIELD, 0); }
void sys_sleep(uint64_t ticks) { syscall1(ABI_SYS_SLEEP, ticks); }

int sys_is_error(uint64_t ret) { return (int64_t)ret < 0; }
uint64_t sys_handle_close(uint64_t handle) { return syscall1(ABI_SYS_HANDLE_CLOSE, handle); }
uint64_t sys_handle_duplicate(uint64_t handle, uint64_t rights_mask) {
    return syscall2(ABI_SYS_HANDLE_DUPLICATE, handle, rights_mask);
}
uint64_t sys_obj_info(uint64_t handle) { return syscall1(ABI_SYS_OBJ_INFO, handle); }

uint64_t sys_write(uint64_t offset, uint64_t length) { return syscall2(ABI_SYS_WRITE, offset, length); }

size_t sys_stage(size_t offset, const char* s) {
    size_t n = 0;
    while (s[n] != '\0' && offset + n < g_ipc_size) {
        g_ipc_base[offset + n] = s[n];
        n++;
    }
    return n;
}

uint64_t sys_print(const char* s) { return sys_write(0, sys_stage(0, s)); }

void sys_copy_in(void* to, size_t offset, size_t length) {
    for (size_t i = 0; i < length; i++) { ((char*)to)[i] = g_ipc_base[offset + i]; }
}

void sys_copy_out(size_t offset, const void* from, size_t length) {
    for (size_t i = 0; i < length; i++) { g_ipc_base[offset + i] = ((const char*)from)[i]; }
}

int sys_channel_drain(uint64_t channel, uint64_t msg_at, uint64_t msg_cap, uint64_t handles_at, uint64_t handle_cap,
                      void (*consume)(void* ctx, uint64_t result), void* ctx) {
    for (;;) {
        uint64_t got = sys_channel_recv(channel, msg_at, msg_cap, handles_at, handle_cap);
        // An oversized message was consumed and discarded by the kernel; keep draining what is
        // behind it. Any other error means the queue is exhausted.
        if ((int64_t)got == ABI_ERR_TRUNCATED) { continue; }
        if (sys_is_error(got)) { break; }
        consume(ctx, got);
    }
    uint64_t sig = sys_object_wait(channel, 0, 0);
    return (sig & ABI_CHANNEL_SIGNAL_PEER_CLOSED) != 0 && (sig & ABI_CHANNEL_SIGNAL_READABLE) == 0;
}

void sys_close_arrived(uint64_t result, uint64_t handles_at) {
    size_t count = (size_t)(result >> 32);
    for (size_t i = 0; i < count; i++) {
        uint64_t handle;
        sys_copy_in(&handle, handles_at + i * sizeof(handle), sizeof(handle));
        (void)sys_handle_close(handle);
    }
}

uint64_t sys_channel_create(uint64_t offset) { return syscall1(ABI_SYS_CHANNEL_CREATE, offset); }
uint64_t sys_channel_send(uint64_t handle, uint64_t offset, uint64_t length, uint64_t handles_offset,
                          uint64_t handle_count) {
    return syscall6(ABI_SYS_CHANNEL_SEND, handle, offset, length, handles_offset, handle_count, 0);
}
uint64_t sys_channel_recv(uint64_t handle, uint64_t offset, uint64_t capacity, uint64_t handles_offset,
                          uint64_t handle_capacity) {
    return syscall6(ABI_SYS_CHANNEL_RECV, handle, offset, capacity, handles_offset, handle_capacity, 0);
}

uint64_t sys_object_wait(uint64_t handle, uint64_t mask, uint64_t timeout_ns) {
    return syscall3(ABI_SYS_OBJECT_WAIT, handle, mask, timeout_ns);
}

uint64_t sys_port_create(void) { return syscall1(ABI_SYS_PORT_CREATE, 0); }
uint64_t sys_port_bind(uint64_t port, uint64_t object, uint64_t key, uint64_t mask) {
    return syscall6(ABI_SYS_PORT_BIND, port, object, key, mask, 0, 0);
}
uint64_t sys_port_unbind(uint64_t port, uint64_t key) { return syscall2(ABI_SYS_PORT_UNBIND, port, key); }
uint64_t sys_port_wait(uint64_t port, uint64_t offset, uint64_t timeout_ns) {
    return syscall3(ABI_SYS_PORT_WAIT, port, offset, timeout_ns);
}

uint64_t sys_vmo_create(uint64_t size) { return syscall1(ABI_SYS_VMO_CREATE, size); }
uint64_t sys_vmo_map(uint64_t vmo, uint64_t vaddr, uint64_t vmo_offset, uint64_t length, uint64_t prot) {
    return syscall6(ABI_SYS_VMO_MAP, vmo, vaddr, vmo_offset, length, prot, 0);
}
uint64_t sys_vmo_unmap(uint64_t vaddr) { return syscall1(ABI_SYS_VMO_UNMAP, vaddr); }

uint64_t sys_task_kill(uint64_t task) { return syscall1(ABI_SYS_TASK_KILL, task); }
uint64_t sys_task_status(uint64_t task) { return syscall1(ABI_SYS_TASK_STATUS, task); }
uint64_t sys_task_spawn(uint64_t image, uint64_t offset) { return syscall2(ABI_SYS_TASK_SPAWN, image, offset); }
