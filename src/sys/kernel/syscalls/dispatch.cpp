#include <kernel/sched/scheduler.h>
#include <kernel/synchronization/execution_context.h>
#include <kernel/syscall.h>

#include "internal.h"

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
                    auto task = kernel::syscalls::calling_task(self);
                    if (task.get() != kernel::sched::kernel_task().get()) {
                        task->record_exit(kernel::syscall::TASK_EXIT_EXITED, static_cast<uint32_t>(a0));
                    }
                }
            }
            kernel::synchronization::syscall_exit();
            kernel::sched::exit_current();
        }
        case kernel::syscall::SYS_YIELD: kernel::sched::yield(); break;
        case kernel::syscall::SYS_SLEEP: kernel::sched::sleep_ticks(a0); break;
        case kernel::syscall::SYS_WRITE: ret = kernel::syscalls::sys_write(a0, a1); break;
        case kernel::syscall::SYS_HANDLE_CLOSE:
        case kernel::syscall::SYS_HANDLE_DUPLICATE:
        case kernel::syscall::SYS_OBJ_INFO:
        case kernel::syscall::SYS_TASK_KILL:
        case kernel::syscall::SYS_TASK_STATUS: ret = kernel::syscalls::handle_syscall(nr, a0, a1); break;
        case kernel::syscall::SYS_CHANNEL_CREATE: ret = kernel::syscalls::sys_channel_create(a0); break;
        case kernel::syscall::SYS_CHANNEL_SEND: ret = kernel::syscalls::sys_channel_send(a0, a1, a2, a3, a4); break;
        case kernel::syscall::SYS_CHANNEL_RECV: ret = kernel::syscalls::sys_channel_recv(a0, a1, a2, a3, a4); break;
        case kernel::syscall::SYS_OBJECT_WAIT: ret = kernel::syscalls::sys_object_wait(a0, a1, a2); break;
        case kernel::syscall::SYS_PORT_CREATE: ret = kernel::syscalls::sys_port_create(); break;
        case kernel::syscall::SYS_PORT_BIND: ret = kernel::syscalls::sys_port_bind(a0, a1, a2, a3); break;
        case kernel::syscall::SYS_PORT_UNBIND: ret = kernel::syscalls::sys_port_unbind(a0, a1); break;
        case kernel::syscall::SYS_PORT_WAIT: ret = kernel::syscalls::sys_port_wait(a0, a1, a2); break;
        case kernel::syscall::SYS_TASK_SPAWN: ret = kernel::syscalls::sys_task_spawn(a0, a1); break;
        case kernel::syscall::SYS_VMO_CREATE: ret = kernel::syscalls::sys_vmo_create(a0); break;
        case kernel::syscall::SYS_VMO_MAP: ret = kernel::syscalls::sys_vmo_map(a0, a1, a2, a3, a4); break;
        case kernel::syscall::SYS_VMO_UNMAP: ret = kernel::syscalls::sys_vmo_unmap(a0); break;
        case kernel::syscall::SYS_SOCKET_CREATE: ret = kernel::syscalls::sys_socket_create(a0); break;
        case kernel::syscall::SYS_SOCKET_WRITE: ret = kernel::syscalls::sys_socket_write(a0, a1, a2); break;
        case kernel::syscall::SYS_SOCKET_READ: ret = kernel::syscalls::sys_socket_read(a0, a1, a2); break;
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
