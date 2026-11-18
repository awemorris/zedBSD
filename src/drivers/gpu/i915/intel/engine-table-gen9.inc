/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The engines a Skylake, Kaby Lake, Coffee Lake or Comet Lake GT can have,
 * each with its graphics version 9 register base; device-info.c includes
 * them inside the table's initializer.  A row is
 * { id, class, instance, register base, name }.  The second video decode
 * engine of the GT3 and GT4 parts (VCS1, 0x1c000) is left out: the driver
 * offers no video work on Gen9.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_engine_cs.c
 * (intel_engines[]); drivers/gpu/drm/i915/i915_reg.h (RENDER_RING_BASE,
 * BLT_RING_BASE, GEN6_BSD_RING_BASE, VEBOX_RING_BASE).
 */

	{ I915_RCS0, I915_RENDER_CLASS, 0, 0x02000U, "rcs0" },
	{ I915_BCS0, I915_COPY_ENGINE_CLASS, 0, 0x22000U, "bcs0" },
	{ I915_VCS0, I915_VIDEO_DECODE_CLASS, 0, 0x12000U, "vcs0" },
	{ I915_VECS0, I915_VIDEO_ENHANCEMENT_CLASS, 0, 0x1a000U, "vecs0" },
