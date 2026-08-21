#include <stdlib.h>

void itoa(unsigned long long n, char* buffer, unsigned int base) {
    if (base < 2 || base > 16) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    if (n == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    unsigned long long tmp = n;
    int len                = 0;
    while (tmp) {
        tmp /= base;
        len++;
    }

    tmp = n;
    for (int i = 0; i < len; i++) {
        buffer[len - i - 1] = "0123456789ABCDEF"[tmp % base];
        tmp /= base;
    }
    buffer[len] = '\0';
}
