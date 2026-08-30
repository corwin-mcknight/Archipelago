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
#define CONFIG_LOCKDEP_MAX_HELD 16
#define CONFIG_LOCKDEP_MAX_LOCKS 128
#define CONFIG_LOCKDEP_MAX_EDGES 512

#ifdef __cplusplus
static_assert((KERNEL_MINIMUM_PAGE_SIZE & (KERNEL_MINIMUM_PAGE_SIZE - 1)) == 0,
              "minimum page size must be a power of two");
static_assert((CONFIG_CPU_CACHE_LINE_SIZE & (CONFIG_CPU_CACHE_LINE_SIZE - 1)) == 0,
              "cache-line size must be a power of two");
static_assert(CONFIG_KERNEL_STACK_SIZE % KERNEL_MINIMUM_PAGE_SIZE == 0, "kernel stacks must span whole pages");
static_assert(CONFIG_KERNEL_STACK_TRIPWIRE_MARGIN < CONFIG_KERNEL_STACK_SIZE,
              "the stack tripwire must leave usable stack space");
static_assert(CONFIG_MAX_CORES > 0 && CONFIG_MAX_CORES <= 64, "core indices must fit the 64-bit core masks");
static_assert(CONFIG_SCHED_TRACE_EVENTS > 0, "the scheduler trace must have storage");
static_assert(CONFIG_MAX_OBJECT_TYPES > 0, "the object registry must have storage");
#endif

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
