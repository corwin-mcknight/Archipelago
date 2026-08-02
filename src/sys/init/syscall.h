#pragma once

#include <abi/syscall.h>
#include <stddef.h>
#include <stdint.h>

// Thin syscall wrappers for user programs. The numbers come from the installed public ABI header;
// only the instruction sequence and register assignment are per-architecture.
//
// No syscall takes a pointer. Data a syscall reads comes from this thread's IPC buffer, whose base
// and size the kernel delivered in registers at thread entry -- so a syscall argument is an offset
// into memory the kernel already knows about, not an address it has to be talked into trusting.

namespace init {

// One assembly block for every syscall. Wrappers below pass explicit zeros for arguments they do
// not use, rather than leaving those registers holding whatever the last call put there -- the
// kernel ignores unread arguments today, but a syscall that starts reading one should not see
// leftovers from an unrelated call.
inline uint64_t syscall6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t ret;
#if defined(__x86_64__)
    // SYSCALL itself destroys rcx and r11 (it parks the return address and flags there), so those are
    // gone regardless of what the kernel does. Every other caller-saved register survives: the entry
    // stub pushes and restores them so user mode never sees leftover kernel values. r10 carries the
    // fourth argument because rcx is unavailable.
    register uint64_t rdi asm("rdi") = a0;
    register uint64_t rsi asm("rsi") = a1;
    register uint64_t rdx asm("rdx") = a2;
    register uint64_t r10 asm("r10") = a3;
    register uint64_t r8 asm("r8")   = a4;
    register uint64_t r9 asm("r9")   = a5;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(nr), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
#elif defined(__riscv)
    register uint64_t r_a7 asm("a7") = nr;
    register uint64_t r_a0 asm("a0") = a0;
    register uint64_t r_a1 asm("a1") = a1;
    register uint64_t r_a2 asm("a2") = a2;
    register uint64_t r_a3 asm("a3") = a3;
    register uint64_t r_a4 asm("a4") = a4;
    register uint64_t r_a5 asm("a5") = a5;
    asm volatile("ecall"
                 : "+r"(r_a0)
                 : "r"(r_a7), "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a4), "r"(r_a5)
                 : "memory");
    ret = r_a0;
#endif
    return ret;
}

inline uint64_t syscall2(uint64_t nr, uint64_t a0, uint64_t a1) { return syscall6(nr, a0, a1, 0, 0, 0, 0); }
inline uint64_t syscall1(uint64_t nr, uint64_t a0) { return syscall6(nr, a0, 0, 0, 0, 0, 0); }

inline void exit() { syscall1(abi::syscall::SYS_EXIT, 0); }
inline void yield() { syscall1(abi::syscall::SYS_YIELD, 0); }
inline void sleep(uint64_t ticks) { syscall1(abi::syscall::SYS_SLEEP, ticks); }

// Handle operations return a negative error code on failure; see <abi/syscall.h> for the
// verification pipeline every one of them runs before doing anything.
inline bool is_error(uint64_t ret) { return static_cast<int64_t>(ret) < 0; }
inline uint64_t handle_close(uint64_t handle) { return syscall1(abi::syscall::SYS_HANDLE_CLOSE, handle); }
inline uint64_t handle_duplicate(uint64_t handle, uint64_t rights_mask) {
    return syscall2(abi::syscall::SYS_HANDLE_DUPLICATE, handle, rights_mask);
}
inline uint64_t obj_info(uint64_t handle) { return syscall1(abi::syscall::SYS_OBJ_INFO, handle); }

// This thread's IPC buffer, as handed over at entry. Every program stashes it before doing anything
// else; there is no way to ask for it again.
struct ipc_buffer {
    char* base;
    size_t size;

    // Stage a NUL-terminated string at `offset`, returning the byte count staged. Truncates rather
    // than overrunning -- the kernel would reject an out-of-range write anyway, but failing here
    // keeps the reason local.
    size_t stage(size_t offset, const char* s) const {
        size_t n = 0;
        while (s[n] != '\0' && offset + n < size) {
            base[offset + n] = s[n];
            n++;
        }
        return n;
    }

    // Emit [offset, offset + length). Returns the kernel's result: bytes written, or a negative
    // error code.
    uint64_t write(size_t offset, size_t length) const {
        return syscall2(abi::syscall::SYS_WRITE, offset, length);
    }

    uint64_t print(const char* s) const {
        size_t n = stage(0, s);
        return write(0, n);
    }
};

}  // namespace init
