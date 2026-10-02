#ifndef CLIP_H
#define CLIP_H

#include <stdint.h>

// The clipboard. What one program copies, any other pastes: text from the
// editor into the terminal, files from the file manager into another of its
// windows. Kept in the kernel, so it outlives the program that copied.

#define CLIP_NONE  0
#define CLIP_TEXT  1    // UTF-8
#define CLIP_FILES 2    // "copy\n" or "cut\n", then one absolute path a line

#define CLIP_MAX (4u * 1024 * 1024)

int clip_set(int type, const void *src, uint32_t n);    // 0, or -1 if too big
// Copies up to `max` bytes; returns the whole length; *type its kind.
uint32_t clip_get(void *dst, uint32_t max, int *type);
uint32_t clip_seq(void);         // changes whenever something is copied
int clip_type(void);
uint32_t clip_len(void);

#endif
