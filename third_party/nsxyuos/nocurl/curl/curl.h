/* Not curl. This exists so that one line in NetSurf can be read.
 *
 * content/fetch.c includes content/fetchers/curl.h whatever it was built
 * with, and that header ends with:
 *
 *     extern CURLM *fetch_curl_multi;
 *
 * A declaration of a pointer to a type nothing here has. The call that would
 * use it sits behind WITH_CURL and is not compiled, so the only thing
 * standing between this build and a working fetch factory is a name for that
 * type.
 *
 * So the type is named and left INCOMPLETE. A pointer to it declares fine,
 * which is all fetch.c asks. Anything that tried to use it -- pass it to a
 * curl call, look inside it, take its size -- does not compile, and says so
 * at the line that tried. That is the point: this must never be mistakable
 * for curl being present. The fetcher that does the work is
 * third_party/nsxyuos/fetch_xyuos.c, over this system's own network calls.
 */
#ifndef XYUOS_NOT_CURL_H
#define XYUOS_NOT_CURL_H

typedef struct curl_multi_that_is_not_here CURLM;
typedef struct curl_easy_that_is_not_here  CURL;

#endif
