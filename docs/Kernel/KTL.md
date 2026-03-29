# KTL

The Kernel Template Library is a freestanding C++ container library that replaces the standard library.
Headers live in `src/sys/kernel/includes/ktl/`.
There are no exceptions, no RTTI, and no libc dependency.
See the headers for full API details.

## Containers
### `ktl::vector<T>`
Dynamic array with doubling growth strategy.
Move-only (no copy).
Supports `push_back`, `pop_back`, `operator[]`, iterators, `size`, `capacity`, `reserve`, `clear`.

### `ktl::deque<T>`
Double-ended queue. `push_front`, `push_back`, `pop_front`, `pop_back`.

### `ktl::stack<T>`
LIFO container. Used by the [[Memory Subsystem#Physical Memory Manager|PMM]] to track free pages.

### `ktl::string`
Mutable, heap-allocated string. Concatenation, comparison, substring operations.

### `ktl::string_view`
Non-owning, `constexpr`-compatible string reference. Does not allocate.

### `ktl::fixed_string<N>`
Fixed-capacity string that stores up to `N` characters inline.
Used by the [[Device Drivers|logging system]] for log messages (240 bytes).

### `ktl::circular_buffer<T, N>`
Fixed-size circular buffer. Used by `system_log` to hold the last 64 log messages.

### `ktl::static_array<T, N>`
Fixed-size array. Equivalent to `std::array`.

### `ktl::static_vector<T, N>`
Fixed-capacity vector with dynamic count. No heap allocation.

## Utilities
### `ktl::maybe<T>`
Optional value type.

`has_value()`, `value()`, `value_or(default)`, `map(fn)`, `and_then(fn)`, `or_else(fn)`, `filter(pred)`.

### `ktl::result<T, E>`
Error handling type.

`is_ok()`, `is_err()`, `unwrap()`, `map(fn)`, `and_then(fn)`.

### `ktl::fmt`
Printf-style formatting. Custom types specialize `kfmt_printer<T>`. Used throughout the kernel for log output.

### Type Support
`ktl::type_traits`, `ktl::algorithm`, `ktl::utility` provide metaprogramming and algorithm foundations
(`move`, `forward`, `swap`, `min`, `max`, etc.).
