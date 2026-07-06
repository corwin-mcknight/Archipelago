#pragma once

#include <kernel/config.h>

// Selects the architecture's arch_aspace definition -- the arch-shaped state
// embedded in vm_aspace. Each architecture provides the same struct name with
// its own members; the vm_aspace methods implemented by that architecture are
// the only code that touches them.
#if ARCH_X86
#include <kernel/x86/aspace.h>
#else
#error "arch_aspace: no definition for this architecture"
#endif
