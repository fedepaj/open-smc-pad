/* The few C library functions the bootloader (and TinyUSB) need; no libc is linked. */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        while (n--) {
            d[n] = s[n];
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;

    for (; n; n--, x++, y++) {
        if (*x != *y) {
            return *x - *y;
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/* The bootloader runs with interrupts off and never enables the USB interrupt: dcd_br25.c's
 * masking around SIE accesses has nothing to mask. */
void bit_clr_ie(unsigned char index)
{
    (void)index;
}

void bit_set_ie(unsigned char index)
{
    (void)index;
}
