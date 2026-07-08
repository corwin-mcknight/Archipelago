#pragma once

#ifdef ARCH_X86
#include "kernel/x86/registers.h"  // IWYU pragma: export
#elif defined(ARCH_RISCV64)
#include "kernel/riscv/registers.h"  // IWYU pragma: export
#endif
