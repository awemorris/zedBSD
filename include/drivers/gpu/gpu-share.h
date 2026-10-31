/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Independent allocation capabilities behind the common GPU resource API.
 */

#ifndef DRIVERS_GPU_SHARE_H
#define DRIVERS_GPU_SHARE_H

#include <kern/handle.h>
#include <stdint.h>
#include <uapi/gpu.h>
#include <drivers/gpu/gpu-scanout.h>

/*
 * One immutable sharing contract, independent of the exporting open lifetime.
 * Export returns one owned backend reference and leaves the session resource
 * valid. Release consumes only that reference and cannot fail. Import borrows
 * the shared object and returns an independently owned session resource; its
 * ordinary resource_destroy callback retires it. Failures return no ownership.
 * Import gets the request's GPU_IMPORT_* flags: GPU_IMPORT_SCANOUT asks for a
 * resource the backend's own display can scan out, which a backend may refuse
 * with ENOTSUP for an allocation its display cannot scan (the caller then
 * copies instead); a backend without that limit ignores the flags.
 * The GPU core retains the registered device until all exported references end.
 *
 * A nonnull image requests the immutable image contract: linear rows, or a
 * layout the backend's drv_gpu_layout_ops accepted before the export.  An
 * allocation already exported with one description is refused (EINVAL)
 * when exported again with another.
 * A null image requests allocation-only sharing and is used only when the
 * backend advertises GPU_CAP_ALLOCATION_SHARE. It makes no layout or scanout
 * promise. Application metadata stays in the common capability, not here.
 */
struct drv_gpu_share_ops {
	int (*export_resource)(void *, void *, void *, const struct gpu_image_descriptor *, void **);
	void (*release)(void *, void *);
	int (*import_resource)(void *, void *, void *, uint32_t, void **, uint32_t *);

	/* Optional physical view remains borrowed while the exported object lives. */
	int (*get_scanout_backing)(void *, void *, struct drv_gpu_scanout_backing *);

	/* Optional physical view of an allocation-only export, borrowed the same way. */
	int (*get_allocation_backing)(void *, void *, struct drv_gpu_scanout_backing *);
};

/*
 * The image layouts a device names besides linear rows (optional, both
 * operations together; drv_gpu_ops.layouts).  A display-only node has them
 * as a renderer does, since it may scan out a layout another device wrote.
 *
 * image_layout(device, image, bytes) checks a description whose tiling is
 * not GPU_IMAGE_LINEAR and reports the bytes from its offset that the image
 * takes; EOPNOTSUPP for a layout or variant the device neither writes nor
 * reads.  The core then bounds those bytes by the allocation.  It runs
 * without the device's locks and touches no hardware.
 *
 * image_layouts(device, session, request) answers GPU_IMAGE_LAYOUTS: the
 * core has checked the inputs and cleared the outputs; the backend fills
 * count and the entries, fastest first, each with every use the request
 * names, and the core checks the answer.  A layout listed other than
 * linear rows must be one image_layout accepts.
 *
 * A display presenting a blob of a layout other than linear rows bounds
 * and lays the image out by what its own export or import recorded; the
 * core's check of the present request's stride and height says nothing
 * about such an image.
 */
struct drv_gpu_layout_ops {
	int (*image_layout)(void *, const struct gpu_image_descriptor *, uint64_t *);
	int (*image_layouts)(void *, void *, struct gpu_image_layouts *);
};

/*
 * Describes the physical pages of an allocation-only GPU capability for
 * another kernel driver (a camera writing frames into memory a Vulkan
 * application allocated).  The handle is a descriptor's typed reference;
 * the pages stay valid while the caller keeps a reference to it.  The
 * exporting device's identity is reported.  EINVAL for a handle that is
 * not a GPU capability or carries an image, ENOTSUP for a backend
 * without the view.
 */
int drv_gpu_shared_backing(struct kernel_handle *handle, struct drv_gpu_scanout_backing *backing, uint64_t *device_identity);

#endif
