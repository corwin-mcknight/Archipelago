#include <kernel/registers.h>

#include "kernel/mm/vm_aspace.h"

// x86_64 #PF glue: pull the faulting address from CR2, decode the error code,
// and hand an arch-neutral fault description to the VMM resolver. Returns
// true when resolved (the iret retries the access); false falls through to
// the crash path with all diagnostics intact.
extern "C" bool x86_try_resolve_page_fault(register_frame_t* regs) {
    uintptr_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));

    kernel::mm::vm_fault fault{
        .vaddr   = cr2,
        .write   = (regs->err_code & (1u << 1)) != 0,
        .present = (regs->err_code & (1u << 0)) != 0,
        .user    = (regs->err_code & (1u << 2)) != 0,
    };
    return kernel::mm::vmm_handle_fault(fault);
}
