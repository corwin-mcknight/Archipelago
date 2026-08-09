#pragma once

#include <stddef.h>
#include <stdint.h>

// The user-program runtime surface: thin C wrappers over the raw syscalls in <abi/syscall.h>,
// plus the IPC buffer the kernel handed this thread at entry. Deliberately not a libc -- there is
// no malloc, stdio, string, or locale here, and nothing needs them yet.
//
// No syscall takes a pointer. Data a syscall reads comes from this thread's IPC buffer, whose
// base and size the kernel delivered in registers at thread entry -- so a syscall argument is an
// offset into memory the kernel already knows about, not an address it has to be talked into
// trusting.

#ifdef __cplusplus
extern "C" {
#endif

// The program supplies `int main(void)`, called by the runtime's _start. The return value is
// discarded until SYS_EXIT carries a status argument.

// This thread's IPC buffer, as handed over at entry and stashed by _start. There is no way to ask
// the kernel for it again.
char* sys_ipc_base(void);
size_t sys_ipc_size(void);

void sys_exit(void);
void sys_yield(void);
void sys_sleep(uint64_t ticks);

// Handle operations return a negative error code on failure; see <abi/syscall.h> for the
// verification pipeline every one of them runs before doing anything.
int sys_is_error(uint64_t ret);
uint64_t sys_handle_close(uint64_t handle);
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

// Channel syscalls; like everything else, data rides in the IPC buffer and arguments are offsets
// into it. create writes the two endpoint handles at `offset`. Messages can carry handles: send
// reads `handle_count` uint64 handle values at `handles_offset`, recv lands arrived handles there
// and reports their count in the high 32 bits of its return, the byte count in the low 32.
uint64_t sys_channel_create(uint64_t offset);
uint64_t sys_channel_send(uint64_t handle, uint64_t offset, uint64_t length, uint64_t handles_offset,
                          uint64_t handle_count);
uint64_t sys_channel_recv(uint64_t handle, uint64_t offset, uint64_t capacity, uint64_t handles_offset,
                          uint64_t handle_capacity);

// Nonzero mask blocks until any of those signal bits assert and returns the signals observed;
// zero mask polls, returning the current signals immediately. timeout_ns of 0 waits forever;
// nonzero bounds the wait and a lapse returns the timed_out error.
uint64_t sys_object_wait(uint64_t handle, uint64_t mask, uint64_t timeout_ns);

// Ports aggregate signal transitions from bound objects into packets; wait lands a 16-byte
// packet (key, then signals) at `offset` in the IPC buffer. See <abi/syscall.h>.
uint64_t sys_port_create(void);
uint64_t sys_port_bind(uint64_t port, uint64_t object, uint64_t key, uint64_t mask);
uint64_t sys_port_unbind(uint64_t port, uint64_t key);
uint64_t sys_port_wait(uint64_t port, uint64_t offset, uint64_t timeout_ns);

#ifdef __cplusplus
}
#endif
