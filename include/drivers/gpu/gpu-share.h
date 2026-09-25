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

#include <uapi/gpu.h>
#include <drivers/gpu/gpu-scanout.h>

/*
 * One immutable sharing contract, independent of the exporting open lifetime.
 * Export returns one owned backend reference and leaves the session resource
 * valid. Release consumes only that reference and cannot fail. Import borrows
 * the shared object and returns an independently owned session resource; its
 * ordinary resource_destroy callback retires it. Failures return no ownership.
 * The GPU core retains the registered device until all exported references end.
 *
 * A nonnull image requests the existing immutable linear scanout contract.
 * A null image requests allocation-only sharing and is used only when the
 * backend advertises GPU_CAP_ALLOCATION_SHARE. It makes no layout or scanout
 * promise. Application metadata stays in the common capability, not here.
 */
struct drv_gpu_share_ops {
	int (*export_resource)(void *, void *, void *, const struct gpu_image_descriptor *, void **);
	void (*release)(void *, void *);
	int (*import_resource)(void *, void *, void *, void **, uint32_t *);

	/* Optional physical view remains borrowed while the exported object lives. */
	int (*get_scanout_backing)(void *, void *, struct drv_gpu_scanout_backing *);
};

#endif
