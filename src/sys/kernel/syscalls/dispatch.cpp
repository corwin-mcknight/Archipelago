#include <kernel/sched/scheduler.h>
#include <kernel/synchronization/execution_context.h>
#include <kernel/syscall.h>

#include "internal.h"

namespace kernel::syscalls {
namespace {

uint64_t dispatch(sched::Thread& self, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    switch (nr) {
        case kernel::syscall::SYS_EXIT:
            if (self.owner().get() != kernel::sched::kernel_task().get()) {
                self.owner()->record_exit(kernel::syscall::TASK_EXIT_EXITED, static_cast<uint32_t>(a0));
            }
            return 0;
        case kernel::syscall::SYS_YIELD: kernel::sched::yield(); break;
        case kernel::syscall::SYS_SLEEP: kernel::sched::sleep_ticks(a0); break;
        case kernel::syscall::SYS_WRITE: return sys_write(self, a0, a1);
        case kernel::syscall::SYS_HANDLE_CLOSE: return sys_handle_close(self.owner()->handles(), a0);
        case kernel::syscall::SYS_HANDLE_RESTRICT: return sys_handle_restrict(self.owner()->handles(), a0, a1, a2);
        case kernel::syscall::SYS_HANDLE_DUPLICATE: return sys_handle_duplicate(self.owner()->handles(), a0, a1);
        case kernel::syscall::SYS_OBJ_INFO: return sys_object_info(self.owner()->handles(), a0);
        case kernel::syscall::SYS_TASK_KILL: return sys_task_kill(self.owner()->handles(), a0);
        case kernel::syscall::SYS_TASK_STATUS: return sys_task_status(self.owner()->handles(), a0);
        case kernel::syscall::SYS_CHANNEL_CREATE: return sys_channel_create(self, a0);
        case kernel::syscall::SYS_CHANNEL_SEND: return sys_channel_send(self, a0, a1, a2, a3, a4);
        case kernel::syscall::SYS_CHANNEL_RECV: return sys_channel_recv(self, a0, a1, a2, a3, a4);
        case kernel::syscall::SYS_OBJECT_WAIT: return sys_object_wait(self, a0, a1, a2);
        case kernel::syscall::SYS_PORT_CREATE: return sys_port_create(self);
        case kernel::syscall::SYS_PORT_BIND: return sys_port_bind(self, a0, a1, a2, a3);
        case kernel::syscall::SYS_PORT_UNBIND: return sys_port_unbind(self, a0, a1);
        case kernel::syscall::SYS_PORT_WAIT: return sys_port_wait(self, a0, a1, a2);
        case kernel::syscall::SYS_TASK_SPAWN: return sys_task_spawn(self, a0, a1);
        case kernel::syscall::SYS_VMO_CREATE: return sys_vmo_create(self, a0);
        case kernel::syscall::SYS_VMO_MAP: return sys_vmo_map(self, a0, a1, a2, a3, a4);
        case kernel::syscall::SYS_VMO_UNMAP: return sys_vmo_unmap(self, a0);
        case kernel::syscall::SYS_SOCKET_CREATE: return sys_socket_create(self, a0);
        case kernel::syscall::SYS_SOCKET_WRITE: return sys_socket_write(self, a0, a1, a2);
        case kernel::syscall::SYS_SOCKET_READ: return sys_socket_read(self, a0, a1, a2);
        default: return static_cast<uint64_t>(-1);
    }
    return 0;
}

}  // namespace
}  // namespace kernel::syscalls

extern "C" uint64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                     uint64_t a5) {
    (void)a5;
    kernel::synchronization::syscall_enter();
    uint64_t ret;
    bool exit_now = false;
    {
        auto self = kernel::sched::current();
        ret       = self ? kernel::syscalls::dispatch(*self, nr, a0, a1, a2, a3, a4)
                         : kernel::syscalls::errc_of(ktl::errc::invalid_operation);
        exit_now  = self && (nr == kernel::syscall::SYS_EXIT || self->killed());
    }
    // Every return, including unknown syscalls, crosses this kill boundary with references and
    // locks released. exit_current() abandons the stack without unwinding it.
    kernel::synchronization::syscall_exit();
    if (exit_now) { kernel::sched::exit_current(); }
    return ret;
}
