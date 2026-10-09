/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The byte images of V3D 4.2's initial shader-free, one-pixel job.
 *
 * Packet sizes, field positions and opcodes come from the fixed Mesa
 * v3d_packet.xml recorded by WS141.  The serializer writes explicit bytes
 * so unaligned command addresses and host endianness do not affect the
 * images.  It neither allocates memory nor touches hardware registers.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* The exclusive end of V3D's GPU virtual address space. */
#define CL_VIRTUAL_END ((uint64_t)1 << 32)

/* The 64-byte alignment of a tile-list-set base encoded in its packet. */
#define CL_POOL_ALIGNMENT 64U

/* The address fields within the fixed render stream, measured in bytes. */
#define CL_RENDER_POOL_ADDRESS 30U
#define CL_RENDER_TILE_START 44U
#define CL_RENDER_TILE_END 48U

/*
 * One reserved GPU interval used while validating the complete job.
 * The interval is half-open and held only for the preparation call.
 */
struct cl_span {
	uint64_t first;
	uint64_t end;
};

/*
 * The immutable bin stream for one layer and a one-pixel 32-bpp target.
 * Dimensions and target count use minus-one fields, hence their zero bytes.
 * No semaphore is emitted: the owner submits render after bin completion.
 */
static const uint8_t noop_bin_image[BCM2711_V3D_NOOP_BIN_BYTES] = {
	/* Declares one layer before the binning configuration. */
	119, 0,

	/* Configures 1x1, one target, no MSAA, and 64-byte tile-list blocks. */
	120, 0, 0, 0, 0, 0, 0, 0, 0,

	/* Discards vertex cache state, opens binning, and closes the empty frame. */
	19, 6, 4
};

/*
 * The immutable render stream, with three address fields filled per job.
 * It describes one tile without color stores or shaders.  The depth clear
 * value is the IEEE-754 encoding of 1.0, written without CPU floating point.
 */
static const uint8_t noop_render_image[BCM2711_V3D_NOOP_RENDER_BYTES] = {
	/* Sets 1x1 and disables early Z for the single 32-bpp target. */
	121, 0, 1, 0, 1, 0, 0x40, 0, 0,

	/* Selects an 8-bit-component target with no clamping. */
	121, 0x81, 0, 0, 0, 0, 0, 0, 0,

	/* Defines the unused depth/stencil clear state. */
	121, 2, 0, 0, 0, 0x80, 0x3f, 0, 0,

	/* Uses auto-chained tile lists with an initial 64-byte block. */
	126, 4,

	/* Names tile-list set zero; its pool address is patched below. */
	123, 0, 0, 0, 0,

	/* Covers one bin list, one supertile and one tile, without multicore mode. */
	122, 0, 0, 1, 1, 1, 0x10, 0, 0,

	/* Names the generic tile sub-list, with start and end patched below. */
	20, 0, 0, 0, 0, 0, 0, 0, 0,

	/* Executes supertile zero and closes rendering. */
	23, 0, 0, 13
};

/*
 * The immutable generic tile sub-list shared by the sole tile of this job.
 * The store packet selects NONE, so the frame has no output image buffer.
 * The branch consumes the bin-produced list before returning to render.
 */
static const uint8_t noop_tile_image[BCM2711_V3D_NOOP_TILE_BYTES] = {
	/* Takes tile coordinates from render and finishes the empty load phase. */
	125, 26,

	/* Consumes the implicit bin list from tile-list set zero. */
	21, 0,

	/* Completes the tile without storing any color or depth buffer. */
	29, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

	/* Ends the tile and returns to the enclosing render stream. */
	27, 18
};

static int validate_job(const struct bcm2711_v3d_noop *job);
static int validate_buffer(const struct bcm2711_v3d_cl *buffer, uint32_t required);
static void copy_image(uint8_t *destination, const uint8_t *source, uint32_t bytes);
static void put_address(uint8_t *destination, uint32_t address);

/*
 * Builds a complete 1x1 noop job into already allocated command buffers.
 *
 * All CPU buffers must be separate.  Their capacities describe complete
 * reserved GPU intervals, which cannot overlap each other or the tile pool.
 * Returns ENOSPC for short command buffers, and EINVAL for invalid storage,
 * GPU addresses or reservations.  A refusal changes no image bytes and
 * leaves all used lengths zero.  Success does not submit or synchronize a job.
 * The owner serializes preparation against submission and never rebuilds a
 * job while either hardware queue still references its storage.
 */
int
bcm2711_v3d_noop_prepare(
	struct bcm2711_v3d_noop *job)
{
	int error;
	uint32_t tile_end;

	/* Prevents a failed rebuild from leaving old lengths ready for submission. */
	job->bin.used = 0;
	job->render.used = 0;
	job->tile.used = 0;

	/* Validates every reservation before touching any caller-owned image. */
	error = validate_job(job);
	if (error != 0)
		return error;

	/* Installs all fixed packets before filling their job-specific addresses. */
	copy_image(job->bin.bytes, noop_bin_image, BCM2711_V3D_NOOP_BIN_BYTES);
	copy_image(job->render.bytes, noop_render_image, BCM2711_V3D_NOOP_RENDER_BYTES);
	copy_image(job->tile.bytes, noop_tile_image, BCM2711_V3D_NOOP_TILE_BYTES);

	/* Relocates the pool and complete generic sub-list in the render image. */
	tile_end = job->tile.address + BCM2711_V3D_NOOP_TILE_BYTES;
	put_address(job->render.bytes + CL_RENDER_POOL_ADDRESS, job->pool_address);
	put_address(job->render.bytes + CL_RENDER_TILE_START, job->tile.address);
	put_address(job->render.bytes + CL_RENDER_TILE_END, tile_end);

	/* Publishes each stream only after the complete set of images is ready. */
	job->bin.used = BCM2711_V3D_NOOP_BIN_BYTES;
	job->render.used = BCM2711_V3D_NOOP_RENDER_BYTES;
	job->tile.used = BCM2711_V3D_NOOP_TILE_BYTES;

	/* Succeeded: the owner can clean these images before bin then render. */
	return 0;
}

/* Checks all command buffers and pool reservations as one indivisible job. */
static int
validate_job(
	const struct bcm2711_v3d_noop *job)
{
	struct cl_span spans[4];
	int error;
	unsigned first;
	unsigned second;

	/* Bounds each command stream before computing its reserved interval. */
	error = validate_buffer(&job->bin, BCM2711_V3D_NOOP_BIN_BYTES);
	if (error != 0)
		return error;

	/* Requires a complete render image, including both indirect addresses. */
	error = validate_buffer(&job->render, BCM2711_V3D_NOOP_RENDER_BYTES);
	if (error != 0)
		return error;

	/* Requires a complete sub-list and a representable exclusive end. */
	error = validate_buffer(&job->tile, BCM2711_V3D_NOOP_TILE_BYTES);
	if (error != 0)
		return error;

	/* Preserves the GPU's reserved zero page for the tile pool as well. */
	if (job->pool_address < BCM2711_V3D_PAGE_BYTES)
		return EINVAL;

	/* Refuses low address bits that conflict with the tile-list-set field. */
	if ((job->pool_address & (CL_POOL_ALIGNMENT - 1U)) != 0)
		return EINVAL;

	/* Reserves enough space for one tile and the initial overflow headroom. */
	if (job->pool_bytes < BCM2711_V3D_NOOP_POOL_BYTES)
		return EINVAL;

	/* Bounds the pool without overflowing a 32-bit GPU endpoint. */
	if (job->pool_bytes > CL_VIRTUAL_END - job->pool_address)
		return EINVAL;

	/* Includes unused buffer capacity in the reservations, not just packets. */
	spans[0].first = job->bin.address;
	spans[0].end = spans[0].first + job->bin.capacity;
	spans[1].first = job->render.address;
	spans[1].end = spans[1].first + job->render.capacity;
	spans[2].first = job->tile.address;
	spans[2].end = spans[2].first + job->tile.capacity;
	spans[3].first = job->pool_address;
	spans[3].end = spans[3].first + job->pool_bytes;

	/* Refuses every overlap before the GPU could overwrite a live command. */
	for (first = 0; first < 4; first++) {
		/* Compares each interval once with the following reservations. */
		for (second = first + 1; second < 4; second++) {
			/* Disjoint intervals may meet at their exclusive endpoints. */
			if (spans[first].end <= spans[second].first)
				continue;

			/* The opposite ordering also leaves both intervals untouched. */
			if (spans[second].end <= spans[first].first)
				continue;

			/* Any remaining pair would share GPU storage. */
			return EINVAL;
		}
	}

	/* Succeeded: the complete job owns disjoint representable GPU intervals. */
	return 0;
}

/* Bounds one command buffer without requiring aligned packet addresses. */
static int
validate_buffer(
	const struct bcm2711_v3d_cl *buffer,
	uint32_t required)
{
	/* Requires CPU storage for the generated bytes. */
	if (buffer->bytes == NULL)
		return EINVAL;

	/* Refuses short storage before any stream is modified. */
	if (buffer->capacity < required)
		return ENOSPC;

	/* Keeps all command bytes outside the reserved zero GPU page. */
	if (buffer->address < BCM2711_V3D_PAGE_BYTES)
		return EINVAL;

	/* Keeps the end address stored in packets representable in 32 bits. */
	if (required > UINT32_MAX - buffer->address)
		return EINVAL;

	/* Requires the entire reservation to fit even when its tail is unused. */
	if (buffer->capacity > CL_VIRTUAL_END - buffer->address)
		return EINVAL;

	/* Succeeded: the caller owns enough GPU-addressable storage. */
	return 0;
}

/* Installs a fixed packet image without touching the unused capacity tail. */
static void
copy_image(
	uint8_t *destination,
	const uint8_t *source,
	uint32_t bytes)
{
	uint32_t index;

	/* Copies exactly the complete image, leaving capacity canaries unchanged. */
	for (index = 0; index < bytes; index++)
		destination[index] = source[index];
}

/* Writes a GPU byte address in the packet's little-endian representation. */
static void
put_address(
	uint8_t *destination,
	uint32_t address)
{
	/* Avoids an unaligned native store and its dependence on CPU endianness. */
	destination[0] = (uint8_t)address;
	destination[1] = (uint8_t)(address >> 8);
	destination[2] = (uint8_t)(address >> 16);
	destination[3] = (uint8_t)(address >> 24);
}
