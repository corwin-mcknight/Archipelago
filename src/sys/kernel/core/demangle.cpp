// Itanium C++ ABI demangler -- split out of symbols.cpp so it can be linked and fuzzed on its own.
// This translation unit has no dependency on the embedded symbol table or any hardware state: it is a
// pure function over a mangled string and a caller-provided output buffer, which is exactly what lets
// the host fuzz lane target it (see the two-tier test system, step 6).

#include <stddef.h>

#include <ktl/maybe>
#include <ktl/span>
#include <ktl/string_view>

#include "kernel/symbols.h"

namespace kernel::symbols {

using ktl::maybe;
using ktl::nothing;
using ktl::string_view;

namespace {

// Bounded output buffer for the demangler: writes silently stop once the buffer
// is full, and `finish()` reports whether the whole name fit. This collapses the
// repeated `if (!put(...)) return false;` checks into a single trailing test.
struct sym_writer {
    ktl::span<char> out;
    size_t pos = 0;
    bool ok    = true;

    void put(char c) {
        if (pos + 1 >= out.size()) {
            ok = false;
            return;
        }
        out[pos++] = c;
    }

    void put(string_view s) {
        for (char c : s) { put(c); }
    }

    bool finish() {
        if (ok) { out[pos] = '\0'; }
        return ok;
    }
};

// Parse one Itanium `<decimal-length><identifier>` source-name, advancing `p`
// past it. Returns nothing on a missing or zero-length prefix.
maybe<string_view> read_source_name(const char*& p) {
    if (*p < '0' || *p > '9') { return nothing; }

    size_t len = 0;
    while (*p >= '0' && *p <= '9') {
        len = len * 10 + static_cast<size_t>(*p - '0');
        p++;
    }
    if (len == 0) { return nothing; }

    // Reject a length that runs past the string's NUL terminator -- a malformed/hostile prefix would
    // otherwise make the view below over-read. The input is NUL-terminated, so this scan stays in
    // bounds (it stops at the NUL).
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '\0') { return nothing; }
    }

    string_view name(p, len);
    p += len;
    return name;
}

}  // namespace

bool demangle(const char* mangled, ktl::span<char> out) {
    if (mangled == nullptr || out.data() == nullptr || out.size() < 4) { return false; }
    if (!string_view(mangled).starts_with("_Z")) { return false; }

    const char* p = mangled + 2;
    sym_writer w{out};

    bool nested = (*p == 'N');
    if (nested) { p++; }

    bool first = true;
    while (true) {
        if (nested && *p == 'E') { break; }
        if (!nested && !first) { break; }

        auto component = read_source_name(p);
        if (!component) { return false; }

        if (!first) { w.put(string_view("::")); }
        first = false;

        w.put(*component == "_GLOBAL__N_1" ? string_view("(anonymous namespace)") : *component);
    }

    w.put(string_view("()"));
    return w.finish();
}

}  // namespace kernel::symbols
