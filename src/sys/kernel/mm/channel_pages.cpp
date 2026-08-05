#include <kernel/config.h>
#include <kernel/mm/pmm.h>
#include <kernel/obj/channel.h>

extern uintptr_t g_hhdm_offset;

namespace kernel::obj {

// A message must fit its one page, or MessageBuffer::create would hand out storage smaller than
// the length it promises.
static_assert(Channel::MAX_MESSAGE_BYTES <= KERNEL_MINIMUM_PAGE_SIZE);

uintptr_t channel_page_alloc() {
    auto frame = kernel::mm::g_page_frame_allocator.alloc();
    if (!frame.has_value()) { return 0; }
    return frame.value() + g_hhdm_offset;
}

void channel_page_free(uintptr_t page) { kernel::mm::g_page_frame_allocator.free(page - g_hhdm_offset); }

}  // namespace kernel::obj
