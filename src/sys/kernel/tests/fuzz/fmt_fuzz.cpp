// libFuzzer target for the ktl::format brace parser (host fuzz lane, step 6).
//
// format_to_buffer_raw walks the format string with unchecked string_view::operator[] and, in the
// ":<spec>" branch, does several bare ++format_index reads (alignment/pad/width/specifier). A format
// string that ends mid-spec can walk the index past fmt.size(). Driving it with mutated bytes under
// ASan turns any such over-read into a precise, formatter-attributed report.
//
// The format string lives in an exactly-sized heap allocation (no NUL slack) so a read past
// fmt.size() lands in an ASan redzone rather than incidental valid memory. A fixed, varied argument
// pack exercises the integer/unsigned/string printer paths; the fuzzer explores specifiers itself.
//
// Freestanding like the code under test: malloc/free are declared here and satisfied by libc at link;
// no libc headers are included.

#include <stddef.h>
#include <stdint.h>

#include <ktl/fmt>
#include <ktl/string_view>

extern "C" void* malloc(size_t);
extern "C" void free(void*);

// KTL bounds checks reach for the global panic(). During fuzzing a panic IS a finding, so trap and
// let libFuzzer record the input.
void panic(const char*) { __builtin_trap(); }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    char* fmt = static_cast<char*>(malloc(size));  // exact size: over-read past size hits the redzone
    for (size_t i = 0; i < size; i++) { fmt[i] = static_cast<char>(data[i]); }

    constexpr size_t kOut = 128;
    char* out             = static_cast<char*>(malloc(kOut));
    ktl::format::format_to_buffer_raw(out, kOut, ktl::string_view(fmt, size), (int)-42, (const char*)"str",
                                      (unsigned)42u, (char)'z');

    free(out);
    free(fmt);
    return 0;
}
