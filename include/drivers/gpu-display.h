/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Optional native display operations, independent of a rendering command ABI.
 */

#ifndef DRIVERS_GPU_DISPLAY_H
#define DRIVERS_GPU_DISPLAY_H

#include <uapi/gpu-display.h>

/*
 * Immutable display operations shared by every instance of one GPU backend.
 * The core passes validated kernel request copies and resolves presentation
 * storage before calling the backend. No callback runs under a core spinlock.
 * The backend owns lease arbitration, frame timing and scanout lifetimes.
 */
struct drv_gpu_display_ops {
	int (*query)(void *, void *, struct gpu_display_info *);
	int (*mode)(void *, void *, struct gpu_display_mode *);
	int (*claim)(void *, void *, struct gpu_display_claim *);
	int (*release)(void *, void *, const struct gpu_display_release *);
	int (*present)(void *, void *, void *, struct gpu_display_present *);
	int (*wait)(void *, void *, struct gpu_display_wait *);
};

#endif
