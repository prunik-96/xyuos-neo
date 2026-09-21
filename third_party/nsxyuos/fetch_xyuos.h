/* NetSurf's http and https fetcher, over xyuOS's network syscall.
 *
 * Call this after netsurf_init(). fetcher_add() is public and does its own
 * setting up, so nothing in NetSurf has to be patched to reach this: without
 * WITH_CURL its own fetcher_init() simply registers no http fetcher, and this
 * fills the gap afterwards.
 */
#ifndef FETCH_XYUOS_H
#define FETCH_XYUOS_H

#include "utils/errors.h"

/* Registers for both http and https. */
nserror fetch_xyuos_register(void);

#endif
