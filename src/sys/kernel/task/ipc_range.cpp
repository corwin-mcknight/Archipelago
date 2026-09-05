#include <kernel/panic.h>
#include <kernel/sched/ipc_buffer.h>

namespace kernel::sched {

ktl::result<IpcRange> ipc_buffer::range(uint64_t offset, uint64_t length) const& {
    const uint64_t size = size_bytes();
    if (!valid() || offset > size || length > size - offset) { return ktl::err(ktl::errc::out_of_range); }
    return ktl::result<IpcRange>::ok(IpcRange(*this, offset, static_cast<size_t>(length)));
}

ktl::span<uint8_t> IpcRange::next() {
    if (m_length == 0) { return {}; }
    constexpr size_t PAGE_SIZE = KERNEL_MINIMUM_PAGE_SIZE;
    size_t page                = static_cast<size_t>(m_offset / PAGE_SIZE);
    size_t in_page             = static_cast<size_t>(m_offset % PAGE_SIZE);
    size_t take                = ktl::min(m_length, PAGE_SIZE - in_page);
    auto* data                 = reinterpret_cast<uint8_t*>(m_buffer->m_frames[page] + in_page);
    m_offset += take;
    m_length -= take;
    return {data, take};
}

void IpcRange::read(void* dst, size_t length) const {
    if (length > m_length) { panic("IPC read exceeds checked range"); }
    auto cursor     = *this;
    cursor.m_length = length;
    auto* out       = static_cast<uint8_t*>(dst);
    for (auto chunk = cursor.next(); !chunk.empty(); chunk = cursor.next()) {
        __builtin_memcpy(out, chunk.data(), chunk.size());
        out += chunk.size();
    }
}

void IpcRange::write(const void* src, size_t length) const {
    if (length > m_length) { panic("IPC write exceeds checked range"); }
    auto cursor     = *this;
    cursor.m_length = length;
    auto* in        = static_cast<const uint8_t*>(src);
    for (auto chunk = cursor.next(); !chunk.empty(); chunk = cursor.next()) {
        __builtin_memcpy(chunk.data(), in, chunk.size());
        in += chunk.size();
    }
}

}  // namespace kernel::sched
