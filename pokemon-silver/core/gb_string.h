/* ==========================================================================
 *  gb_string.h -- the core's only "libc" dependency, isolated in one place
 *
 *  Native builds get the real <string.h>.  Freestanding builds (wasm32) get
 *  the three functions the core actually uses, implemented in
 *  freestanding.c.  Keeping it here means the emulator has zero external
 *  dependencies in every target.
 * ========================================================================== */
#ifndef GB_STRING_H
#define GB_STRING_H

#ifdef GB_FREESTANDING
#include <stdint.h>
#include <stddef.h>
void *memset(void *s, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memmove(void *dst, const void *src, size_t n);
#else
#include <string.h>
#endif

#endif /* GB_STRING_H */
