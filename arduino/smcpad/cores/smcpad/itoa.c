/* itoa & co. for String (not in newlib). */
#include <stdlib.h>
#include <string.h>

static char *u_to_a(unsigned long value, char *out, int base, int negative)
{
    char tmp[34];
    int n = 0, i = 0;

    if (base < 2 || base > 36) {
        *out = 0;
        return out;
    }
    do {
        int d = value % base;
        tmp[n++] = d < 10 ? '0' + d : 'a' + d - 10;
        value /= base;
    } while (value);
    if (negative) {
        out[i++] = '-';
    }
    while (n) {
        out[i++] = tmp[--n];
    }
    out[i] = 0;
    return out;
}

char *ultoa(unsigned long value, char *out, int base) { return u_to_a(value, out, base, 0); }
char *utoa(unsigned value, char *out, int base) { return u_to_a(value, out, base, 0); }

char *ltoa(long value, char *out, int base)
{
    if (base == 10 && value < 0) {
        return u_to_a(-(unsigned long)value, out, base, 1);
    }
    return u_to_a((unsigned long)value, out, base, 0);
}

char *itoa(int value, char *out, int base) { return ltoa(value, out, base); }
