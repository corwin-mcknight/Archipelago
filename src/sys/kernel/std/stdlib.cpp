#include <stdlib.h>

int atoi(const char* str) {
    int result = 0;
    int sign = (*str == '-') ? -1 : 1;
    if (sign == -1) ++str;
    while (*str) { result = result * 10 + (*str++ - '0'); }
    return sign * result;
}

void itoa(unsigned long long n, char* buffer, unsigned int base) {
    if (n == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    unsigned long long tmp = n;
    int len = 0;
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
