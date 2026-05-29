#include <stdlib.h>

int atoi(const char* str) {
    if (str == nullptr) { return 0; }

    int sign = (*str == '-') ? -1 : 1;
    if (sign == -1) ++str;

    constexpr int INT_MAX_VALUE = 2147483647;
    constexpr int INT_MIN_VALUE = -2147483647 - 1;

    unsigned long long acc      = 0;
    while (*str >= '0' && *str <= '9') {
        acc = acc * 10 + (unsigned long long)(*str - '0');
        if (sign == 1 && acc > (unsigned long long)INT_MAX_VALUE) { return INT_MAX_VALUE; }
        if (sign == -1 && acc > (unsigned long long)INT_MAX_VALUE + 1) { return INT_MIN_VALUE; }
        ++str;
    }

    if (sign == -1) {
        if (acc == (unsigned long long)INT_MAX_VALUE + 1) { return INT_MIN_VALUE; }
        return -(int)acc;
    }
    return (int)acc;
}

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
