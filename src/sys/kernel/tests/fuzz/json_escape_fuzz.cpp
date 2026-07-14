// libFuzzer target for kernel::write_json_escaped (host fuzz lane).
//
// write_json_escaped is an output function -- it reads a NUL-terminated string and emits an escaped
// JSON string body. Two properties are checked here under mutated input:
//   1. Bound: worst-case expansion is 6 bytes per input byte (a control byte -> "\u00XX"). The output
//      buffer is sized to exactly that, so any over-emission is a precise ASan heap-overflow.
//   2. Contract: the escaped body must contain no raw control byte (< 0x20) -- every such byte must
//      have been escaped. A survivor is a real escaping bug, so trap and let libFuzzer record it.
//
// Freestanding like the code under test: malloc/free are declared here and satisfied by libc at link.

#include <kernel/json_escape.h>
#include <stddef.h>
#include <stdint.h>

extern "C" void* malloc(size_t);
extern "C" void free(void*);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    char* in = static_cast<char*>(malloc(size + 1));
    for (size_t i = 0; i < size; i++) { in[i] = static_cast<char>(data[i]); }
    in[size]   = '\0';

    size_t cap = size * 6 + 1;  // exact worst case: any over-emission lands in the ASan redzone
    char* out  = static_cast<char*>(malloc(cap));
    size_t n   = 0;
    kernel::write_json_escaped([&](char ch) { out[n++] = ch; }, in);

    for (size_t i = 0; i < n; i++) {
        if (static_cast<unsigned char>(out[i]) < 0x20) { __builtin_trap(); }  // unescaped control byte
    }

    free(out);
    free(in);
    return 0;
}
