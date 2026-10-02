#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>

// The virtio GPU, as QEMU offers it (-vga virtio): a display that is told
// which rectangles of a picture in ordinary memory changed, instead of being
// written to pixel by pixel. In a virtual machine that is the difference
// between a smooth desktop and a slide show: writes to an emulated card's
// video memory crawl (about 90 MB/s measured under KVM in WSL), while the
// host copies a rectangle out of guest memory at memory speed.
//
// Only the 2D part is used: one resource, backed by the desktop's back
// buffer, shown on scanout 0.

// Find the device and set it up. 1 if there is one ready to take the screen.
int vgpu_init(void);

// Show `px` (w x h pixels, `pitch` bytes a row, XRGB) from now on. The
// memory must stay where it is. 1 on success.
int vgpu_take_screen(void *px, uint32_t pitch, uint32_t w, uint32_t h);

// 1 once the screen is ours: fb_present goes through vgpu_update.
int vgpu_active(void);

// These rectangles of the picture changed: copy them across and show them.
// rects is n x {x0, y0, x1, y1}. Returns once the host has them.
void vgpu_update(const uint32_t (*rects)[4], int n);

#endif
