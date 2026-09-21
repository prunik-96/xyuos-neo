/* zlib's shape, over the DEFLATE this system already had.
 *
 * NetSurf keeps its message catalogue gzipped and reads it two ways: line by
 * line off disk with gzopen/gzgets, and out of a block of memory with the
 * z_stream interface. Neither wants anything from zlib but decompression --
 * nothing in the browser core compresses -- so nothing here compresses, and
 * the names that would are not declared. Code that wants to deflate will say
 * so at compile time, by its own file name, rather than link and misbehave.
 *
 * The inflate underneath is userland/inflate.h, written for PNG.
 *
 * One honest limitation, spelt out because it is invisible otherwise: the
 * inflate here is one-shot. It takes a whole stream and gives back a whole
 * result. So inflate() treats the input present at the first call as the
 * entire stream, and afterwards only hands that result out in whatever
 * sized pieces the caller asks for. A caller that drains output in chunks --
 * which is what NetSurf does -- is served exactly. A caller that feeds input
 * in chunks is not, and gets Z_DATA_ERROR instead of a truncated answer.
 */
#ifndef ZLIB_H
#define ZLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZLIB_VERSION "1.2.11"

typedef unsigned char  Bytef;
typedef unsigned int   uInt;
typedef unsigned long  uLong;
typedef void          *voidpf;

#define Z_NULL 0

/* Return codes. */
#define Z_OK             0
#define Z_STREAM_END     1
#define Z_NEED_DICT      2
#define Z_ERRNO        (-1)
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR   (-3)
#define Z_MEM_ERROR    (-4)
#define Z_BUF_ERROR    (-5)
#define Z_VERSION_ERROR (-6)

/* Flush values. Accepted and ignored: there is nothing to flush when the
 * whole answer was computed at the first call. */
#define Z_NO_FLUSH      0
#define Z_PARTIAL_FLUSH 1
#define Z_SYNC_FLUSH    2
#define Z_FULL_FLUSH    3
#define Z_FINISH        4

/* The largest window DEFLATE defines. inflateInit2 takes this plus 32 to
 * mean "look at the first bytes and tell me which wrapper this is", plus 16
 * to mean gzip, and its negation to mean there is no wrapper at all. */
#define MAX_WBITS 15

struct z_state;

typedef struct z_stream_s {
    const Bytef *next_in;    /* what is left to read */
    uInt         avail_in;
    uLong        total_in;

    Bytef       *next_out;   /* where to put what comes back */
    uInt         avail_out;
    uLong        total_out;

    const char  *msg;        /* why it stopped, when it stopped badly */
    struct z_state *state;

    /* Declared because callers set them to Z_NULL on their way in. This
     * implementation allocates with malloc and never calls them. */
    voidpf     (*zalloc)(voidpf opaque, uInt items, uInt size);
    void       (*zfree)(voidpf opaque, voidpf address);
    voidpf       opaque;

    int          data_type;
    uLong        adler;
    uLong        reserved;
} z_stream;

typedef z_stream *z_streamp;

/* windowBits carries the wrapper question; see MAX_WBITS above. */
int inflateInit2(z_streamp strm, int windowBits);
int inflateInit(z_streamp strm);            /* zlib wrapper assumed */
int inflate(z_streamp strm, int flush);
int inflateEnd(z_streamp strm);
int inflateReset(z_streamp strm);

const char *zError(int err);
const char *zlibVersion(void);

/* --- the file side ------------------------------------------------------ */

struct gz_file;
typedef struct gz_file *gzFile;

/* Reads only; the mode is looked at far enough to refuse a write. A file
 * that is not gzipped opens too and reads through unchanged, which is the
 * behaviour the callers rely on for an uncompressed catalogue. */
gzFile gzopen(const char *path, const char *mode);
char  *gzgets(gzFile f, char *buf, int len);
int    gzread(gzFile f, void *buf, unsigned len);
int    gzeof(gzFile f);
int    gzclose(gzFile f);

#ifdef __cplusplus
}
#endif

#endif
