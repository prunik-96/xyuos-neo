/* iconv's shape, over the character-set codecs NetSurf already ships.
 *
 * libparserutils carries a full set of decoders -- ASCII, the ISO 8859
 * parts, the windows-125x and KOI8 family, UTF-8, UTF-16 -- because a parser
 * has to read pages in whatever they were written in. Each of them converts
 * between its own encoding and UCS-4 in both directions. iconv is exactly
 * that pair joined in the middle: decode the source into UCS-4, encode the
 * UCS-4 into the destination. So this is a joining, not a stub, and it
 * supports every encoding the parser does.
 *
 * The codecs are put in their strict error mode on purpose. iconv's callers
 * are written against a function that reports EILSEQ and lets them decide --
 * NetSurf's HTML path answers it by writing the character out as an entity
 * instead. A codec left in its default lenient mode would quietly substitute
 * a replacement character and that decision would never be offered.
 */
#ifndef ICONV_H
#define ICONV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *iconv_t;

/* Opens a conversion from `from` to `to`. A "//TRANSLIT" or "//IGNORE"
 * suffix is accepted and ignored -- what follows the slashes is a request
 * about error handling, and the answer here is the strict one either way.
 * Returns (iconv_t)-1 with errno EINVAL if either name is not one the
 * codecs know. */
iconv_t iconv_open(const char *to, const char *from);

/* Converts, updating all four in place as it goes. Returns the number of
 * characters converted inexactly, which here is always 0, or (size_t)-1
 * with errno:
 *
 *   E2BIG   the output ran out; input is left pointing at what did not fit
 *   EILSEQ  a byte sequence the source encoding does not allow, or a
 *           character the destination cannot represent
 *   EINVAL  the input stops part way through a character
 *
 * Called with a null inbuf it resets the descriptor, as iconv does. */
size_t iconv(iconv_t cd, char **inbuf, size_t *inleft,
             char **outbuf, size_t *outleft);

int iconv_close(iconv_t cd);

#ifdef __cplusplus
}
#endif

#endif
