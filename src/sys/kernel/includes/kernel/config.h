#pragma once

// Arch identity comes from the build system (-D ARCH_X86_64 / -D ARCH_RISCV64), never from this
// header. At most one may be defined; host-tier builds define none (logic-authoritative, arch-free).
#if defined(ARCH_X86_64) && defined(ARCH_RISCV64)
#error "config: multiple architectures defined"
#endif

// 64 bytes on both supported targets (x86_64, riscv64).
#define CONFIG_CPU_CACHE_LINE_SIZE 64
#define KERNEL_MINIMUM_PAGE_SIZE 0x1000

#define CONFIG_MAX_CORES 16
#define CONFIG_KERNEL_VERSION "0.0.1"
#define CONFIG_LOG_MAX_DEVICES 8

#define KERNEL_ASSERT_HANG 1
#define KERNEL_ASSERT 1

#define CONFIG_KERNEL_LOG_COLORS 1
#define CONFIG_KERNEL_TESTING 1
#define CONFIG_KERNEL_SHELL 1
#define CONFIG_MAX_OBJECT_TYPES 64

// Kernel thread stacks: physically contiguous, used through the HHDM mapping.
#define CONFIG_KERNEL_STACK_SIZE (16 * 1024)
// Trap entries panic when sp falls below (stack base + margin); see the stack tripwire.
#define CONFIG_KERNEL_STACK_TRIPWIRE_MARGIN 4096
// Round-robin timeslice in kernel ticks (1 tick = 1 ms on both timers today).
#define CONFIG_SCHED_TIMESLICE_TICKS 10
// Scheduler trace ring capacity in records (~32 bytes each; always-on flight recorder).
#define CONFIG_SCHED_TRACE_EVENTS 512

// Testing overrides
#ifndef PRODUCT_DEBUG
#define PRODUCT_DEBUG 0
#endif

#if CONFIG_KERNEL_TESTING && !CONFIG_KERNEL_SHELL
#error "CONFIG_KERNEL_TESTING requires CONFIG_KERNEL_SHELL"
#endif

#if CONFIG_KERNEL_TESTING
#undef CONFIG_KERNEL_LOG_COLORS
#define CONFIG_KERNEL_LOG_COLORS 0
#endif

#if PRODUCT_DEBUG
#define INLINE_RELEASE_ONLY
#else
#define INLINE_RELEASE_ONLY inline
#endif