#pragma once

#include <kernel/sched/ipc_buffer.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>

namespace kernel::syscalls {

inline uint64_t errc_of(ktl::errc error) { return static_cast<uint64_t>(error); }
uint64_t install_pair(obj::HandleTable& table, ktl::ref<obj::Object> first, ktl::ref<obj::Object> second,
                      obj::Rights rights, const sched::IpcRange& output);

uint64_t sys_handle_close(obj::HandleTable& table, uint64_t handle);
uint64_t sys_handle_duplicate(obj::HandleTable& table, uint64_t handle, uint64_t rights);
uint64_t sys_handle_restrict(obj::HandleTable& table, uint64_t handle, uint64_t rights, uint64_t mode);
uint64_t sys_object_info(obj::HandleTable& table, uint64_t handle);
uint64_t sys_task_kill(obj::HandleTable& table, uint64_t handle);
uint64_t sys_task_status(obj::HandleTable& table, uint64_t handle);

uint64_t sys_write(sched::Thread& self, uint64_t offset, uint64_t length);
uint64_t sys_channel_create(sched::Thread& self, uint64_t offset);
uint64_t sys_channel_send(sched::Thread& self, uint64_t handle, uint64_t offset, uint64_t length,
                          uint64_t handles_offset, uint64_t handle_count);
uint64_t sys_channel_recv(sched::Thread& self, uint64_t handle, uint64_t offset, uint64_t capacity,
                          uint64_t handles_offset, uint64_t handle_capacity);
uint64_t sys_object_wait(sched::Thread& self, uint64_t handle, uint64_t mask, uint64_t timeout_ns);
uint64_t sys_port_create(sched::Thread& self);
uint64_t sys_port_bind(sched::Thread& self, uint64_t port_handle, uint64_t object_handle, uint64_t key, uint64_t mask);
uint64_t sys_port_unbind(sched::Thread& self, uint64_t port_handle, uint64_t key);
uint64_t sys_port_wait(sched::Thread& self, uint64_t port_handle, uint64_t offset, uint64_t timeout_ns);
uint64_t sys_task_spawn(sched::Thread& self, uint64_t handle, uint64_t offset);
uint64_t sys_socket_create(sched::Thread& self, uint64_t offset);
uint64_t sys_socket_write(sched::Thread& self, uint64_t handle, uint64_t offset, uint64_t length);
uint64_t sys_socket_read(sched::Thread& self, uint64_t handle, uint64_t offset, uint64_t capacity);
uint64_t sys_vmo_create(sched::Thread& self, uint64_t size);
uint64_t sys_vmo_map(sched::Thread& self, uint64_t handle, uint64_t vaddr, uint64_t vmo_offset, uint64_t length,
                     uint64_t prot);
uint64_t sys_vmo_unmap(sched::Thread& self, uint64_t vaddr);

}  // namespace kernel::syscalls
