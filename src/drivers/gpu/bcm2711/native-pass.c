/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Independent 4.2 pass serialization uses fixed packet fields and native workarounds; allocation, cache management and launch belong to the job owner. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/native-pass.h"

/* One complete native pass remains inside the enclosing job's aggregate staging limit. */
#define NATIVE_PASS_MAX_BYTES (256U * 1024U * 1024U)

/* A temporary half-open interval exists only while all independent native owners are checked before list publication. */
struct native_pass_span {
	uint64_t first;
	uint64_t end;
};

static int prepare_span(uint32_t address, uint32_t bytes, uint32_t alignment, struct native_pass_span *span);
static int validate_images(const struct bcm2711_native_pass *state, const struct bcm2711_native_pass_sizes *sizes, struct bcm2711_native_pass_image *bin, struct bcm2711_native_pass_image *render, struct bcm2711_native_pass_image *tile);
static void write_word(uint8_t *bytes, uint32_t word);
static void write_half(uint8_t *bytes, uint32_t half);
static void write_raster(uint8_t *bytes, uint8_t opcode, const struct bcm2711_native_pass *state);
static void encode_render(const struct bcm2711_native_pass *state, const struct bcm2711_native_pass_image *tile, const struct bcm2711_native_pass_sizes *sizes, uint8_t *bytes);
static void encode_tile(const struct bcm2711_native_pass *state, uint8_t *bytes);

/*
 * Measures exact list lengths and complete binning storage for one bounded pass.
 *
 * Native tiles are 64x64 pixels; power-of-two square supertiles keep the
 * complete framebuffer inside the 256-group hardware limit.  Every selected
 * supertile executes the generic sub-list for each of its in-bounds tiles.  A load is required when pixel preservation outside a
 * partial render area needs the old framebuffer; the owner handles partial
 * clear before launch instead of clearing an entire native tile.
 */
int
bcm2711_native_pass_measure(
	const struct bcm2711_native_pass *state,
	uint32_t draws,
	struct bcm2711_native_pass_sizes *sizes)
{
	struct bcm2711_native_pass_sizes measured;
	uint32_t columns;
	uint32_t rows;
	uint32_t tiles;
	uint32_t group;
	uint32_t selected_columns;
	uint32_t selected_rows;

	/* Missing numerical input cannot supply any complete measurement. */
	if (state == NULL || sizes == NULL)
		return EINVAL;

	/* One single-colour, 32-bpp framebuffer uses the implemented bounded raster geometry. */
	if (state->width == 0 ||
	    state->width > 4096U ||
	    state->height == 0 ||
	    state->height > 4096U)
		return EINVAL;

	/* The byte pitch must cover every visible word and fit the native twenty-bit field. */
	if (state->pitch < state->width * 4U ||
	    state->pitch > 0xfffffU ||
	    (state->pitch & 63U) != 0)
		return EINVAL;

	/* Load and store choices are Boolean, independently of whether any draw consumes the target. */
	if (state->load > 1 || state->store > 1)
		return EINVAL;

	/* The complete selected tile rectangle must lie inside the framebuffer's binning grid. */
	columns = (state->width + 63U) / 64U;
	rows = (state->height + 63U) / 64U;
	if (state->first_x > state->last_x ||
	    state->last_x >= columns ||
	    state->first_y > state->last_y ||
	    state->last_y >= rows)
		return EINVAL;

	/* The aggregate staging bound keeps list arithmetic representable before allocation. */
	if (draws > (NATIVE_PASS_MAX_BYTES - 14U) / BCM2711_NATIVE_BIN_BYTES)
		return E2BIG;

	/* Every binning tile has a sixty-four-byte first block and two hundred fifty-six bytes of tile state. */
	tiles = columns * rows;
	measured.pool = (tiles * 64U + 4095U) & ~4095U;
	measured.pool += 8192U;
	measured.pool += 512U * 1024U;
	measured.state = tiles * 256U;

	/* One generic list includes explicit triangle/instance state and both native store and clear operations. */
	measured.bin = 14U + draws * BCM2711_NATIVE_BIN_BYTES;
	measured.tile = 28U;
	if (state->load != 0)
		measured.tile += 13U;

	/* Square power-of-two groups cover all binning tiles within the native maximum of 256 supertiles. */
	group = 1;
	measured.columns = columns;
	measured.rows = rows;
	while (measured.columns * measured.rows > 256U) {
		group *= 2U;
		measured.columns = (columns + group - 1U) / group;
		measured.rows = (rows + group - 1U) / group;
	}

	/* Selected groups may include neighbouring tiles, whose samples the owner must preserve with a load when necessary. */
	measured.supertile_width = group;
	measured.supertile_height = group;
	selected_columns = state->last_x / group - state->first_x / group + 1U;
	selected_rows = state->last_y / group - state->first_y / group + 1U;
	measured.render = 103U + selected_columns * selected_rows * 3U;

	/* Succeeded: no partial measurement escapes a refused pass geometry. */
	*sizes = measured;
	return 0;
}

/*
 * Builds complete bin/render/tile lists while preserving preassembled draw sequences.
 *
 * Draws already occupy the bin image immediately after its thirteen-byte
 * prefix.  All complete GPU and CPU intervals are checked before any list
 * byte changes.  Active counts remain zero on failure.  The enclosing owner
 * cleans the images and retains output, tile state, pool and every draw input
 * through completion or a global reset proving native DMA has stopped.
 */
int
bcm2711_native_pass_encode(
	const struct bcm2711_native_pass *state,
	uint32_t draws,
	struct bcm2711_native_pass_image *bin,
	struct bcm2711_native_pass_image *render,
	struct bcm2711_native_pass_image *tile)
{
	struct bcm2711_native_pass_sizes sizes;
	int error;

	/* Missing output descriptors cannot publish any active list. */
	if (bin == NULL ||
	    render == NULL ||
	    tile == NULL)
		return EINVAL;

	/* A failure leaves every list inactive even if the caller supplied old counts. */
	bin->used = 0;
	render->used = 0;
	tile->used = 0;

	/* Exact geometry and sizes are established before touching caller-owned list bytes. */
	error = bcm2711_native_pass_measure(state, draws, &sizes);
	if (error != 0)
		return error;

	/* Complete storage intervals must be independent, large enough and exactly encodable. */
	error = validate_images(state, &sizes, bin, render, tile);
	if (error != 0)
		return error;

	/* One layer and one target begin binning; preassembled numerical draw sequences stay untouched. */
	kern_memset(bin->bytes, 0, BCM2711_NATIVE_PASS_BIN_PREFIX);
	bin->bytes[0] = 119;
	bin->bytes[2] = 120;
	write_half(bin->bytes + 7, state->width - 1U);
	write_half(bin->bytes + 9, state->height - 1U);
	bin->bytes[11] = 19;
	bin->bytes[12] = 6;
	bin->bytes[sizes.bin - 1U] = 4;

	/* Real rendering uses complete initialized packet images with no inherited reserved fields. */
	kern_memset(render->bytes, 0, sizes.render);
	kern_memset(tile->bytes, 0, sizes.tile);
	encode_render(state, tile, &sizes, render->bytes);
	encode_tile(state, tile->bytes);

	/* Succeeded: all three lists publish only after the complete no-failure construction. */
	bin->used = sizes.bin;
	render->used = sizes.render;
	tile->used = sizes.tile;
	return 0;
}

/* Establishes one nonempty, aligned complete native interval without wrapping the thirty-two-bit VA space. */
static int
prepare_span(
	uint32_t address,
	uint32_t bytes,
	uint32_t alignment,
	struct native_pass_span *span)
{
	uint64_t end;

	/* Reserved page zero and incomplete or misaligned owners cannot be encoded as live native storage. */
	if (address < 4096U ||
	    bytes == 0 ||
	    (address & (alignment - 1U)) != 0)
		return EINVAL;

	/* The exclusive end may equal the address-space limit only for storage without an encoded end pointer. */
	end = (uint64_t)address + bytes;
	if (end > ((uint64_t)1 << 32))
		return EINVAL;

	/* Succeeded: the temporary span covers the complete independently retained allocation. */
	span->first = address;
	span->end = end;
	return 0;
}

/* Verifies all numerical native spans and caller CPU images before the first list byte is written. */
static int
validate_images(
	const struct bcm2711_native_pass *state,
	const struct bcm2711_native_pass_sizes *sizes,
	struct bcm2711_native_pass_image *bin,
	struct bcm2711_native_pass_image *render,
	struct bcm2711_native_pass_image *tile)
{
	struct native_pass_span spans[6];
	struct native_pass_span cpu[3];
	struct bcm2711_native_pass_image *images[3];
	uint32_t required[3];
	uint32_t index;
	uint32_t other;
	uint64_t output_bytes;
	int error;

	/* All active list bytes and the complete raster rows must fit their independently owned intervals. */
	output_bytes = (uint64_t)state->pitch * state->height;
	if (output_bytes > state->output_bytes ||
	    state->pool_bytes < sizes->pool ||
	    state->state_bytes < sizes->state)
		return ENOSPC;

	/* The output, first-block pool and tile state are distinct from all command storage. */
	error = prepare_span(state->output, state->output_bytes, 64U, &spans[3]);
	if (error != 0)
		return error;
	error = prepare_span(state->pool, state->pool_bytes, 64U, &spans[4]);
	if (error != 0)
		return error;
	error = prepare_span(state->state, state->state_bytes, 4096U, &spans[5]);
	if (error != 0)
		return error;

	/* Each command image owns enough bytes for both its active stream and its retained allocation. */
	images[0] = bin;
	images[1] = render;
	images[2] = tile;
	required[0] = sizes->bin;
	required[1] = sizes->render;
	required[2] = sizes->tile;
	for (index = 0; index < 3U; index++) {
		/* A missing CPU image or short capacity cannot supply a complete command list. */
		if (images[index]->bytes == NULL)
			return EINVAL;

		/* Capacity refusal occurs before inspecting or mutating any preassembled draw sequence. */
		if (images[index]->capacity < required[index])
			return ENOSPC;

		/* Complete native command intervals exclude reserved page zero and encoded endpoint wrap. */
		error = prepare_span(images[index]->address, images[index]->capacity, 8U, &spans[index]);
		if (error != 0)
			return error;

		/* Each command list's exclusive end must fit in the native start/end register or packet. */
		if ((uint64_t)images[index]->address + required[index] >= ((uint64_t)1 << 32))
			return EINVAL;

		/* CPU intervals also stay disjoint so serialization cannot overwrite another complete image. */
		cpu[index].first = (uint64_t)(uintptr_t)images[index]->bytes;
		cpu[index].end = cpu[index].first + images[index]->capacity;
		if (cpu[index].end <= cpu[index].first)
			return EINVAL;
	}

	/* No whole native allocation may alias a framebuffer, tile state, pool or another command image. */
	for (index = 0; index < 6U; index++) {
		for (other = index + 1U; other < 6U; other++) {
			/* Half-open allocations can meet at their endpoints but must never overlap. */
			if (spans[index].first < spans[other].end && spans[other].first < spans[index].end)
				return EINVAL;
		}
	}

	/* All three complete CPU images are independent through final publication. */
	for (index = 0; index < 3U; index++) {
		for (other = index + 1U; other < 3U; other++) {
			/* Full capacities, including untouched padding, cannot alias another image. */
			if (cpu[index].first < cpu[other].end && cpu[other].first < cpu[index].end)
				return EINVAL;
		}
	}

	/* Succeeded: every subsequent store targets a validated independent image and numerical relocation. */
	return 0;
}

/* Writes one explicit little-endian native word without imposing CPU alignment. */
static void
write_word(
	uint8_t *bytes,
	uint32_t word)
{
	/* Native relocations and raw data bits use the target's byte order on every host. */
	bytes[0] = (uint8_t)word;
	bytes[1] = (uint8_t)(word >> 8);
	bytes[2] = (uint8_t)(word >> 16);
	bytes[3] = (uint8_t)(word >> 24);

	/* Succeeded: the complete word is initialized. */
	return;
}

/* Writes one bounded sixteen-bit framebuffer dimension or binning field. */
static void
write_half(
	uint8_t *bytes,
	uint32_t half)
{
	/* The admitted dimensions fit the exact native field. */
	bytes[0] = (uint8_t)half;
	bytes[1] = (uint8_t)(half >> 8);

	/* Succeeded: the complete halfword is initialized. */
	return;
}

/* Encodes one complete raster load/store with RGBA8 bytes and no additional fragment-code channel swap. */
static void
write_raster(
	uint8_t *bytes,
	uint8_t opcode,
	const struct bcm2711_native_pass *state)
{
	/* The hardware pitch starts at payload bit twenty-eight and uses bytes for raster storage. */
	bytes[0] = opcode;
	write_word(bytes + 1, (27U << 12) | (state->pitch << 28));
	write_word(bytes + 5, state->pitch >> 4);
	write_word(bytes + 9, state->output);

	/* Succeeded: the complete packet references only the owned native output interval. */
	return;
}

/* Emits complete rendering state, both required dummy stores and exactly one invocation per selected tile. */
static void
encode_render(
	const struct bcm2711_native_pass *state,
	const struct bcm2711_native_pass_image *tile,
	const struct bcm2711_native_pass_sizes *sizes,
	uint8_t *bytes)
{
	uint32_t columns;
	uint32_t rows;
	uint32_t x;
	uint32_t y;
	uint32_t cursor;

	/* Common is the first configuration; target clear/type follow and depth clear is last. */
	bytes[0] = 121;
	write_half(bytes + 2, state->width);
	write_half(bytes + 4, state->height);
	bytes[6] = 0x40U;
	bytes[9] = 121;
	bytes[10] = 3;
	write_word(bytes + 11, state->clear_colour);
	bytes[18] = 121;
	bytes[19] = 0x81U;
	bytes[27] = 121;
	bytes[28] = 2;
	write_word(bytes + 30, 0x3f800000U);

	/* Auto-chained sixty-four-byte first blocks precede all branches into bin-produced lists. */
	bytes[36] = 126;
	bytes[37] = 4;
	columns = (state->width + 63U) / 64U;
	rows = (state->height + 63U) / 64U;
	bytes[38] = 122;
	bytes[39] = (uint8_t)(sizes->supertile_width - 1U);
	bytes[40] = (uint8_t)(sizes->supertile_height - 1U);
	bytes[41] = (uint8_t)sizes->columns;
	bytes[42] = (uint8_t)sizes->rows;
	write_word(bytes + 43, columns | (rows << 12));

	/* Two NONE stores settle the 4.x type/size change before any shader can observe old TLB state. */
	bytes[47] = 124;
	bytes[51] = 26;
	bytes[52] = 29;
	bytes[53] = 8;
	bytes[65] = 25;
	bytes[66] = 3;
	bytes[67] = 27;
	bytes[68] = 124;
	bytes[72] = 26;
	bytes[73] = 29;
	bytes[74] = 8;
	bytes[86] = 27;
	bytes[87] = 19;

	/* Layer zero uses first blocks at the pool base and one independently owned generic sub-list. */
	bytes[88] = 123;
	write_word(bytes + 89, state->pool);
	bytes[93] = 20;
	write_word(bytes + 94, tile->address);
	write_word(bytes + 98, tile->address + sizes->tile);

	/* Selected supertiles cover the bounded tile rectangle in row-major order within the complete configured grid. */
	cursor = 102;
	for (y = state->first_y / sizes->supertile_height; y <= state->last_y / sizes->supertile_height; y++) {
		for (x = state->first_x / sizes->supertile_width; x <= state->last_x / sizes->supertile_width; x++) {
			/* Each selected group executes the generic sequence for all of its valid framebuffer tiles. */
			bytes[cursor] = 23;
			bytes[cursor + 1U] = (uint8_t)x;
			bytes[cursor + 2U] = (uint8_t)y;
			cursor += 3U;
		}
	}

	/* Succeeded: the render queue reaches its native completion marker after all selected tiles. */
	bytes[cursor] = 13;
	return;
}

/* Emits exact load/draw/store state for one tile, including explicit primitive type and instance ID. */
static void
encode_tile(
	const struct bcm2711_native_pass *state,
	uint8_t *bytes)
{
	uint32_t cursor;

	/* Implicit coordinates come from the enclosing supertile invocation and remain bounded by complete framebuffer dimensions. */
	bytes[0] = 125;
	cursor = 1;

	/* A raster load preserves old samples before the compiled fragment code reads or updates the tile. */
	if (state->load != 0) {
		write_raster(bytes + cursor, 30, state);
		cursor += 13U;
	}

	/* Loads finish before triangle-list mode and the PTB-assumed zero instance ID are established. */
	bytes[cursor] = 26;
	bytes[cursor + 1U] = 56;
	bytes[cursor + 2U] = 2;
	bytes[cursor + 3U] = 54;
	cursor += 8U;
	bytes[cursor] = 21;
	cursor += 2U;

	/* Store writes the colour target, or the mandatory NONE packet when attachment storage is discarded. */
	if (state->store != 0) {
		write_raster(bytes + cursor, 29, state);
	} else {
		bytes[cursor] = 29;
		bytes[cursor + 1U] = 8;
	}

	/* Both tile buffers clear between invocations; depth/stencil uses the explicit 4.2 clear command. */
	cursor += 13U;
	bytes[cursor] = 25;
	bytes[cursor + 1U] = 3;
	bytes[cursor + 2U] = 27;
	bytes[cursor + 3U] = 18;

	/* Succeeded: one complete generic tile invocation returns to the next selected supertile. */
	return;
}
