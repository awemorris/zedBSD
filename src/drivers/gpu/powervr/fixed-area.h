/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fixed area at the start of three heaps of every memory context of
 * the PowerVR core.
 *
 * The firmware and the hardware reach a few places of each context's
 * address space by their offset from a heap's base, without the driver
 * naming them in any command: in the general heap the fence the cache
 * flushes write and the colour conversion coefficients; in the data
 * sequencer heap the synchronisation program and the end-of-tile code;
 * in the shading cluster heap the program the synchronisation task
 * starts.  The fence must be backed by real memory even though what it
 * holds is never read.  A context's own allocations in these heaps start
 * on the page after the area.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_FIXED_AREA_H
#define KERN_DRIVERS_GPU_POWERVR_FIXED_AREA_H

#include <stdint.h>

/*
 * The places and sizes of the fixed area's parts, each from its heap's
 * base, and the bytes each heap gives the area: the end of its last
 * part rounded up to a whole page.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_vm.c (the static data
 * areas: the fence at the general heap's offset 0, 128 bytes; the
 * colour coefficients at 128, 1024 bytes; the synchronisation program at
 * the data sequencer heap's offset 0, 128 bytes; the end-of-tile code at
 * 128, 128 bytes; the synchronisation program at the shading cluster
 * heap's offset 0, 128 bytes; GET_RESERVED_SIZE: the last part's end
 * rounded up to the page size).
 */
#define POWERVR_FIXED_FENCE_OFFSET		0U
#define POWERVR_FIXED_FENCE_BYTES		128U
#define POWERVR_FIXED_COLOUR_OFFSET		128U
#define POWERVR_FIXED_COLOUR_BYTES		1024U
#define POWERVR_FIXED_SYNC_PDS_OFFSET		0U
#define POWERVR_FIXED_SYNC_PDS_BYTES		128U
#define POWERVR_FIXED_TILE_END_OFFSET		128U
#define POWERVR_FIXED_TILE_END_BYTES		128U
#define POWERVR_FIXED_SYNC_USC_OFFSET		0U
#define POWERVR_FIXED_SYNC_USC_BYTES		128U
#define POWERVR_FIXED_GENERAL_BYTES		0x1000U
#define POWERVR_FIXED_PDS_BYTES			0x1000U
#define POWERVR_FIXED_USC_BYTES			0x1000U

/* The heaps that hold a fixed area, in the order a context keeps their pages. */
#define POWERVR_FIXED_HEAP_GENERAL		0U
#define POWERVR_FIXED_HEAP_PDS			1U
#define POWERVR_FIXED_HEAP_USC			2U
#define POWERVR_FIXED_HEAPS			3U

int powervr_fixed_area_write(volatile uint8_t *pds_page, volatile uint8_t *usc_page);

#endif /* KERN_DRIVERS_GPU_POWERVR_FIXED_AREA_H */
