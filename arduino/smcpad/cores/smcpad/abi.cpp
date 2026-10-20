/* What C++ needs from a runtime without libstdc++ (compiled with -fno-exceptions -fno-rtti
 * -fno-threadsafe-statics -fno-use-cxa-atexit). */
#include <stdlib.h>
#include <stddef.h>

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

/* The SDK's heap has malloc/free but no realloc (String needs it). Provisional: copies `size`
 * bytes, which may read past the end of the old block -- harmless reads on this chip, but to be
 * replaced by one that knows the block size. */
extern "C" void *realloc(void *p, size_t size)
{
    void *q;

    if (!p) {
        return malloc(size);
    }
    if (!size) {
        free(p);
        return 0;
    }
    q = malloc(size);
    if (q) {
        __builtin_memcpy(q, p, size);
        free(p);
    }
    return q;
}
