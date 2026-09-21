#ifndef XYUOS_NET_HTTP_H
#define XYUOS_NET_HTTP_H

#include <stdint.h>

/* Reading an HTTP response.
 *
 * Both transports speak the same protocol above the socket, and for a while
 * they each had their own idea of it: kernel/net/tls.c rejoined a chunked
 * body, kernel/net/net.c handed one over still in pieces, and neither touched
 * the header that said so. A browser cannot tell those apart -- it has one
 * header and two possible meanings -- so it guessed, and guessed wrong half
 * the time. The answer is not a better guess. It is one implementation.
 *
 * Every function here reads a response that is already in memory. Nothing in
 * this file touches a socket, which is what lets it be shared. */

/* Does the named header carry the given token? `name` must begin with a
 * newline so it can only match at the start of a line -- otherwise
 * "x-original-content-length" answers for "content-length". */
int http_header_token(const uint8_t *h, int len, const char *name,
                      const char *tok);

int http_is_chunked(const uint8_t *h, int len);

/* The declared body length, or -1 when the header is absent. */
int http_content_length(const uint8_t *h, int len);

/* The three-digit code from the status line, or 0. */
int http_status(const uint8_t *h, int len);

/* Will the connection still be there afterwards? HTTP/1.1 keeps it unless
 * told otherwise; HTTP/1.0 drops it unless told otherwise. */
int http_keeps_alive(const uint8_t *h, int len);

/* Has the terminating zero-length chunk arrived? This is what lets a reader
 * stop at the true end of a body instead of waiting for the connection to
 * close. It walks the framing without decoding it. */
int http_chunked_complete(const uint8_t *p, int len);

/* Rewrite a chunked body in place as the bytes it actually carries, and
 * return the new length. Each chunk is a hex length, CRLF, that many bytes,
 * CRLF; a zero length ends it. */
int http_dechunk(uint8_t *p, int len);

/* Rename the Transfer-Encoding header of a body that has just been rejoined.
 *
 * Deleting the line would mean moving the body, so instead the name is
 * overwritten with one of exactly the same width -- the value is left alone,
 * and `X-Xyuos-Dechunked: chunked` says what actually happened. After this
 * the absence of Transfer-Encoding means what a reader expects it to mean:
 * the bytes below are the body. */
void http_mark_dechunked(uint8_t *h, int len);

#endif
