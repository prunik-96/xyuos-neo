/* Addresses to text and back. */
#ifndef ARPA_INET_H
#define ARPA_INET_H

#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read a dotted quad or a colon-separated v6 address into `dst`. Returns 1 if
 * it was one, 0 if it was not, and -1 if the family is not one we know. */
int inet_pton(int af, const char *src, void *dst);

/* The older, lenient form: it accepts "10.1" and "0x7f000001" as well as a
 * full four-part address, because that is what it has always accepted and
 * callers rely on it. Returns 1 for an address it understood, 0 otherwise. */
int inet_aton(const char *src, struct in_addr *dst);

/* And back to text. Returns `dst`, or NULL if it would not fit. */
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);

/* The two oldest: a dotted quad to an address (INADDR_NONE if it is not
 * one), and back -- into a buffer of its own, overwritten every call. */
in_addr_t inet_addr(const char *src);
char     *inet_ntoa(struct in_addr in);

#ifdef __cplusplus
}
#endif

#endif
