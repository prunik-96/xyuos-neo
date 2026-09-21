#ifndef INTTYPES_H
#define INTTYPES_H

// stdint.h itself comes from the compiler (freestanding); this adds the
// printf/scanf format macros that ELF headers and tcc expect.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRId8    "d"
#define PRId16   "d"
#define PRId32   "d"
#define PRId64   "ld"
#define PRIi32   "i"
#define PRIi64   "li"
#define PRIu8    "u"
#define PRIu16   "u"
#define PRIu32   "u"
#define PRIu64   "lu"
#define PRIx32   "x"
#define PRIx64   "lx"
#define PRIX32   "X"
#define PRIX64   "lX"
#define PRIo32   "o"
#define PRIo64   "lo"

#define PRIdPTR  "ld"
#define PRIuPTR  "lu"
#define PRIxPTR  "lx"

#define SCNd32   "d"
#define SCNd64   "ld"
#define SCNu32   "u"
#define SCNu64   "lu"
#define SCNx32   "x"
#define SCNx64   "lx"

typedef struct { intmax_t quot; intmax_t rem; } imaxdiv_t;

intmax_t imaxabs(intmax_t v);

#ifdef __cplusplus
}
#endif

#endif
