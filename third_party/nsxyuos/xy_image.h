/* The picture handler, over this system's own decoders.
 *
 * Call this after netsurf_init(), like the fetcher: NetSurf's own image
 * handlers are not compiled in -- each wants a library that is not here --
 * so nothing has registered for these types and this fills the gap without
 * patching anything.
 */
#ifndef XY_IMAGE_H
#define XY_IMAGE_H

#include "utils/errors.h"

/* Registers for PNG, JPEG, BMP, WebP, SVG and their usual aliases. */
nserror xy_image_init(void);

#endif
