#pragma once

#include <stdint.h>

#include <ktl/result>

#include "kernel/mm/page.h"
#include "kernel/mm/paging.h"

namespace kernel::mm {

// Backing-store strategy for a VMO. Pagers are held by ktl::ref from their
// VMOs. Two implementations this milestone: anonymous (zero-fill) and device
// (MMIO windows); the file-backed userspace pager protocol is IPC-gated
// future work and slots in behind this same interface.
class pager {
   public:
    virtual ~pager()                                    = default;

    // Produce the frame backing the given page offset within the VMO.
    // Writeback joins this interface when the userspace pager protocol lands;
    // nothing pages out until then.
    virtual ktl::result<vm_paddr_t> fill(uint64_t page) = 0;

    // Whether frames produced by fill() belong to the PMM (freed on
    // decommit/destroy). Device windows return false: their frames are
    // never PMM-owned and never evictable.
    virtual bool owns_frames() const                    = 0;

    // Whether the owning VMO may be resized. Device windows are fixed.
    virtual bool resizable() const                      = 0;

    // Cache mode for mappings of this pager's frames.
    virtual vm_cache_mode cache_mode() const { return vm_cache_mode::CACHED; }
};

// Zero-fill anonymous memory: fill hands out zeroed PMM frames. Stateless.
class anonymous_pager : public pager {
   public:
    ktl::result<vm_paddr_t> fill(uint64_t page) override;
    bool owns_frames() const override { return true; }
    bool resizable() const override { return true; }
};

// A fixed physical window (MMIO or wired scratch): fill translates a page
// offset to base + offset. Frames are never PMM-owned, never evictable, and
// the window never resizes.
class device_pager : public pager {
   public:
    device_pager(vm_paddr_t base, vm_cache_mode mode) : m_base(base), m_mode(mode) {}

    ktl::result<vm_paddr_t> fill(uint64_t page) override;
    bool owns_frames() const override { return false; }
    bool resizable() const override { return false; }
    vm_cache_mode cache_mode() const override { return m_mode; }

   private:
    vm_paddr_t m_base;
    vm_cache_mode m_mode;
};

}  // namespace kernel::mm
