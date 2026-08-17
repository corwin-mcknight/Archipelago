#include <abi/syscall.h>
#include <stddef.h>
#include <stdint.h>
#include <sys.h>

// K&R-style first-fit heap over anonymous VMOs. Each arena is a VMO mapped at a kernel-chosen
// address whose handle is closed immediately -- the mapping keeps the memory alive -- so the heap
// holds no handles, only a free list threaded through the arenas. Arenas are never unmapped;
// free() returns blocks to the list, not pages to the kernel.

typedef struct header {
    struct header* next; /* next free block; circular, address-ordered */
    size_t units;        /* block size including this header, in header units */
} header;

static header g_base; /* zero-length block anchoring the free list */
static header* g_freep;

// Growth quantum. Small allocations share an arena; a larger request gets an arena of its own.
#define ARENA_MIN_BYTES (16u * 1024u)

static int morecore(size_t units) {
    size_t bytes = units * sizeof(header);
    if (bytes < ARENA_MIN_BYTES) { bytes = ARENA_MIN_BYTES; }
    bytes = (bytes + (ABI_VM_PAGE_SIZE - 1)) & ~(size_t)(ABI_VM_PAGE_SIZE - 1);

    uint64_t vmo = sys_vmo_create(bytes);
    if (sys_is_error(vmo)) { return 0; }
    uint64_t mapped = sys_vmo_map(vmo, 0, 0, bytes, ABI_VM_PROT_READ | ABI_VM_PROT_WRITE);
    (void)sys_handle_close(vmo);
    if (sys_is_error(mapped)) { return 0; }

    header* block = (header*)(uintptr_t)mapped;
    block->units  = bytes / sizeof(header);
    free(block + 1);
    return 1;
}

void* malloc(size_t size) {
    if (size == 0) { return NULL; }
    size_t units = (size + sizeof(header) - 1) / sizeof(header) + 1;

    if (g_freep == NULL) {
        g_base.next  = &g_base;
        g_base.units = 0;
        g_freep      = &g_base;
    }
    header* prev = g_freep;
    for (header* p = prev->next;; prev = p, p = p->next) {
        if (p->units >= units) {
            if (p->units == units) {
                prev->next = p->next;
            } else {
                p->units -= units; /* carve the tail so the list stays untouched */
                p += p->units;
                p->units = units;
            }
            g_freep = prev;
            return (void*)(p + 1);
        }
        if (p == g_freep) { /* wrapped: nothing fits */
            if (!morecore(units)) { return NULL; }
            p = g_freep;
        }
    }
}

void free(void* ptr) {
    if (ptr == NULL) { return; }
    header* block = (header*)ptr - 1;

    // Find the address-ordered insertion point; the p >= p->next slot is the list's wrap.
    header* p = g_freep;
    for (; !(block > p && block < p->next); p = p->next) {
        if (p >= p->next && (block > p || block < p->next)) { break; }
    }

    if (block + block->units == p->next) { /* coalesce with the block after */
        block->units += p->next->units;
        block->next = p->next->next;
    } else {
        block->next = p->next;
    }
    if (p + p->units == block) { /* coalesce with the block before */
        p->units += block->units;
        p->next = block->next;
    } else {
        p->next = block;
    }
    g_freep = p;
}
