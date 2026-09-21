#ifndef KMATH_H
#define KMATH_H

#include <stddef.h>

// Minimal freestanding libm/libc shims for stb_truetype. Compiled with SSE
// enabled (kernel/gfx has no -mgeneral-regs-only). Only floor/ceil/fabs/fmod/
// sqrt are on the glyph-rasterization path; pow/cos/acos exist only to satisfy
// compilation of stb code paths we never call (packing/SDF).
double k_sqrt(double x);
double k_floor(double x);
double k_ceil(double x);
double k_fabs(double x);
double k_fmod(double x, double y);
double k_pow(double x, double y);
double k_cos(double x);
double k_acos(double x);

void  *k_memcpy(void *dst, const void *src, size_t n);
void  *k_memset(void *dst, int c, size_t n);
size_t k_strlen(const char *s);

#endif
