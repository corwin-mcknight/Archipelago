// libFuzzer target for atoi (host fuzz lane, step 6).
//
// atoi is a hand-written decimal parser with explicit INT_MAX/INT_MIN clamping. It reads a
// NUL-terminated string; ASan (no over-read past the terminator) and UBSan (no signed overflow in
// the accumulate/negate arithmetic) are the oracle for arbitrary mutated bytes. `-fno-sanitize-recover`
// turns any violation into an abort libFuzzer records.
//
// Freestanding like the code under test: malloc/free are declared here and satisfied by libc at link.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

extern "C" void* malloc(size_t);
extern "C" void free(void*);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    char* in = static_cast<char*>(malloc(size + 1));
    for (size_t i = 0; i < size; i++) { in[i] = static_cast<char>(data[i]); }
    in[size] = '\0';

    (void)atoi(in);

    free(in);
    return 0;
}
