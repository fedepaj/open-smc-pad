/* What C++ needs from a runtime without libstdc++ (compiled with -fno-exceptions -fno-rtti
 * -fno-threadsafe-statics -fno-use-cxa-atexit). */
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

void *operator new(size_t size) { return malloc(size); }
void *operator new[](size_t size) { return malloc(size); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" void __cxa_pure_virtual(void)
{
    for (;;) {
    }
}

extern "C" void *__dso_handle = 0;

/* Global destructors never run (the firmware does not exit): nothing to register. */
extern "C" int atexit(void (*)(void)) { return 0; }

/* The SDK's heap (FreeRTOS heap_4 as JieLi changed it, pvPortMalloc in system.a) has
 * malloc/free but no realloc. A block is preceded by one word: its size, header included, with
 * the bits 0x69680000 set while allocated (free() checks them). Read through a volatile
 * address: before the block is outside it as far as the compiler knows.
 * Note: when the heap runs out, the SDK's malloc logs and spins until the watchdog resets. */
#define HEAP_MAGIC 0x69680000u

static size_t usable_size(void *p)
{
    uint32_t h = *(volatile uint32_t *)((uintptr_t)p - 4);

    if ((h & HEAP_MAGIC) != HEAP_MAGIC) {
        return 0;
    }
    h &= ~HEAP_MAGIC & ~1u;
    return h >= 4 ? h - 4 : 0;
}

extern "C" size_t smcpad_malloc_usable_size(void *p)
{
    return p ? usable_size(p) : 0;
}

extern "C" void *realloc(void *p, size_t size)
{
    size_t have;
    void *q;

    if (!p) {
        return malloc(size);
    }
    if (!size) {
        free(p);
        return 0;
    }
    have = usable_size(p);
    if (have >= size) {
        return p;                           /* fits (shrinking keeps the block) */
    }
    q = malloc(size);
    if (q) {
        __builtin_memcpy(q, p, have ? have : size);
        free(p);
    }
    return q;
}

/* Nor calloc. */
extern "C" void *calloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p = (size && total / size != n) ? 0 : malloc(total);

    if (p) {
        __builtin_memset(p, 0, total);
    }
    return p;
}

/* newlib's reentrant entry points (strtod & co. allocate through them) */
struct _reent;
extern "C" void *_malloc_r(struct _reent *, size_t size) { return malloc(size); }
extern "C" void *_calloc_r(struct _reent *, size_t n, size_t size) { return calloc(n, size); }
extern "C" void *_realloc_r(struct _reent *, void *p, size_t size) { return realloc(p, size); }
extern "C" void _free_r(struct _reent *, void *p) { free(p); }
