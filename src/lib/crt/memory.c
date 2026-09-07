#include <stddef.h>

// Freestanding compiler-generated aggregate copies and initialization use these symbols too.
void* memcpy(void* destination, const void* source, size_t size) {
    unsigned char* out      = destination;
    const unsigned char* in = source;
    for (size_t i = 0; i < size; ++i) { out[i] = in[i]; }
    return destination;
}

void* memset(void* destination, int value, size_t size) {
    unsigned char* out = destination;
    for (size_t i = 0; i < size; ++i) { out[i] = (unsigned char)value; }
    return destination;
}
