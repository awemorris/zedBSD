/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the PowerVR driver's display node may ask of its render node: the
 * buffer behind an image the render node exported, so the display node
 * can read it without going through physical pages it was only lent.
 *
 * A buffer is one physically contiguous run below the core's DMA limit,
 * which the host reaches only through its uncached kernel view.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_RENDER_H
#define KERN_DRIVERS_GPU_POWERVR_RENDER_H

#include <stdint.h>

#include <drivers/gpu/gpu-scanout.h>
#include <uapi/gpu.h>

struct powervr_buffer;

/*
 * Finds the render buffer exported with this image whose backing is this
 * one, and retains it for the caller.
 *
 * The backing is matched by identity: it must be the very page list the
 * render node handed out for the export, so a foreign or forged one is
 * never taken for a render buffer.  ENOTSUP means no live export of the
 * render node matches; nothing is retained then.
 */
int powervr_render_lookup(const struct gpu_image_descriptor *image, const struct drv_gpu_scanout_backing *backing, struct powervr_buffer **result);

/*
 * Reports a retained buffer's uncached kernel view.  The view stays valid
 * until the caller's reference is released; only plain aligned loads may
 * read it.
 */
const volatile uint8_t *powervr_buffer_view(const struct powervr_buffer *buffer);

/* Reports a retained buffer's size in bytes, as allocated. */
uint64_t powervr_buffer_bytes(const struct powervr_buffer *buffer);

/* Releases a reference taken by powervr_render_lookup(). */
void powervr_buffer_release(struct powervr_buffer *buffer);

#endif
