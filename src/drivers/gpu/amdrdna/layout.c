/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Planning where things may go in the VRAM of an RDNA GPU.
 *
 * Some ranges belong to others and are kept out of every allocation: the
 * top megabyte, where the GPU keeps its block table and the security
 * processor its boot database and training buffers; the firmware's
 * picture, which stays the console's surface; the ranges the video BIOS
 * keeps; and the lowest 32 MiB, which memory training overwrites, until
 * training has run or has been skipped.  What remains forms two pools: the
 * part the CPU reaches through its 256 MiB window, and the part above it
 * that only the GPU reaches.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The top range kept for the block table, the boot database and the
 * training buffers, and the low range memory training overwrites.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h (PSP_RUNTIME_DB_OFFSET,
 * BIST_MEM_TRAINING_ENCROACHED_SIZE).
 */
#define LAYOUT_TOP_BYTES		0x100000ULL
#define LAYOUT_TRAINING_BYTES		0x2000000ULL

/*
 * The driver's own choices: the page the ranges are rounded to, and the
 * least VRAM the plan takes (the top range, the training range and room
 * above them).
 */
#define LAYOUT_PAGE_BYTES		4096ULL
#define LAYOUT_LEAST_VRAM		(64ULL * 1024U * 1024U)

/*
 * The names the log gives the reasons a range is kept, in the order of
 * enum amdrdna_range_reason.
 */
static const char *const range_reason_names[] = {
	"top",
	"gop",
	"fw",
	"drv",
	"train"
};

static int keep_range(struct amdrdna_layout *layout, uint64_t start, uint64_t bytes, uint64_t vram_bytes, enum amdrdna_range_reason reason);

/*
 * Plans the VRAM layout from what the earlier stages read.
 *
 * Returns 0 with the layout planned, or EINVAL when the VRAM is too small
 * or a range the input names lies outside it.
 */
int
amdrdna_layout_plan(
	const struct amdrdna_layout_input *input,
	struct amdrdna_layout *layout)
{
	uint64_t top_start;
	uint64_t window;
	int error;

	/* Starts with nothing kept and no pool. */
	layout->reserved_count = 0U;
	layout->cpu_pool_start = 0U;
	layout->cpu_pool_end = 0U;
	layout->gpu_pool_start = 0U;
	layout->gpu_pool_end = 0U;

	/* A VRAM too small for the fixed ranges cannot be planned. */
	if (input->vram_bytes < LAYOUT_LEAST_VRAM)
		return EINVAL;

	/* Keeps the top range. */
	top_start = input->vram_bytes - LAYOUT_TOP_BYTES;
	error = keep_range(layout, top_start, LAYOUT_TOP_BYTES, input->vram_bytes, AMDRDNA_RANGE_TOP);
	if (error != 0)
		return error;

	/* Keeps the firmware's picture, which stays the console's surface. */
	if (input->gop_present) {
		error = keep_range(layout, input->gop_offset, input->gop_bytes, input->vram_bytes, AMDRDNA_RANGE_GOP);
		if (error != 0)
			return error;
	}

	/* Keeps the range the video BIOS reserves for the firmware. */
	if (input->firmware_bytes != 0U) {
		error = keep_range(layout, input->firmware_start, input->firmware_bytes, input->vram_bytes, AMDRDNA_RANGE_FIRMWARE);
		if (error != 0)
			return error;
	}

	/* Keeps the range the video BIOS reserves for the driver's own use. */
	if (input->driver_bytes != 0U) {
		error = keep_range(layout, input->driver_start, input->driver_bytes, input->vram_bytes, AMDRDNA_RANGE_DRIVER);
		if (error != 0)
			return error;
	}

	/* Keeps the range memory training overwrites. */
	error = keep_range(layout, 0U, LAYOUT_TRAINING_BYTES, input->vram_bytes, AMDRDNA_RANGE_TRAINING);
	if (error != 0)
		return error;

	/* The CPU's pool ends where its window or the top range does, whichever is lower. */
	window = input->window_bytes;
	if (window > top_start)
		window = top_start;
	layout->cpu_pool_start = 0U;
	layout->cpu_pool_end = window;

	/* The GPU's own pool is what lies between the window and the top range. */
	layout->gpu_pool_start = window;
	layout->gpu_pool_end = top_start;

	/* Succeeded: the ranges are kept and the pools are bounded. */
	return 0;
}

/*
 * Names the reason a range is kept, for the log.
 */
const char *
amdrdna_range_reason_name(
	enum amdrdna_range_reason reason)
{
	/* A value outside the enumeration has no name. */
	if ((unsigned)reason >= sizeof(range_reason_names) / sizeof(range_reason_names[0]))
		return "?";

	/* Succeeded: the reason's short name. */
	return range_reason_names[reason];
}

/* Adds one kept range, widened to whole pages, after checking that it lies inside the VRAM. */
static int
keep_range(
	struct amdrdna_layout *layout,
	uint64_t start,
	uint64_t bytes,
	uint64_t vram_bytes,
	enum amdrdna_range_reason reason)
{
	struct amdrdna_range *range;
	uint64_t end;

	/* A range that starts outside the VRAM or runs past it is refused. */
	if (start >= vram_bytes)
		return EINVAL;
	if (bytes > vram_bytes - start)
		return EINVAL;

	/* The plan holds a fixed number of ranges. */
	if (layout->reserved_count == AMDRDNA_LAYOUT_RANGES)
		return EINVAL;

	/* Widens the range to whole pages, so no allocation shares a page with it. */
	end = start + bytes;
	start &= ~(LAYOUT_PAGE_BYTES - 1U);
	end = (end + LAYOUT_PAGE_BYTES - 1U) & ~(LAYOUT_PAGE_BYTES - 1U);

	/* Records the range. */
	range = &layout->reserved[layout->reserved_count];
	range->start = start;
	range->end = end;
	range->reason = reason;
	layout->reserved_count++;

	/* Succeeded: the range is kept. */
	return 0;
}
