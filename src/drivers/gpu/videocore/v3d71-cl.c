/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The byte images of V3D 7.1's shader-free self-test jobs and the words of
 * its compute shaders.
 *
 * The images are the Pi 4's 4.2 noop and clear jobs with the packets 7.1
 * changed: the binning and rendering configurations name the tile size, the
 * single render target is described by a "render target part 1" packet that
 * also carries its clear colour and its place in the tile buffer, the depth
 * clear values moved to sub-packet 1, and the clear of the first dummy tile
 * has no fields.  The serializer writes explicit bytes, so unaligned
 * command addresses and host endianness do not affect the images.  It
 * neither allocates memory nor touches hardware registers.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/cle/v3d_packet.xml (the
 * packets with min_ver 71 or without a version bound),
 * src/broadcom/vulkan/v3dvx_queue.c lines 30-131 (the 7.1 noop job) and
 * v3dvx_meta_common.c lines 140-245 (the 7.1 clear and dummy tiles),
 * src/broadcom/common/v3d_util.c lines 269-279 (the render target stride).
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/v3d71-private.h"

/* The exclusive end of V3D's GPU virtual address space. */
#define CL_VIRTUAL_END ((uint64_t)1 << 32)

/* The 64-byte alignment of a tile-list-set base encoded in its packet. */
#define CL_POOL_ALIGNMENT 64U

/* The address fields within the fixed render stream, measured in bytes. */
#define CL_RENDER_POOL_ADDRESS 30U
#define CL_RENDER_TILE_START 44U
#define CL_RENDER_TILE_END 48U

/* The address fields of the clear's render stream, after its 40 bytes of dummy tiles. */
#define CL_CLEAR_POOL_ADDRESS 30U
#define CL_CLEAR_TILE_START 84U
#define CL_CLEAR_TILE_END 88U

/* Where the render target's clear colour sits in its part 1 packet of the render stream. */
#define CL_CLEAR_COLOUR 14U

/*
 * The render target's stride field of a 64-pixel-wide 32-bit tile: 64 words
 * per row cover two rows in 128-bit units, 32, written minus one.
 */
#define CL_CLEAR_STRIDE_FIELD 0x7cU

/*
 * One reserved GPU interval used while validating the complete job.
 * The interval is half-open and held only for the preparation call.
 */
struct cl_span {
	uint64_t first;
	uint64_t end;
};

/*
 * The immutable bin stream for one layer and a one-pixel image in 64x64
 * tiles.  Dimensions use minus-one fields, hence their zero bytes.  No
 * semaphore is emitted: the owner submits render after bin completion.
 */
static const uint8_t noop_bin_image[BCM2712_V3D_NOOP_BIN_BYTES] = {
	/* Declares one layer before the binning configuration. */
	119, 0,

	/* Configures 1x1 in 64x64 tiles (log2 3 and 3) with 64-byte tile-list blocks. */
	120, 0, 0x1b, 0, 0, 0, 0, 0, 0,

	/* Discards vertex cache state, opens binning, and closes the empty frame. */
	19, 6, 4
};

/*
 * The immutable render stream, with three address fields filled per job.
 * It describes one tile without colour stores or shaders.  The depth clear
 * value is the IEEE-754 encoding of 1.0, written without CPU floating point.
 */
static const uint8_t noop_render_image[BCM2712_V3D_NOOP_RENDER_BYTES] = {
	/* Sets 1x1, one target, early Z disabled and 64x64 tiles. */
	121, 0, 1, 0, 1, 0, 0x40, 0xb0, 0x01,

	/* Declares target 0 as 32-bit, 8-bit unclamped components, unused (stride 1). */
	121, 2, 0, 0, 0x40, 0, 0, 0, 0,

	/* Defines the unused depth/stencil clear state. */
	121, 1, 0, 0, 0, 0x80, 0x3f, 0, 0,

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
static const uint8_t noop_tile_image[BCM2712_V3D_NOOP_TILE_BYTES] = {
	/* Takes tile coordinates from render and finishes the empty load phase. */
	125, 26,

	/* Consumes the implicit bin list from tile-list set zero. */
	21, 0,

	/* Completes the tile without storing any colour or depth buffer. */
	29, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

	/* Ends the tile and returns to the enclosing render stream. */
	27, 18
};

/*
 * Two dummy tiles establish the clear state and the initial-tile workaround.
 * The first clears the render targets; the second stores NONE and then
 * drains VCD before the real generic tile list runs.  No shader runs.
 */
static const uint8_t clear_dummy_image[40] = {
	124, 0, 0, 0, 26,
	29, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	25, 27,
	124, 0, 0, 0, 26,
	29, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	27, 19
};

/*
 * The compute shader that does nothing: its first word ends the thread,
 * and the two words after it are the end's delay slots.  The program has
 * no other thread switch, so it runs in a single segment.
 *
 * The words were packed with Mesa 25.3.6's 7.1 packer (MIT,
 * src/broadcom/qpu/qpu_pack.c); plan/ws279/tests/v3d71-shader-host-test.c
 * checks them against it.
 */
const uint64_t bcm2712_v3d_empty_shader[BCM2712_V3D_EMPTY_SHADER_WORDS] = {
	/* nop ; thrsw: the thread end. */
	0x38203186bb03f000ULL,

	/* nop, nop: the end's delay slots. */
	0x38003186bb03f000ULL,
	0x38003186bb03f000ULL
};

/*
 * The compute shader that stores one word through the TMU.
 *
 * Its uniforms are the address and the value; every lane writes the value
 * to the address, waits for the write, and ends.
 */
const uint64_t bcm2712_v3d_store_shader[BCM2712_V3D_STORE_SHADER_WORDS] = {
	/* nop ; ldunif: the address arrives in rf0. */
	0x38403186bb03f000ULL,

	/* mov rf1, rf0 ; ldunif: keeps the address, the value arrives in rf0. */
	0x38402181f903f003ULL,

	/* mov tmud, rf0: the data of the write. */
	0x3800318bf903f003ULL,

	/* mov tmua, rf1: the address, which starts the write. */
	0x3800318cf903f043ULL,

	/* tmuwt: waits for the write to finish. */
	0x38003186bb03f00fULL,

	/* nop ; thrsw: the thread end. */
	0x38203186bb03f000ULL,

	/* nop, nop: the end's delay slots. */
	0x38003186bb03f000ULL,
	0x38003186bb03f000ULL
};

static int validate_job(const struct bcm2711_v3d_noop *job, uint32_t render_bytes);
static int validate_buffer(const struct bcm2711_v3d_cl *buffer, uint32_t required);
static int validate_clear(const struct bcm2711_v3d_clear *job);
static void copy_image(uint8_t *destination, const uint8_t *source, uint32_t bytes);
static void put_word(uint8_t *destination, uint32_t address);

/*
 * Builds a complete 1x1 noop job into already allocated command buffers.
 *
 * All CPU buffers must be separate.  Their capacities describe complete
 * reserved GPU intervals, which cannot overlap each other or the tile pool.
 * Returns ENOSPC for short command buffers, and EINVAL for invalid storage,
 * GPU addresses or reservations.  A refusal changes no image bytes and
 * leaves all used lengths zero.  Success does not submit or synchronize a job.
 */
int
bcm2712_v3d_noop_prepare(
	struct bcm2711_v3d_noop *job)
{
	uint32_t tile_end;
	int error;

	/* Prevents a failed rebuild from leaving old lengths ready for submission. */
	job->bin.used = 0;
	job->render.used = 0;
	job->tile.used = 0;

	/* Validates every reservation before touching any caller-owned image. */
	error = validate_job(job, BCM2712_V3D_NOOP_RENDER_BYTES);
	if (error != 0)
		return error;

	/* Installs all fixed packets before filling their job-specific addresses. */
	copy_image(job->bin.bytes, noop_bin_image, BCM2712_V3D_NOOP_BIN_BYTES);
	copy_image(job->render.bytes, noop_render_image, BCM2712_V3D_NOOP_RENDER_BYTES);
	copy_image(job->tile.bytes, noop_tile_image, BCM2712_V3D_NOOP_TILE_BYTES);

	/* Relocates the pool and complete generic sub-list in the render image. */
	tile_end = job->tile.address + BCM2712_V3D_NOOP_TILE_BYTES;
	put_word(job->render.bytes + CL_RENDER_POOL_ADDRESS, job->pool_address);
	put_word(job->render.bytes + CL_RENDER_TILE_START, job->tile.address);
	put_word(job->render.bytes + CL_RENDER_TILE_END, tile_end);

	/* Publishes each stream only after the complete set of images is ready. */
	job->bin.used = BCM2712_V3D_NOOP_BIN_BYTES;
	job->render.used = BCM2712_V3D_NOOP_RENDER_BYTES;
	job->tile.used = BCM2712_V3D_NOOP_TILE_BYTES;

	/* Succeeded: the owner can clean these images before bin then render. */
	return 0;
}

/*
 * Builds one shader-free RGBA8 clear/store tile with the dummy-tile sequence.
 *
 * Dimensions are at most 64x64 (one 64x64 tile), one layer and target, no
 * MSAA or double buffering.  All reservations and capacities are checked
 * before writing any command byte; refused jobs preserve previous images and
 * publish zero used lengths.
 */
int
bcm2712_v3d_clear_prepare(
	struct bcm2711_v3d_clear *job)
{
	struct bcm2711_v3d_noop *lists;
	uint8_t *render;
	uint8_t *tile;
	uint64_t store;
	uint32_t index;
	int error;

	/* A refused rebuild never leaves older packets eligible for another launch. */
	lists = &job->lists;
	lists->bin.used = 0;
	lists->render.used = 0;
	lists->tile.used = 0;
	error = validate_clear(job);
	if (error != 0)
		return error;

	/* Binning describes the exact one-tile geometry while keeping noop's shader-free prolog. */
	copy_image(lists->bin.bytes, noop_bin_image, BCM2712_V3D_NOOP_BIN_BYTES);
	lists->bin.bytes[7] = (uint8_t)(job->width - 1U);
	lists->bin.bytes[9] = (uint8_t)(job->height - 1U);

	/* The rendering configuration names the image size the target is stored at. */
	render = lists->render.bytes;
	copy_image(render, noop_render_image, 43);
	render[2] = (uint8_t)job->width;
	render[4] = (uint8_t)job->height;

	/* Target 0 takes its place in the tile buffer: base 0, a 64-pixel row's stride, and the colour. */
	render[12] = CL_CLEAR_STRIDE_FIELD;
	put_word(render + CL_CLEAR_COLOUR, job->color);

	/* The dummy tiles clear the targets before the real tile list, then the fixed end follows. */
	copy_image(render + 43, clear_dummy_image, sizeof(clear_dummy_image));
	copy_image(render + 83, noop_render_image + 43, 13);
	put_word(render + CL_CLEAR_POOL_ADDRESS, lists->pool_address);
	put_word(render + CL_CLEAR_TILE_START, lists->tile.address);
	put_word(render + CL_CLEAR_TILE_END, lists->tile.address + BCM2712_V3D_NOOP_TILE_BYTES);

	/* The target 0 raster store uses RGBA8 format 27 and the exact byte stride, with no R/B swap. */
	tile = lists->tile.bytes;
	copy_image(tile, noop_tile_image, BCM2712_V3D_NOOP_TILE_BYTES);
	store = (uint64_t)27U << 12;
	store |= (uint64_t)job->stride << 28;

	/* Writes the store's configuration bytes one at a time, low byte first. */
	for (index = 0; index < 8; index++)
		tile[5 + index] = (uint8_t)(store >> (index * 8U));

	/* The store's address follows its configuration. */
	put_word(tile + 13, job->target_address);

	/* Publishing lengths last exposes only a complete group of relocated packet images. */
	lists->bin.used = BCM2712_V3D_NOOP_BIN_BYTES;
	lists->render.used = BCM2712_V3D_CLEAR_RENDER_BYTES;
	lists->tile.used = BCM2712_V3D_NOOP_TILE_BYTES;

	/* Succeeded: the owner can clean inputs, submit bin/render and observe the clear output. */
	return 0;
}

/* Checks all command buffers and pool reservations as one indivisible job. */
static int
validate_job(
	const struct bcm2711_v3d_noop *job,
	uint32_t render_bytes)
{
	struct cl_span spans[4];
	unsigned first;
	unsigned second;
	int error;

	/* Bounds each command stream before computing its reserved interval. */
	error = validate_buffer(&job->bin, BCM2712_V3D_NOOP_BIN_BYTES);
	if (error != 0)
		return error;

	/* Requires a complete render image, including both indirect addresses. */
	error = validate_buffer(&job->render, render_bytes);
	if (error != 0)
		return error;

	/* Requires a complete sub-list and a representable exclusive end. */
	error = validate_buffer(&job->tile, BCM2712_V3D_NOOP_TILE_BYTES);
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

/* Bounds a one-tile raster and preserves all existing CL/pool reservations. */
static int
validate_clear(
	const struct bcm2711_v3d_clear *job)
{
	const struct bcm2711_v3d_noop *lists;
	struct cl_span spans[4];
	uint64_t target_end;
	uint32_t index;
	int error;

	/* Full clear render capacity is checked before any template is copied. */
	lists = &job->lists;
	error = validate_job(lists, BCM2712_V3D_CLEAR_RENDER_BYTES);
	if (error != 0)
		return error;

	/* One 32-bpp target without MSAA occupies at most one 64x64 tile. */
	if (job->width == 0 || job->width > 64U)
		return EINVAL;
	if (job->height == 0 || job->height > 64U)
		return EINVAL;
	if (job->stride < job->width * 4U || job->stride > 0xfffffU)
		return EINVAL;
	if ((job->stride & 3U) != 0 || (job->target_address & 3U) != 0)
		return EINVAL;
	if (job->target_address < BCM2711_V3D_PAGE_BYTES)
		return EINVAL;
	if (job->target_bytes < (uint64_t)job->stride * job->height)
		return EINVAL;
	if (job->target_bytes > CL_VIRTUAL_END - job->target_address)
		return EINVAL;

	/* The complete target reservation cannot overlap commands or PTB-produced tile lists. */
	spans[0].first = lists->bin.address;
	spans[0].end = spans[0].first + lists->bin.capacity;
	spans[1].first = lists->render.address;
	spans[1].end = spans[1].first + lists->render.capacity;
	spans[2].first = lists->tile.address;
	spans[2].end = spans[2].first + lists->tile.capacity;
	spans[3].first = lists->pool_address;
	spans[3].end = spans[3].first + lists->pool_bytes;
	target_end = (uint64_t)job->target_address + job->target_bytes;

	/* Refuses a target alias before exposing any partially regenerated stream. */
	for (index = 0; index < 4; index++) {
		/* A target wholly before or wholly after the interval leaves it alone. */
		if (target_end <= spans[index].first)
			continue;
		if (spans[index].end <= job->target_address)
			continue;

		/* The target would share storage with a command or the pool. */
		return EINVAL;
	}

	/* Succeeded: the output target and all command reservations are disjoint and bounded. */
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

/* Writes one 32-bit packet field in its little-endian representation. */
static void
put_word(
	uint8_t *destination,
	uint32_t address)
{
	/* Avoids an unaligned native store and its dependence on CPU endianness. */
	destination[0] = (uint8_t)address;
	destination[1] = (uint8_t)(address >> 8);
	destination[2] = (uint8_t)(address >> 16);
	destination[3] = (uint8_t)(address >> 24);
}
