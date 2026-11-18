/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rows of the Gen9 MOCS table; workarounds.c includes them inside the
 * initializer of i915_gen9_mocs_table[].  A row is { index, control, l3cc }:
 * index 0 uncached, 1 as the PTE says, 2 cached, and 63 the entry the L3
 * uses for its evictions and for uncacheable cycles.  Every other index
 * takes the values of index 1.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_mocs.c
 * (skl_mocs_table, GEN9_MOCS_ENTRIES).
 */

	{  0, I915_GT_LE_1_UC | I915_GT_LE_TC_2_LLC_ELLC, I915_GT_L3_1_UC },
	{  1, I915_GT_LE_0_PAGETABLE | I915_GT_LE_TC_0_PAGETABLE | I915_GT_LE_LRUM(3), I915_GT_L3_3_WB },
	{  2, I915_GT_LE_3_WB | I915_GT_LE_TC_2_LLC_ELLC | I915_GT_LE_LRUM(3), I915_GT_L3_3_WB },
	{ 63, I915_GT_LE_3_WB | I915_GT_LE_TC_1_LLC | I915_GT_LE_LRUM(3), I915_GT_L3_1_UC },
