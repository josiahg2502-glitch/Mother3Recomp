/* ==========================================================================
 *  freestanding.c -- memset / memcpy / memcmp for wasm32-freestanding
 *
 *  Compiled to nothing on native targets (guarded by GB_FREESTANDING) so the
 *  host libc keeps its optimised versions.  Word-at-a-time with a byte tail;
 *  the core's hot paths are small fixed-size copies, so this is plenty.
 * ========================================================================== */
#include "gb_string.h"

#ifdef GB_FREESTANDING

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    unsigned char v = (unsigned char)c;

    /* align to 4 bytes, then fill a word at a time */
    while (n && ((uintptr_t)p & 3)) { *p++ = v; n--; }
    if (n >= 4) {
        uint32_t w = (uint32_t)v * 0x01010101u;
        uint32_t *wp = (uint32_t *)(void *)p;
        while (n >= 4) { *wp++ = w; n -= 4; }
        p = (unsigned char *)(void *)wp;
    }
    while (n--) *p++ = v;
    return s;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (dst == src) return dst;

    while (n && ((uintptr_t)d & 3)) { *d++ = *s++; n--; }
    if (n >= 4) {
        uint32_t *dw = (uint32_t *)(void *)d;
        const uint32_t *sw = (const uint32_t *)(const void *)s;
        while (n >= 4) { *dw++ = *sw++; n -= 4; }
        d = (unsigned char *)(void *)dw;
        s = (const unsigned char *)(const void *)sw;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++;
        q++;
    }
    return 0;
}

#endif /* GB_FREESTANDING */
