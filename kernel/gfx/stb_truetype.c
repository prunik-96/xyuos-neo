// stb_truetype implementation TU, wired to the kernel's heap and math shims.
// Compiled with SSE enabled (kernel/gfx drops -mgeneral-regs-only).
#include "../mm/heap.h"
#include "kmath.h"

#define STBTT_malloc(x, u)  ((void)(u), kmalloc(x))
#define STBTT_free(x, u)    ((void)(u), kfree(x))
#define STBTT_assert(x)     ((void)0)

#define STBTT_ifloor(x)     ((int)k_floor(x))
#define STBTT_iceil(x)      ((int)k_ceil(x))
#define STBTT_sqrt(x)       k_sqrt(x)
#define STBTT_pow(x, y)     k_pow(x, y)
#define STBTT_fmod(x, y)    k_fmod(x, y)
#define STBTT_cos(x)        k_cos(x)
#define STBTT_acos(x)       k_acos(x)
#define STBTT_fabs(x)       k_fabs(x)

#define STBTT_strlen(x)     k_strlen(x)
#define STBTT_memcpy        k_memcpy
#define STBTT_memset        k_memset

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
