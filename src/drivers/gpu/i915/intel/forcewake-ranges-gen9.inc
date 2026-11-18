/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rows of the Gen9 register to forcewake domain map; mmio.c includes
 * them inside the initializer of i915_gen9_ranges[].  The copy engine's
 * domain is GT and the one media domain is I915_FORCEWAKE_MEDIA_GEN9.  The
 * two ranges that need RENDER and MEDIA together are listed under RENDER:
 * the running device holds every domain at once, so the map only decides
 * which hold the checked access looks for.  The always-on ranges have no
 * row.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/intel_uncore.c
 * (__gen9_fw_ranges).
 */

	{ 0x0u, 0xaffu, I915_FORCEWAKE_GT },
	/* 0xb00 .. 0x1fff always-on */
	{ 0x2000u, 0x26ffu, I915_FORCEWAKE_RENDER },
	{ 0x2700u, 0x2fffu, I915_FORCEWAKE_GT },
	{ 0x3000u, 0x3fffu, I915_FORCEWAKE_RENDER },
	{ 0x4000u, 0x51ffu, I915_FORCEWAKE_GT },
	{ 0x5200u, 0x7fffu, I915_FORCEWAKE_RENDER },
	{ 0x8000u, 0x812fu, I915_FORCEWAKE_GT },
	{ 0x8130u, 0x813fu, I915_FORCEWAKE_MEDIA_GEN9 },
	{ 0x8140u, 0x815fu, I915_FORCEWAKE_RENDER },
	{ 0x8160u, 0x82ffu, I915_FORCEWAKE_GT },
	{ 0x8300u, 0x84ffu, I915_FORCEWAKE_RENDER },
	{ 0x8500u, 0x87ffu, I915_FORCEWAKE_GT },
	{ 0x8800u, 0x89ffu, I915_FORCEWAKE_MEDIA_GEN9 },
	{ 0x8a00u, 0x8bffu, I915_FORCEWAKE_GT },
	{ 0x8c00u, 0x8cffu, I915_FORCEWAKE_RENDER },
	{ 0x8d00u, 0x93ffu, I915_FORCEWAKE_GT },
	/* 0x9400 .. 0x97ff needs RENDER and MEDIA together */
	{ 0x9400u, 0x97ffu, I915_FORCEWAKE_RENDER },
	{ 0x9800u, 0xafffu, I915_FORCEWAKE_GT },
	{ 0xb000u, 0xb47fu, I915_FORCEWAKE_RENDER },
	{ 0xb480u, 0xcfffu, I915_FORCEWAKE_GT },
	{ 0xd000u, 0xd7ffu, I915_FORCEWAKE_MEDIA_GEN9 },
	{ 0xd800u, 0xdfffu, I915_FORCEWAKE_GT },
	{ 0xe000u, 0xe8ffu, I915_FORCEWAKE_RENDER },
	{ 0xe900u, 0x11fffu, I915_FORCEWAKE_GT },
	{ 0x12000u, 0x13fffu, I915_FORCEWAKE_MEDIA_GEN9 },
	{ 0x14000u, 0x19fffu, I915_FORCEWAKE_GT },
	{ 0x1a000u, 0x1e9ffu, I915_FORCEWAKE_MEDIA_GEN9 },
	{ 0x1ea00u, 0x243ffu, I915_FORCEWAKE_GT },
	{ 0x24400u, 0x247ffu, I915_FORCEWAKE_RENDER },
	{ 0x24800u, 0x2ffffu, I915_FORCEWAKE_GT },
	{ 0x30000u, 0x3ffffu, I915_FORCEWAKE_MEDIA_GEN9 },
