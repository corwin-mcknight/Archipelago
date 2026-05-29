#pragma once

#include <stdint.h>

namespace kernel {

/// Writes `s` as a JSON string body (without the surrounding quotes), calling `emit(char)` for
/// each output character and escaping the JSON-special characters: double-quote, backslash, the
/// common control characters (newline/carriage-return/tab), and any other control byte below
/// 0x20 as a \u00XX sequence. Use it when interpolating arbitrary text into a JSON string field.
template <typename Emit> void write_json_escaped(Emit emit, const char* s) {
    static const char hex[] = "0123456789abcdef";
    for (; s != nullptr && *s != '\0'; ++s) {
        unsigned char c = static_cast<unsigned char>(*s);
        switch (c) {
            case '"':
                emit('\\');
                emit('"');
                break;
            case '\\':
                emit('\\');
                emit('\\');
                break;
            case '\n':
                emit('\\');
                emit('n');
                break;
            case '\r':
                emit('\\');
                emit('r');
                break;
            case '\t':
                emit('\\');
                emit('t');
                break;
            default:
                if (c < 0x20) {
                    emit('\\');
                    emit('u');
                    emit('0');
                    emit('0');
                    emit(hex[(c >> 4) & 0xF]);
                    emit(hex[c & 0xF]);
                } else {
                    emit(static_cast<char>(c));
                }
                break;
        }
    }
}

}  // namespace kernel
