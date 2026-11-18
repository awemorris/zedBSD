/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rows of the Gen12 register to forcewake domain map; mmio.c includes
 * them inside the initializer of i915_gen12_ranges[].  The always-on ranges
 * have no row, because no match already means always on.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/intel_uncore.c
 * (__gen12_fw_ranges, the map uncore_forcewake_init() gives Alder Lake-P).
 */

	/* 0x0 .. 0x1fff always-on */
	{ 0x2000u, 0x26ffu, I915_FORCEWAKE_RENDER },
	{ 0x2700u, 0x27ffu, I915_FORCEWAKE_GT },
	{ 0x2800u, 0x2affu, I915_FORCEWAKE_RENDER },
	{ 0x2b00u, 0x2fffu, I915_FORCEWAKE_GT },
	{ 0x3000u, 0x3fffu, I915_FORCEWAKE_RENDER },
	{ 0x4000u, 0x51ffu, I915_FORCEWAKE_GT },
	{ 0x5200u, 0x7fffu, I915_FORCEWAKE_RENDER },
	{ 0x8000u, 0x813fu, I915_FORCEWAKE_GT },
	{ 0x8140u, 0x815fu, I915_FORCEWAKE_RENDER },
	/* 0x8160 .. 0x81ff always-on */
	{ 0x8200u, 0x82ffu, I915_FORCEWAKE_GT },
	{ 0x8300u, 0x84ffu, I915_FORCEWAKE_RENDER },
	{ 0x8500u, 0x94cfu, I915_FORCEWAKE_GT },
	{ 0x94d0u, 0x955fu, I915_FORCEWAKE_RENDER },
	/* 0x9560 .. 0x97ff always-on */
	{ 0x9800u, 0xafffu, I915_FORCEWAKE_GT },
	{ 0xb000u, 0xb3ffu, I915_FORCEWAKE_RENDER },
	{ 0xb400u, 0xcfffu, I915_FORCEWAKE_GT },
	/* 0xd000 .. 0xd7ff always-on */
	{ 0xd800u, 0xd8ffu, I915_FORCEWAKE_RENDER },
	{ 0xd900u, 0xdbffu, I915_FORCEWAKE_GT },
	{ 0xdc00u, 0xefffu, I915_FORCEWAKE_RENDER },
	{ 0xf000u, 0x147ffu, I915_FORCEWAKE_GT },
	{ 0x14800u, 0x1ffffu, I915_FORCEWAKE_RENDER },
	{ 0x20000u, 0x20fffu, I915_FORCEWAKE_MEDIA_VDBOX0 },
	{ 0x21000u, 0x21fffu, I915_FORCEWAKE_MEDIA_VDBOX2 },
	{ 0x22000u, 0x23fffu, I915_FORCEWAKE_GT },
	/* 0x24000 .. 0x2417f always-on */
	{ 0x24180u, 0x249ffu, I915_FORCEWAKE_GT },
	{ 0x24a00u, 0x251ffu, I915_FORCEWAKE_RENDER },
	{ 0x25200u, 0x255ffu, I915_FORCEWAKE_GT },
	{ 0x25600u, 0x2567fu, I915_FORCEWAKE_MEDIA_VDBOX0 },
	{ 0x25680u, 0x259ffu, I915_FORCEWAKE_MEDIA_VDBOX2 },
	{ 0x25a00u, 0x25a7fu, I915_FORCEWAKE_MEDIA_VDBOX0 },
	{ 0x25a80u, 0x2ffffu, I915_FORCEWAKE_MEDIA_VDBOX2 },
	{ 0x30000u, 0x3ffffu, I915_FORCEWAKE_GT },
	/* 0x40000 .. 0x1bffff always-on */
	{ 0x1c0000u, 0x1c3fffu, I915_FORCEWAKE_MEDIA_VDBOX0 },
	/* 0x1c4000 .. 0x1c7fff always-on */
	{ 0x1c8000u, 0x1cbfffu, I915_FORCEWAKE_MEDIA_VEBOX0 },
	{ 0x1cc000u, 0x1cffffu, I915_FORCEWAKE_MEDIA_VDBOX0 },
	{ 0x1d0000u, 0x1d3fffu, I915_FORCEWAKE_MEDIA_VDBOX2 },
