#pragma once

#include <stddef.h>
#include <stdint.h>

// The user-program runtime surface: thin C wrappers over the raw syscalls in <abi/syscall.h>,
// plus the IPC buffer the kernel handed this thread at entry and a minimal heap over anonymous
// VMOs. Deliberately not a libc -- there is no stdio, string, or locale here, and nothing needs
// them yet.
//
// No syscall takes a pointer. Data a syscall reads comes from this thread's IPC buffer, whose
// base and size the kernel delivered in registers at thread entry -- so a syscall argument is an
// offset into memory the kernel already knows about, not an address it has to be talked into
// trusting.

#ifdef __cplusplus
extern "C" {
#endif

// The program supplies `int main(void)`, called by the runtime's _start. The return value becomes
// the task's exit status, readable by a task-handle holder once the task has terminated.

// This thread's IPC buffer, as handed over at entry and stashed by _start. There is no way to ask
// the kernel for it again.
char* sys_ipc_base(void);
size_t sys_ipc_size(void);

void sys_exit(uint64_t status);
void sys_yield(void);
void sys_sleep(uint64_t ticks);

// Handle operations return a negative error code on failure; see <abi/syscall.h> for the
// verification pipeline every one of them runs before doing anything.
int sys_is_error(uint64_t ret);
uint64_t sys_handle_close(uint64_t handle);
// Keep exactly these rights on the same handle; adding rights fails without changes.
uint64_t sys_handle_restrict(uint64_t handle, uint64_t rights);
// Atomically clear these rights; absent/unknown bits are harmless. Only invalid handles fail.
uint64_t sys_handle_remove_rights(uint64_t handle, uint64_t rights);
uint64_t sys_handle_duplicate(uint64_t handle, uint64_t rights_mask);
uint64_t sys_obj_info(uint64_t handle);

// Emit [offset, offset + length) of the IPC buffer. Returns the kernel's result: bytes written,
// or a negative error code.
uint64_t sys_write(uint64_t offset, uint64_t length);

// Stage a NUL-terminated string at `offset` in the IPC buffer, returning the byte count staged.
// Truncates rather than overrunning -- the kernel would reject an out-of-range write anyway, but
// failing here keeps the reason local.
size_t sys_stage(size_t offset, const char* s);

// Stage at offset 0 and emit in one call.
uint64_t sys_print(const char* s);

// Move typed values between program memory and the IPC buffer, byte-by-byte so alignment never
// matters. These are the one sanctioned way to read a syscall's copy-out or stage a struct.
void sys_copy_in(void* to, size_t offset, size_t length);
void sys_copy_out(size_t offset, const void* from, size_t length);

// Channel syscalls; like everything else, data rides in the IPC buffer and arguments are offsets
// into it. create writes the two endpoint handles at `offset`. Messages can carry handles: send
// reads `handle_count` uint64 handle values at `handles_offset`, recv lands arrived handles there
// and reports their count in the high 32 bits of its return, the byte count in the low 32.
uint64_t sys_channel_create(uint64_t offset);
uint64_t sys_channel_send(uint64_t handle, uint64_t offset, uint64_t length, uint64_t handles_offset,
                          uint64_t handle_count);
uint64_t sys_channel_recv(uint64_t handle, uint64_t offset, uint64_t capacity, uint64_t handles_offset,
                          uint64_t handle_capacity);

// Drain a channel to exhaustion: every queued message lands at msg_at (bytes, up to msg_cap) and
// handles_at (arrived handles, up to handle_cap) and goes through consume(ctx, recv's return
// value); a message wider than either window is discarded by the kernel and skipped here, so a
// hostile or buggy sender cannot wedge the endpoint. Afterwards reports whether the endpoint is
// gone for good: 1 when the peer has hung up with nothing left to read, 0 otherwise -- the
// distinction an edge-observed port binding cannot make on its own.
int sys_channel_drain(uint64_t channel, uint64_t msg_at, uint64_t msg_cap, uint64_t handles_at, uint64_t handle_cap,
                      void (*consume)(void* ctx, uint64_t result), void* ctx);

// Close every handle a recv landed at handles_at, the count taken from recv's return value. For
// consume paths that do not take ownership of what arrived -- an unclosed stray handle squats in
// the receiver's table forever.
void sys_close_arrived(uint64_t result, uint64_t handles_at);

// Nonzero mask blocks until any of those signal bits assert and returns the signals observed;
// zero mask polls, returning the current signals immediately. timeout_ns of 0 waits forever;
// nonzero bounds the wait and a lapse returns the timed_out error.
uint64_t sys_object_wait(uint64_t handle, uint64_t mask, uint64_t timeout_ns);

// Ports aggregate signal transitions from bound objects into packets; wait lands a 16-byte
// packet (key, then signals) at `offset` in the IPC buffer. See <abi/syscall.h>.
uint64_t sys_port_create(void);
uint64_t sys_port_bind(uint64_t port, uint64_t object, uint64_t key, uint64_t mask);
uint64_t sys_port_unbind(uint64_t port, uint64_t key);

// Task lifecycle: kill and status act on a task handle; spawn turns an image VMO handle into a
// running child, writing the new task handle then the bootstrap channel handle (two uint64s) at
// `offset` in the IPC buffer. See <abi/syscall.h> for semantics and rights.
uint64_t sys_task_kill(uint64_t task);
uint64_t sys_task_status(uint64_t task);
uint64_t sys_task_spawn(uint64_t image, uint64_t offset);
uint64_t sys_port_wait(uint64_t port, uint64_t offset, uint64_t timeout_ns);

// Sockets: a byte-stream pair with no message boundaries. create writes the two endpoint handles
// at `offset`; write appends up to `length` bytes from the IPC buffer and returns how many the
// peer's buffer took (short = backpressure, ABI_ERR_WOULD_BLOCK-style only when zero fit --
// capacity_exhausted); read takes up to `capacity` buffered bytes into the IPC buffer and
// returns the count, ABI_ERR_WOULD_BLOCK when empty, ABI_ERR_PEER_CLOSED when empty for good.
uint64_t sys_socket_create(uint64_t offset);
uint64_t sys_socket_write(uint64_t handle, uint64_t offset, uint64_t length);
uint64_t sys_socket_read(uint64_t handle, uint64_t offset, uint64_t capacity);

// User-controlled memory: create an anonymous VMO, map it (vaddr 0 = kernel picks; map returns
// the mapped base), unmap the whole mapping containing an address. Sizes, offsets, and addresses
// are in bytes and must be multiples of ABI_VM_PAGE_SIZE -- rejected, never rounded. A mapping
// keeps its VMO alive, so the handle may be closed as soon as the map returns.
uint64_t sys_vmo_create(uint64_t size);
uint64_t sys_vmo_map(uint64_t vmo, uint64_t vaddr, uint64_t vmo_offset, uint64_t length, uint64_t prot);
uint64_t sys_vmo_unmap(uint64_t vaddr);

// The heap, first-fit over VMO-backed arenas the runtime maps as needed. Arenas are never
// returned to the kernel; free() recycles blocks within them.
void* malloc(size_t size);
void free(void* ptr);

#ifdef __cplusplus
}
#endif
