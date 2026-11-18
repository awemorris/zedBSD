/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The engines Alder Lake-P can have, each with its Gen11+ register base;
 * device-info.c includes them inside the table's initializer.  A row is
 * { id, class, instance, register base, name }.  VCS1 (0x1c4000) is not
 * listed.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_engine_cs.c
 * (intel_engines[]); drivers/gpu/drm/i915/i915_reg.h (RENDER_RING_BASE,
 * BLT_RING_BASE, GEN11_BSD_RING_BASE, GEN11_BSD3_RING_BASE,
 * GEN11_VEBOX_RING_BASE).
 */

	{ I915_RCS0, I915_RENDER_CLASS, 0, 0x02000U, "rcs0" },
	{ I915_BCS0, I915_COPY_ENGINE_CLASS, 0, 0x22000U, "bcs0" },
	{ I915_VCS0, I915_VIDEO_DECODE_CLASS, 0, 0x1c0000U, "vcs0" },
	{ I915_VCS2, I915_VIDEO_DECODE_CLASS, 2, 0x1d0000U, "vcs2" },
	{ I915_VECS0, I915_VIDEO_ENHANCEMENT_CLASS, 0, 0x1c8000U, "vecs0" },
