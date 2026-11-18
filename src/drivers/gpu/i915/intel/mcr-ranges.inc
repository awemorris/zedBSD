/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Gen12 (below 12.50) multicast register ranges; verify-workarounds.c
 * includes them inside the table's initializer.  A row is { start, end },
 * both inclusive: a command streamer read in one of them reaches an
 * unsteered instance.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/gt/intel_workarounds.c (mcr_ranges_gen12).
 */

	{ 0x8150U, 0x815fU },
	{ 0x9520U, 0x955fU },
	{ 0xb100U, 0xb3ffU },
	{ 0xde80U, 0xe8ffU },
	{ 0x24a00U, 0x24a7fU },
