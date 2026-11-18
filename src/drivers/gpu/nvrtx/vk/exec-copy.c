/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The copy engine's methods the executor writes into the work channel's
 * push buffers: a region of a block-linear plane copied out to a buffer's
 * rows (a video picture read back), and a rectangle of texels copied with
 * their components remapped (a depth image's depth or stencil to or from
 * a buffer or another image), on the copy object's subchannel.  Touches
 * no hardware (a host test runs it).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/nvrtx-work.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-exec.h"

/*
 * A method header: incrementing, then the count, the subchannel and the
 * method's word address.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/host/clc46f.h
 * (NVC46F_DMA_INCR_OPCODE, NVC46F_DMA_INCR_COUNT,
 * NVC46F_DMA_INCR_SUBCHANNEL, NVC46F_DMA_INCR_ADDRESS).
 */
#define COPY_METHOD_INCREMENT		(1U << 29)
#define COPY_METHOD_COUNT_SHIFT		16U
#define COPY_METHOD_SUBCHANNEL_SHIFT	13U

/*
 * The copy engine's methods: the start, the source and destination
 * addresses, the pitches, the line's bytes and count, the block-linear
 * source's block size, width, height, depth and layer (one run of five
 * words) and its origin (two words); and the start's fields: a non-pipelined
 * transfer, flushed, from block linear to pitch, of several lines, between
 * virtual addresses.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/dma-copy/clc5b5.h
 * (NVC5B5_LAUNCH_DMA and its DATA_TRANSFER_TYPE, FLUSH_ENABLE,
 * SRC_MEMORY_LAYOUT, DST_MEMORY_LAYOUT and MULTI_LINE_ENABLE fields,
 * NVC5B5_OFFSET_IN_UPPER, NVC5B5_OFFSET_OUT_UPPER, NVC5B5_PITCH_IN,
 * NVC5B5_PITCH_OUT, NVC5B5_LINE_LENGTH_IN, NVC5B5_LINE_COUNT,
 * NVC5B5_SET_SRC_BLOCK_SIZE and its WIDTH, HEIGHT, DEPTH and GOB_HEIGHT
 * fields, NVC5B5_SET_SRC_WIDTH to NVC5B5_SET_SRC_LAYER, NVC5B5_SRC_ORIGIN_X
 * and NVC5B5_SRC_ORIGIN_Y; for a rectangle also NVC5B5_SET_REMAP_COMPONENTS
 * and its DST_X to DST_W (4 bits apart), COMPONENT_SIZE, NUM_SRC_COMPONENTS
 * and NUM_DST_COMPONENTS fields, NVC5B5_SET_DST_BLOCK_SIZE to
 * NVC5B5_SET_DST_LAYER, NVC5B5_DST_ORIGIN_X and NVC5B5_DST_ORIGIN_Y, and
 * LAUNCH_DMA's SRC_MEMORY_LAYOUT and REMAP_ENABLE).
 */
#define COPY_LAUNCH			0x300U
#define COPY_REMAP_COMPONENTS		0x708U
#define COPY_DESTINATION_BLOCK		0x70cU
#define COPY_DESTINATION_ORIGIN_X	0x74cU
#define COPY_LAUNCH_SOURCE_PITCH	(1U << 7)
#define COPY_LAUNCH_REMAP		(1U << 10)
#define COPY_REMAP_SIZE_SHIFT		16U
#define COPY_REMAP_SOURCES_SHIFT	20U
#define COPY_REMAP_DESTINATIONS_SHIFT	24U
#define COPY_REMAP_COMPONENT_BITS	4U
#define COPY_OFFSET_IN_UPPER		0x400U
#define COPY_OFFSET_OUT_UPPER		0x408U
#define COPY_PITCH_IN			0x410U
#define COPY_SOURCE_BLOCK		0x728U
#define COPY_SOURCE_ORIGIN_X		0x744U
#define COPY_LAUNCH_NON_PIPELINED	0x2U
#define COPY_LAUNCH_FLUSH		(1U << 2)
#define COPY_LAUNCH_DESTINATION_PITCH	(1U << 8)
#define COPY_LAUNCH_MULTI_LINE		(1U << 9)
#define COPY_BLOCK_HEIGHT_SHIFT		4U
#define COPY_BLOCK_GOB_HEIGHT_SHIFT	12U
#define COPY_BLOCK_TWO_GOBS		1U
#define COPY_GOB_HEIGHT_FERMI_8		1U

static size_t copy_method(uint8_t *push, size_t at, uint32_t method, uint32_t count);
static size_t copy_block_side(uint8_t *push, size_t at, uint32_t block, uint32_t origin, const struct nvrtx_exec_copy_side *side,
			      uint32_t texel);

/*
 * Writes the copy of one region of a block-linear plane (blocks two GOBs
 * high) out to a buffer's rows, on the copy object's subchannel.
 *
 * Returns where the push buffer goes on; it takes at most
 * NVRTX_EXEC_PLANE_COPY_BYTES.
 *
 * Procedure referenced: Mesa main 921d028a5fff2beb4ec90451913b5fcc05fb1e6a
 * src/nouveau/vulkan/nvk_cmd_copy.c (nouveau_copy_rect: the addresses, the
 * pitches and lines, the block-linear source's surface with the row
 * stride as its width, its origin by the separate origin methods, then
 * the launch).
 */
size_t
nvrtx_exec_copy_plane(
	uint8_t *push,
	size_t at,
	const struct nvrtx_exec_plane_copy *copy)
{
	uint32_t block;

	/* The plane's first byte, then the region's first byte in the buffer. */
	at = copy_method(push, at, COPY_OFFSET_IN_UPPER, 4U);
	nvrtx_put32(push, at, (uint32_t)(copy->source >> 32));
	nvrtx_put32(push, at + 4U, (uint32_t)(copy->source & 0xffffffffU));
	nvrtx_put32(push, at + 8U, (uint32_t)(copy->destination >> 32));
	nvrtx_put32(push, at + 12U, (uint32_t)(copy->destination & 0xffffffffU));
	at += 16U;

	/* The pitches (the plane's, the buffer's), the region's row bytes and its rows. */
	at = copy_method(push, at, COPY_PITCH_IN, 4U);
	nvrtx_put32(push, at, copy->source_pitch);
	nvrtx_put32(push, at + 4U, copy->destination_pitch);
	nvrtx_put32(push, at + 8U, copy->line_bytes);
	nvrtx_put32(push, at + 12U, copy->lines);
	at += 16U;

	/* The plane: blocks one GOB wide and two high, its bytes a row (no tile width), its rows, one layer. */
	block = (COPY_BLOCK_TWO_GOBS << COPY_BLOCK_HEIGHT_SHIFT) | (COPY_GOB_HEIGHT_FERMI_8 << COPY_BLOCK_GOB_HEIGHT_SHIFT);
	at = copy_method(push, at, COPY_SOURCE_BLOCK, 5U);
	nvrtx_put32(push, at, block);
	nvrtx_put32(push, at + 4U, copy->source_pitch);
	nvrtx_put32(push, at + 8U, copy->source_rows);
	nvrtx_put32(push, at + 12U, 1U);
	nvrtx_put32(push, at + 16U, 0U);
	at += 20U;

	/* The region's origin in the plane: bytes across, rows down. */
	at = copy_method(push, at, COPY_SOURCE_ORIGIN_X, 2U);
	nvrtx_put32(push, at, copy->x_bytes);
	nvrtx_put32(push, at + 4U, copy->y);
	at += 8U;

	/* Starts the copy: not pipelined, flushed, block linear to pitch, several lines. */
	at = copy_method(push, at, COPY_LAUNCH, 1U);
	nvrtx_put32(push, at, COPY_LAUNCH_NON_PIPELINED | COPY_LAUNCH_FLUSH | COPY_LAUNCH_DESTINATION_PITCH | COPY_LAUNCH_MULTI_LINE);
	at += 4U;

	/* Succeeded: the copy is written. */
	return at;
}

/*
 * Writes the copy of a rectangle texel by texel, each texel's components
 * remapped, between two surfaces each pitch linear or block linear in
 * one-GOB blocks, on the copy object's subchannel.  With the remap the
 * engine counts across in texels.
 *
 * Returns where the push buffer goes on; it takes at most
 * NVRTX_EXEC_RECT_COPY_BYTES.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_copy.c
 * (nouveau_copy_rect: the remap first, a pitch-linear side's address moved
 * to the origin, a block-linear side's surface with the row stride in
 * texels as its width and its origin by the separate origin methods).
 */
size_t
nvrtx_exec_copy_rect(
	uint8_t *push,
	size_t at,
	const struct nvrtx_exec_rect_copy *copy)
{
	uint64_t source;
	uint64_t destination;
	uint32_t source_texel;
	uint32_t destination_texel;
	uint32_t remap;
	uint32_t launch;
	uint32_t index;

	/* The remap: each destination component's source, the component's size, the components of each side. */
	remap = 0U;
	for (index = 0U; index < 4U; index++)
		remap |= copy->remap[index] << (index * COPY_REMAP_COMPONENT_BITS);
	remap |= (copy->component_bytes - 1U) << COPY_REMAP_SIZE_SHIFT;
	remap |= (copy->source_components - 1U) << COPY_REMAP_SOURCES_SHIFT;
	remap |= (copy->destination_components - 1U) << COPY_REMAP_DESTINATIONS_SHIFT;
	at = copy_method(push, at, COPY_REMAP_COMPONENTS, 1U);
	nvrtx_put32(push, at, remap);
	at += 4U;

	/* A pitch-linear side starts at the rectangle's first texel. */
	source_texel = copy->component_bytes * copy->source_components;
	destination_texel = copy->component_bytes * copy->destination_components;
	source = copy->source.address;
	if (copy->source.block_linear == 0U)
		source += (uint64_t)copy->source.y * copy->source.pitch + (uint64_t)copy->source.x * source_texel;
	destination = copy->destination.address;
	if (copy->destination.block_linear == 0U)
		destination += (uint64_t)copy->destination.y * copy->destination.pitch + (uint64_t)copy->destination.x * destination_texel;

	/* The two first bytes, the pitches, the rectangle's texels across and its rows. */
	at = copy_method(push, at, COPY_OFFSET_IN_UPPER, 8U);
	nvrtx_put32(push, at, (uint32_t)(source >> 32));
	nvrtx_put32(push, at + 4U, (uint32_t)(source & 0xffffffffU));
	nvrtx_put32(push, at + 8U, (uint32_t)(destination >> 32));
	nvrtx_put32(push, at + 12U, (uint32_t)(destination & 0xffffffffU));
	nvrtx_put32(push, at + 16U, copy->source.pitch);
	nvrtx_put32(push, at + 20U, copy->destination.pitch);
	nvrtx_put32(push, at + 24U, copy->width);
	nvrtx_put32(push, at + 28U, copy->height);
	at += 32U;

	/* A block-linear side's surface and origin, and the launch's layouts. */
	launch = COPY_LAUNCH_NON_PIPELINED | COPY_LAUNCH_FLUSH | COPY_LAUNCH_MULTI_LINE | COPY_LAUNCH_REMAP;
	if (copy->source.block_linear != 0U)
		at = copy_block_side(push, at, COPY_SOURCE_BLOCK, COPY_SOURCE_ORIGIN_X, &copy->source, source_texel);
	else
		launch |= COPY_LAUNCH_SOURCE_PITCH;
	if (copy->destination.block_linear != 0U)
		at = copy_block_side(push, at, COPY_DESTINATION_BLOCK, COPY_DESTINATION_ORIGIN_X, &copy->destination, destination_texel);
	else
		launch |= COPY_LAUNCH_DESTINATION_PITCH;

	/* Starts the copy: not pipelined, flushed, several lines, remapped. */
	at = copy_method(push, at, COPY_LAUNCH, 1U);
	nvrtx_put32(push, at, launch);
	at += 4U;

	/* Succeeded: the copy is written. */
	return at;
}

/*
 * Writes one block-linear side of a rectangle copy: blocks one GOB each
 * way, its row pitch in texels as its width (the engine has no tile
 * width), its rows, one slice and layer 0, then the origin in texels and
 * rows.
 */
static size_t
copy_block_side(
	uint8_t *push,
	size_t at,
	uint32_t block,
	uint32_t origin,
	const struct nvrtx_exec_copy_side *side,
	uint32_t texel)
{
	/* The surface. */
	at = copy_method(push, at, block, 5U);
	nvrtx_put32(push, at, COPY_GOB_HEIGHT_FERMI_8 << COPY_BLOCK_GOB_HEIGHT_SHIFT);
	nvrtx_put32(push, at + 4U, side->pitch / texel);
	nvrtx_put32(push, at + 8U, side->rows);
	nvrtx_put32(push, at + 12U, 1U);
	nvrtx_put32(push, at + 16U, 0U);
	at += 20U;

	/* The origin. */
	at = copy_method(push, at, origin, 2U);
	nvrtx_put32(push, at, side->x);
	nvrtx_put32(push, at + 4U, side->y);
	at += 8U;

	/* Succeeded: the side is written. */
	return at;
}

/* Writes an incrementing method header on the copy object's subchannel and returns where its data goes. */
static size_t
copy_method(
	uint8_t *push,
	size_t at,
	uint32_t method,
	uint32_t count)
{
	uint32_t header;

	/* The header's fields. */
	header = COPY_METHOD_INCREMENT |
	    (count << COPY_METHOD_COUNT_SHIFT) |
	    (NVRTX_WORK_SUBCHANNEL_COPY << COPY_METHOD_SUBCHANNEL_SHIFT) |
	    (method >> 2);
	nvrtx_put32(push, at, header);

	/* Succeeded: the data follows the header. */
	return at + 4U;
}
