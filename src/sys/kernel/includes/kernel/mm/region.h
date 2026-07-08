#pragma once

#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>
#include <kernel/obj/types.h>
#include <stddef.h>
#include <stdint.h>

#include <ktl/rb_tree>
#include <ktl/ref>
#include <ktl/result>

#include "kernel/mm/paging.h"

namespace kernel::mm {

class vmo;
class vm_aspace;
class Region;

// One slot in a region's child tree: either a sub-region or a VMO binding.
// Slots are heap-allocated, owned by the parent region, and keyed by base;
// siblings never overlap.
struct region_child {
    ktl::rb_node hook;
    uintptr_t base = 0;
    size_t size    = 0;

    ktl::ref<Region> child;  // engaged for sub-regions
    ktl::ref<vmo> vmo_ref;   // engaged for VMO bindings
    uint64_t vmo_offset = 0;
    vm_prot_t prot      = vm_prot::NONE;  // binding protection
    vm_cache_mode cache = vm_cache_mode::CACHED;

    bool is_binding() const { return child.get() == nullptr; }
};

struct region_child_less {
    bool operator()(const region_child& a, const region_child& b) const { return a.base < b.base; }
    bool operator()(const region_child& a, uintptr_t key) const { return a.base < key; }
    bool operator()(uintptr_t key, const region_child& a) const { return key < a.base; }
};

// A VMAR-shaped node in an address space's region tree. Owns [base, base+size)
// and a max protection its descendants can never exceed. Regions are kernel
// Objects held by ktl::ref; handle exposure and the detached-state machine
// land with the task/IPC milestone.
// A child dropped from its parent while external refs remain has no
// detached state -- callers must not use a region after unmapping it.
class Region : public obj::Object {
   public:
    DECLARE_OBJECT_TYPE(Region, obj::type_ids::REGION)

    Region(vm_aspace& aspace, uintptr_t base, size_t size, vm_prot_t max_prot);
    ~Region() override;

    uintptr_t base() const { return m_base; }
    size_t size() const { return m_size; }
    vm_prot_t max_prot() const { return m_max_prot; }

    // Carve a sub-region. Enforces: within parent bounds, no sibling overlap,
    // child max-prot a subset of the parent's.
    ktl::result<ktl::ref<Region>> create_child(uintptr_t base, size_t size, vm_prot_t max_prot);

    // Bind a VMO range at an explicit vaddr (first-fit search is a later
    // nicety). Pages populate on fault or commit, never here.
    ktl::result<void> map(uintptr_t vaddr, size_t size, ktl::ref<vmo> vmo_ref, uint64_t vmo_offset, vm_prot_t prot,
                          vm_cache_mode cache = vm_cache_mode::CACHED);

    // Remove every child slot fully contained in [base, base+size), zapping
    // any installed translations. A range that partially overlaps a slot is an
    // error. No binding splitting -- whole-slot unmap only until a
    // consumer needs partial unmaps.
    ktl::result<void> unmap(uintptr_t base, size_t size);

    // Narrow the protection of bindings fully contained in the range.
    // Escalation past the current binding prot is rejected; installed
    // translations are zapped and refill through the fault path with the
    // narrowed protection.
    ktl::result<void> protect(uintptr_t base, size_t size, vm_prot_t prot);

    // Deepest binding slot containing vaddr, descending through sub-regions.
    // Pointer is valid only under the VMM lock.
    region_child* find_binding(uintptr_t vaddr);

    // In-key-order visit of the direct children (shell observability).
    template <typename F> void for_each_child(F&& fn) const {
        for (auto it = m_children.begin(); it != m_children.end(); ++it) { fn(*it); }
    }

    static ktl::result<void> register_type(obj::TypeRegistry& registry) {
        return registry.register_type(TYPE_ID, "region", obj::RIGHT_READ | obj::RIGHT_WRITE, obj::RIGHT_READ);
    }

   private:
    // Validates bounds/overlap/prot and links an empty slot into the tree.
    ktl::result<region_child*> insert_slot(uintptr_t base, size_t size, vm_prot_t prot);
    // Unlinks a slot, tears down its contents (recursively for sub-regions,
    // zapping binding translations), and frees it. Teardown happens at unlink
    // time, not at last-ref drop, so a lingering ref sees an inert region.
    void remove_slot(region_child& slot);
    void clear_children();
    void zap_range(uintptr_t base, size_t size);

    vm_aspace& m_aspace;
    uintptr_t m_base;
    size_t m_size;
    vm_prot_t m_max_prot;
    ktl::rb_tree<region_child, &region_child::hook, region_child_less> m_children;
};

}  // namespace kernel::mm
