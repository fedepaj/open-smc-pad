#include <stdint.h>
#include <string.h>
#include "smcpad_rt.h"

static uint32_t count;

static void say(const char *s) { smcpad_serial_write((const uint8_t *)s, strlen(s)); }

void setup(void)
{
}

void loop(void)
{
    char line[32], digits[12];
    int n = 0;
    uint32_t v = ++count;

    do { digits[n++] = '0' + v % 10; v /= 10; } while (v);
    strcpy(line, "hello from a sketch, #");
    int l = strlen(line);
    while (n) line[l++] = digits[--n];
    line[l++] = '\r'; line[l++] = '\n'; line[l] = 0;
    say(line);
    smcpad_delay(1000);
}
