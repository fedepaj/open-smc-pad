/*
 * snprintf & co. for sketches. The toolchain's newlib has no printf family, and the SDK's (in
 * the core object, kept internal) has no floating point; this one does: flags - + space # 0,
 * width and precision (also *), length hh h l ll z j t, conversions d i u o x X c s p f F e E
 * g G %. No %n, no wide characters.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t size, n;             /* capacity, characters produced so far */
} out_t;

static void put(out_t *o, char c)
{
    if (o->n + 1 < o->size) {
        o->buf[o->n] = c;
    }
    o->n++;
}

enum { F_LEFT = 1, F_PLUS = 2, F_SPACE = 4, F_ALT = 8, F_ZERO = 16 };

/* sign/prefix, then zero padding up to `zeros` digits, then the digits, all padded to width */
static void emit(out_t *o, const char *prefix, const char *digits, int ndigits, int zeros,
                 int width, int flags)
{
    int plen = strlen(prefix), len = plen + (zeros > ndigits ? zeros : ndigits), i;

    if (!(flags & F_LEFT) && !(flags & F_ZERO)) {
        for (i = len; i < width; i++) put(o, ' ');
    }
    for (i = 0; i < plen; i++) put(o, prefix[i]);
    if (!(flags & F_LEFT) && (flags & F_ZERO)) {
        for (i = len; i < width; i++) put(o, '0');
    }
    for (i = ndigits; i < zeros; i++) put(o, '0');
    for (i = 0; i < ndigits; i++) put(o, digits[i]);
    if (flags & F_LEFT) {
        for (i = len; i < width; i++) put(o, ' ');
    }
}

static int utoa_base(uint64_t v, unsigned base, int upper, char *out)
{
    char tmp[24];
    int n = 0, i;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    do {
        tmp[n++] = dig[v % base];
        v /= base;
    } while (v);
    for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

/* ------------------------------------------------------------------ floating point */

static const double pow10_tab[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                    1e12, 1e13, 1e14, 1e15, 1e16, 1e17 };

/* v >= 0: digits of v rounded to `prec` decimals; returns the length, *dot = digits before
 * the point. v must be < 1e17 (callers switch to exponent form above). */
static int fixed_digits(double v, int prec, char *out, int *dot)
{
    uint64_t ip, fp;
    int n, i, p = prec > 17 ? 17 : prec;
    double scale = pow10_tab[p], frac;

    ip = (uint64_t)v;
    frac = (v - (double)ip) * scale + 0.5;
    fp = (uint64_t)frac;
    if (fp >= (uint64_t)scale) {            /* rounding carried into the integer part */
        fp -= (uint64_t)scale;
        ip++;
    }
    n = utoa_base(ip, 10, 0, out);
    *dot = n;
    for (i = p - 1; i >= 0; i--) {
        out[n + i] = '0' + fp % 10;
        fp /= 10;
    }
    n += p;
    for (i = p; i < prec; i++) out[n++] = '0';
    return n;
}

/* v > 0: v = m * 10^e with 1 <= m < 10 */
static double normalize(double v, int *e)
{
    int x = 0;

    while (v >= 1e16) { v /= 1e16; x += 16; }
    while (v >= 10.0) { v /= 10.0; x++; }
    while (v < 1e-16) { v *= 1e16; x -= 16; }
    while (v < 1.0) { v *= 10.0; x--; }
    *e = x;
    return v;
}

static void fmt_float(out_t *o, double v, char conv, int prec, int width, int flags)
{
    char digits[64], prefix[2] = { 0, 0 }, body[80];
    int upper = conv == 'F' || conv == 'E' || conv == 'G', n = 0, dot, e = 0, i;
    int style = conv | 0x20;                /* f, e or g */
    int strip = 0;

    if (v < 0 || (v == 0 && 1 / v < 0)) {
        prefix[0] = '-';
        v = -v;
    } else if (flags & F_PLUS) {
        prefix[0] = '+';
    } else if (flags & F_SPACE) {
        prefix[0] = ' ';
    }
    if (v != v || v > 1.7976931348623157e308) {
        emit(o, prefix, v != v ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf"), 3, 0, width,
             flags & ~F_ZERO);
        return;
    }
    if (prec < 0) {
        prec = 6;
    }
    if (prec > 40) {
        prec = 40;
    }
    if (style == 'g') {
        int p = prec ? prec : 1, x = 0;

        if (v != 0) {
            /* the exponent after rounding to p significant digits */
            double m = normalize(v, &x);
            char tmp[64];

            if (fixed_digits(m, p - 1, tmp, &dot) && dot > 1) {
                x++;
            }
        }
        if (x < p && x >= -4) {
            style = 'f';
            prec = p - 1 - x;
        } else {
            style = 'e';
            prec = p - 1;
        }
        strip = !(flags & F_ALT);
    }
    if (style == 'f' && v >= 1e17) {
        style = 'e';                        /* beyond what fixed_digits can hold */
    }
    if (style == 'e') {
        double m = v != 0 ? normalize(v, &e) : 0;
        int len = fixed_digits(m, prec, digits, &dot);

        if (dot > 1) {                      /* 9.99.. rounded up to 10 */
            e++;
            len = fixed_digits(m / 10, prec, digits, &dot);
        }
        body[n++] = digits[0];
        if (prec || (flags & F_ALT)) body[n++] = '.';
        for (i = 1; i < len; i++) body[n++] = digits[i];
        if (strip) {
            while (n > 1 && body[n - 1] == '0') n--;
            if (body[n - 1] == '.') n--;
        }
        body[n++] = upper ? 'E' : 'e';
        body[n++] = e < 0 ? '-' : '+';
        if (e < 0) e = -e;
        if (e >= 100) body[n++] = '0' + e / 100;
        body[n++] = '0' + e / 10 % 10;
        body[n++] = '0' + e % 10;
    } else {
        int len = fixed_digits(v, prec, digits, &dot);

        for (i = 0; i < dot; i++) body[n++] = digits[i];
        if (prec || (flags & F_ALT)) body[n++] = '.';
        for (i = dot; i < len; i++) body[n++] = digits[i];
        if (strip && prec) {
            while (body[n - 1] == '0') n--;
            if (body[n - 1] == '.') n--;
        }
    }
    emit(o, prefix, body, n, 0, width, flags);
}

/* ------------------------------------------------------------------ the formatter */

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    out_t o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        int flags = 0, width = 0, prec = -1, len = 0;
        char digits[24];

        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        for (;;) {
            char c = *++fmt;

            if (c == '-') flags |= F_LEFT;
            else if (c == '+') flags |= F_PLUS;
            else if (c == ' ') flags |= F_SPACE;
            else if (c == '#') flags |= F_ALT;
            else if (c == '0') flags |= F_ZERO;
            else break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= F_LEFT;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + *fmt++ - '0';
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + *fmt++ - '0';
            }
        }
        /* length: 1 hh, 2 h, 3 l, 4 ll; z j t are 32 / 64 bits as on this target */
        for (;;) {
            if (*fmt == 'h') len = len == 2 ? 1 : 2;
            else if (*fmt == 'l') len = len == 3 ? 4 : 3;
            else if (*fmt == 'j') len = 4;
            else if (*fmt == 'z' || *fmt == 't') len = 3;
            else if (*fmt == 'L') len = 0;
            else break;
            fmt++;
        }
        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = len == 4 ? va_arg(ap, long long) : len == 3 ? va_arg(ap, long) : va_arg(ap, int);
            char prefix[2] = { 0, 0 };
            uint64_t u;

            if (len == 1) v = (signed char)v;
            if (len == 2) v = (short)v;
            u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            prefix[0] = v < 0 ? '-' : (flags & F_PLUS) ? '+' : (flags & F_SPACE) ? ' ' : 0;
            if (prec >= 0) flags &= ~F_ZERO;
            emit(&o, prefix, digits, prec == 0 && u == 0 ? 0 : utoa_base(u, 10, 0, digits),
                 prec, width, flags);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            uint64_t u;
            unsigned base = *fmt == 'o' ? 8 : *fmt == 'u' ? 10 : 16;
            const char *prefix = "";
            int n;

            if (*fmt == 'p') {
                u = (uintptr_t)va_arg(ap, void *);
                prefix = "0x";
            } else {
                u = len == 4 ? va_arg(ap, unsigned long long) : len == 3 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
                if (len == 1) u = (unsigned char)u;
                if (len == 2) u = (unsigned short)u;
                if ((flags & F_ALT) && u && base == 16) prefix = *fmt == 'X' ? "0X" : "0x";
            }
            if (prec >= 0) flags &= ~F_ZERO;
            n = prec == 0 && u == 0 ? 0 : utoa_base(u, base, *fmt == 'X', digits);
            if ((flags & F_ALT) && base == 8 && (n == 0 || digits[0] != '0') && prec <= n) prec = n + 1;
            emit(&o, prefix, digits, n, prec, width, flags);
            break;
        }
        case 'c':
            digits[0] = (char)va_arg(ap, int);
            emit(&o, "", digits, 1, 0, width, flags & ~F_ZERO);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            int n = 0;

            if (!s) s = "(null)";
            while (s[n] && (prec < 0 || n < prec)) n++;
            emit(&o, "", s, n, 0, width, flags & ~F_ZERO);
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
            fmt_float(&o, va_arg(ap, double), *fmt, prec, width, flags);
            break;
        case '%':
            put(&o, '%');
            break;
        case 0:
            fmt--;                          /* a lone % at the end */
            break;
        default:
            put(&o, '%');
            put(&o, *fmt);
            break;
        }
    }
    if (size) {
        buf[o.n < size ? o.n : size - 1] = 0;
    }
    return (int)o.n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, (size_t)-1 >> 1, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}
