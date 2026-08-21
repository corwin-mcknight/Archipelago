#pragma once

#include <kernel/sched/ipc_buffer.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>

namespace kernel::syscalls {

ktl::ref<sched::Task> calling_task(ktl::ref<sched::Thread>& self);
uint64_t errc_of(ktl::errc error);
void buffer_write(const sched::ipc_buffer& buffer, uint64_t offset, const void* src, size_t length);
void buffer_read(const sched::ipc_buffer& buffer, uint64_t offset, void* dst, size_t length);

uint64_t sys_write(uint64_t offset, uint64_t length);
uint64_t handle_syscall(uint64_t nr, uint64_t a0, uint64_t a1);
uint64_t sys_channel_create(uint64_t offset);
uint64_t sys_channel_send(uint64_t handle, uint64_t offset, uint64_t length, uint64_t handles_offset,
                          uint64_t handle_count);
uint64_t sys_channel_recv(uint64_t handle, uint64_t offset, uint64_t capacity, uint64_t handles_offset,
                          uint64_t handle_capacity);
uint64_t sys_object_wait(uint64_t handle, uint64_t mask, uint64_t timeout_ns);
uint64_t sys_port_create();
uint64_t sys_port_bind(uint64_t port_handle, uint64_t object_handle, uint64_t key, uint64_t mask);
uint64_t sys_port_unbind(uint64_t port_handle, uint64_t key);
uint64_t sys_port_wait(uint64_t port_handle, uint64_t offset, uint64_t timeout_ns);
uint64_t sys_task_spawn(uint64_t handle, uint64_t offset);
uint64_t sys_socket_create(uint64_t offset);
uint64_t sys_socket_write(uint64_t handle, uint64_t offset, uint64_t length);
uint64_t sys_socket_read(uint64_t handle, uint64_t offset, uint64_t capacity);
uint64_t sys_vmo_create(uint64_t size);
uint64_t sys_vmo_map(uint64_t handle, uint64_t vaddr, uint64_t vmo_offset, uint64_t length, uint64_t prot);
uint64_t sys_vmo_unmap(uint64_t vaddr);

}  // namespace kernel::syscalls
