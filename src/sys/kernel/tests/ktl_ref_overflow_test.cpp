#include <kernel/testing/testing.h>

#include <ktl/ref>

KTEST_MODULE("ktl/ref");

KTEST_CASE_CRASH(ktl_ref_count_overflow) {
    // The hosted panic uses longjmp. Static refs avoid skipping destructors.
    static auto r = ktl::make_ref<int>(42);
    static ktl::ref<int> copy;
    if (!r) { return; }
    r.control_block()->ref_count.store(UINT32_MAX, ktl::memory_order::relaxed);
    copy = r;
}
