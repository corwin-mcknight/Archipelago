# Configuration
Compile-time constants in `kernel/config.h` control kernel behavior and limits.

## Constants
| Constant | Value | Purpose |
|----------|-------|---------|
| `ARCH_X86` | 1 | Architecture flag |
| `PRODUCT_DEBUG` | 1 | Debug build |
| `CONFIG_CPU_CACHE_LINE_SIZE` | 64 | Cache line alignment for sync primitives |
| `KERNEL_MINIMUM_PAGE_SIZE` | `0x1000` | 4K pages |
| `CONFIG_MAX_CORES` | 16 | Maximum CPU cores |
| `CONFIG_KERNEL_VERSION` | `"0.0.1"` | Kernel version string |
| `CONFIG_LOG_MAX_DEVICES` | 8 | Max registered logging devices |
| `CONFIG_KERNEL_LOG_COLORS` | 1 | Color output for log messages (disabled during testing) |
| `KERNEL_ASSERT_HANG` | 1 | Hang on assertion failure |
| `KERNEL_ASSERT` | 1 | Enable assertions |
| `CONFIG_KERNEL_TESTING` | 1 | Testing mode enabled |

## Build Profiles
`PRODUCT_DEBUG` controls debug behavior.
In debug builds, `INLINE_RELEASE_ONLY` expands to nothing, preserving full stack traces.
In release builds, it expands to `inline`.

## Testing Overrides
When `CONFIG_KERNEL_TESTING` is enabled, `CONFIG_KERNEL_LOG_COLORS` is forced off so serial output stays parseable by the [[Testing|test harness]].
