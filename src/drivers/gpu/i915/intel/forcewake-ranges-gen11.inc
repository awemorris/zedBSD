/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rows of the Gen11 register to forcewake domain map; mmio.c includes
 * them inside the initializer of i915_gen11_ranges[].  The always-on ranges
 * have no row, because no match already means always on; the second decode
 * engine of each pair is not used and has no row either.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/intel_uncore.c
 * (__gen11_fw_ranges).
 */

	/* 0x0 .. 0x1fff always-on */
	{ 0x2000u, 0x26ffu, I915_FORCEWAKE_RENDER },
	{ 0x2700u, 0x2fffu, I915_FORCEWAKE_GT },
	{ 0x3000u, 0x3fffu, I915_FORCEWAKE_RENDER },
	{ 0x4000u, 0x51ffu, I915_FORCEWAKE_GT },
	{ 0x5200u, 0x7fffu, I915_FORCEWAKE_RENDER },
	{ 0x8000u, 0x813fu, I915_FORCEWAKE_GT },
	{ 0x8140u, 0x815fu, I915_FORCEWAKE_RENDER },
	{ 0x8160u, 0x82ffu, I915_FORCEWAKE_GT },
	{ 0x8300u, 0x84ffu, I915_FORCEWAKE_RENDER },
	{ 0x8500u, 0x87ffu, I915_FORCEWAKE_GT },
	/* 0x8800 .. 0x8bff always-on */
	{ 0x8c00u, 0x8cffu, I915_FORCEWAKE_RENDER },
	{ 0x8d00u, 0x94cfu, I915_FORCEWAKE_GT },
	{ 0x94d0u, 0x955fu, I915_FORCEWAKE_RENDER },
	/* 0x9560 .. 0x95ff always-on */
	{ 0x9600u, 0xafffu, I915_FORCEWAKE_GT },
	{ 0xb000u, 0xb47fu, I915_FORCEWAKE_RENDER },
	{ 0xb480u, 0xdeffu, I915_FORCEWAKE_GT },
	{ 0xdf00u, 0xe8ffu, I915_FORCEWAKE_RENDER },
	{ 0xe900u, 0x16dffu, I915_FORCEWAKE_GT },
	{ 0x16e00u, 0x19fffu, I915_FORCEWAKE_RENDER },
	{ 0x1a000u, 0x23fffu, I915_FORCEWAKE_GT },
	/* 0x24000 .. 0x2407f always-on */
	{ 0x24080u, 0x2417fu, I915_FORCEWAKE_GT },
	{ 0x24180u, 0x242ffu, I915_FORCEWAKE_RENDER },
	{ 0x24300u, 0x243ffu, I915_FORCEWAKE_GT },
	{ 0x24400u, 0x24fffu, I915_FORCEWAKE_RENDER },
	{ 0x25000u, 0x3ffffu, I915_FORCEWAKE_GT },
	/* 0x40000 .. 0x1bffff always-on */
	{ 0x1c0000u, 0x1c3fffu, I915_FORCEWAKE_MEDIA_VDBOX0 },
	/* 0x1c4000 .. 0x1c7fff always-on */
	{ 0x1c8000u, 0x1cffffu, I915_FORCEWAKE_MEDIA_VEBOX0 },
	{ 0x1d0000u, 0x1d3fffu, I915_FORCEWAKE_MEDIA_VDBOX2 },
	/* 0x1d4000 .. 0x1dbfff always-on */
