// libFuzzer target for the Itanium demangler (host fuzz lane, step 6).
//
// The demangler is a hand-written length-prefixed parser; this drives it with mutated bytes under
// ASan/UBSan. Input and output live in exactly-sized heap allocations so that any over-read past the
// mangled string (e.g. a `<len>` prefix the parser trusts beyond the actual input) or over-write past
// the output buffer is a precise, demangler-attributed ASan report rather than silent slack.
//
// Freestanding like the code under test: malloc/free are declared here and satisfied by libc at link
// (ASan intercepts them for the redzones); no libc headers are included.

#include <kernel/symbols.h>
#include <stddef.h>
#include <stdint.h>

#include <ktl/span>

extern "C" void* malloc(size_t);
extern "C" void free(void*);

// KTL bounds checks (span/string_view) reach for the global panic(). During fuzzing a panic IS a
// finding, so trap and let libFuzzer record the input.
void panic(const char*) { __builtin_trap(); }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    char* in = static_cast<char*>(malloc(size + 1));
    for (size_t i = 0; i < size; i++) { in[i] = static_cast<char>(data[i]); }
    in[size]              = '\0';

    constexpr size_t kOut = 64;
    char* out             = static_cast<char*>(malloc(kOut));
    kernel::symbols::demangle(in, ktl::span<char>(out, kOut));

    free(out);
    free(in);
    return 0;
}
