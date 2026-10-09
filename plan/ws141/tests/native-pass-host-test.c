/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Pure pass packet fixtures expose full streams to the independent fixed-XML oracle; numerical addresses do not represent physical GPU execution. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/native-pass.h"

static void print_image(const char *name, const struct bcm2711_native_pass_image *image);
static void verify_refusal(struct bcm2711_native_pass *state, struct bcm2711_native_pass_image *images, int expected);

/*
 * Exercises complete pass lists, tile geometry and atomic refusal with synthetic independently owned CPU storage.
 *
 * The XML oracle separately builds the exact packet fields for all load/store
 * variants.  Maximum dimensions verify complete traversal and allocation
 * bounds without a physical framebuffer, native MMIO or QPU execution.
 */
int
main(
	void)
{
	struct bcm2711_native_pass state;
	struct bcm2711_native_pass_sizes sizes;
	struct bcm2711_native_pass_sizes saved;
	struct bcm2711_native_pass_image images[3];
	uint8_t *render_bytes;
	uint32_t index;
	uint32_t cursor;
	uint32_t x;
	uint32_t y;
	uint32_t selected;
	int error;
	int same;

	/* Every synthetic list has independent CPU storage and a numerical native interval far from the other owners. */
	memset(images, 0, sizeof(images));
	for (index = 0; index < 3U; index++) {
		images[index].capacity = 16384U;
		images[index].address = (index + 1U) * 0x100000U;
		images[index].bytes = malloc(images[index].capacity);
		assert(images[index].bytes != NULL);
		memset(images[index].bytes, 0xa5, images[index].capacity);
	}

	/* A three-by-three framebuffer selects six tiles, crossing both tile boundaries and the final partial row/column. */
	memset(&state, 0, sizeof(state));
	state.width = 130;
	state.height = 129;
	state.pitch = 576;
	state.output = 0x400000U;
	state.output_bytes = 0x13000U;
	state.pool = 0x800000U;
	state.pool_bytes = 0x83000U;
	state.state = 0xa00000U;
	state.state_bytes = 4096;
	state.first_x = 1;
	state.last_x = 2;
	state.last_y = 2;
	state.load = 1;
	state.store = 1;
	state.clear_colour = 0x12345678U;
	error = bcm2711_native_pass_measure(&state, 2, &sizes);
	assert(error == 0 && sizes.bin == 246U && sizes.render == 121U && sizes.tile == 41U);
	assert(sizes.pool == state.pool_bytes && sizes.state == 2304U);
	for (index = 0; index < 2U * BCM2711_NATIVE_BIN_BYTES; index++)
		images[0].bytes[BCM2711_NATIVE_PASS_BIN_PREFIX + index] = (uint8_t)(index * 17U + 3U);
	error = bcm2711_native_pass_encode(&state, 2, &images[0], &images[1], &images[2]);
	assert(error == 0);
	print_image("load-bin", &images[0]);
	print_image("load-render", &images[1]);
	print_image("load-tile", &images[2]);

	/* All preassembled numerical draw bytes survive, and initialized stream ends do not touch retained padding. */
	for (index = 0; index < 2U * BCM2711_NATIVE_BIN_BYTES; index++)
		assert(images[0].bytes[BCM2711_NATIVE_PASS_BIN_PREFIX + index] == (uint8_t)(index * 17U + 3U));
	for (index = 0; index < 3U; index++)
		assert(images[index].bytes[images[index].used] == 0xa5);

	/* No-load clear uses the configured tile colour and complete store/clear sequencing. */
	state.load = 0;
	error = bcm2711_native_pass_encode(&state, 0, &images[0], &images[1], &images[2]);
	assert(error == 0 && images[0].used == 14U && images[2].used == 28U);
	print_image("clear-bin", &images[0]);
	print_image("clear-render", &images[1]);
	print_image("clear-tile", &images[2]);

	/* Store discard still emits the mandatory NONE store before explicit colour/depth clear and end markers. */
	state.store = 0;
	error = bcm2711_native_pass_encode(&state, 0, &images[0], &images[1], &images[2]);
	assert(error == 0);
	print_image("discard-tile", &images[2]);

	/* Short geometry storage and complete-span aliasing refuse before changing any previously encoded byte. */
	state.output_bytes = state.pitch * state.height - 1U;
	verify_refusal(&state, images, ENOSPC);
	state.output_bytes = 0x13000U;
	state.pool_bytes--;
	verify_refusal(&state, images, ENOSPC);
	state.pool_bytes++;
	state.state_bytes = 2303U;
	verify_refusal(&state, images, ENOSPC);
	state.state_bytes = 4096U;
	state.pool = state.output;
	verify_refusal(&state, images, EINVAL);
	state.pool = 0x800000U;
	images[1].address = images[0].address + 8192U;
	verify_refusal(&state, images, EINVAL);
	images[1].address = 0x200000U;
	images[2].address = 0xfffffff0U;
	verify_refusal(&state, images, EINVAL);
	images[2].address = 0x300000U;

	/* Capacity and CPU-image overlap independently refuse even when numerical native intervals remain distinct. */
	images[1].capacity = 120U;
	verify_refusal(&state, images, ENOSPC);
	images[1].capacity = 16384U;
	render_bytes = images[1].bytes;
	images[1].bytes = images[0].bytes;
	verify_refusal(&state, images, EINVAL);
	images[1].bytes = NULL;
	verify_refusal(&state, images, EINVAL);
	images[1].bytes = render_bytes;

	/* Invalid geometry never publishes a partial size result or silently truncates the native twenty-bit pitch field. */
	memset(&sizes, 0x5a, sizeof(sizes));
	saved = sizes;
	state.pitch = 0x100000U;
	error = bcm2711_native_pass_measure(&state, 0, &sizes);
	assert(error == EINVAL);
	same = memcmp(&sizes, &saved, sizeof(sizes));
	assert(same == 0);
	state.pitch = 576;
	state.last_x = 3;
	verify_refusal(&state, images, EINVAL);
	state.last_x = 2;
	state.load = 2;
	verify_refusal(&state, images, EINVAL);
	state.load = 0;
	error = bcm2711_native_pass_measure(&state, 0xffffffffU, &sizes);
	assert(error == E2BIG);

	/* Maximum supported framebuffer geometry has sixty-four rows/columns and 256 four-by-four supertile invocations. */
	state.width = 4096;
	state.height = 4096;
	state.pitch = 16384;
	state.output_bytes = 64U * 1024U * 1024U;
	state.output = 0x10000000U;
	state.pool_bytes = 0xc2000U;
	state.state_bytes = 1024U * 1024U;
	state.state = 0xb00000U;
	state.first_x = 0;
	state.last_x = 63;
	state.last_y = 63;
	state.load = 1;
	state.store = 1;
	error = bcm2711_native_pass_measure(&state, 0, &sizes);
	assert(error == 0 && sizes.render == 871U && sizes.tile == 41U);
	assert(sizes.pool == state.pool_bytes && sizes.state == state.state_bytes);
	assert(sizes.supertile_width == 4U && sizes.supertile_height == 4U && sizes.columns == 16U && sizes.rows == 16U);
	error = bcm2711_native_pass_encode(&state, 0, &images[0], &images[1], &images[2]);
	assert(error == 0);
	cursor = 102;
	selected = 0;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			/* Every row-major coordinate is distinct and representable without a narrowed count or omitted last tile. */
			assert(images[1].bytes[cursor] == 23);
			assert(images[1].bytes[cursor + 1U] == x && images[1].bytes[cursor + 2U] == y);
			cursor += 3U;
			selected++;
		}
	}

	/* The final completion opcode follows all admitted coordinates. */
	assert(selected == 256U && images[1].bytes[cursor] == 13 && cursor + 1U == images[1].used);

	/* Independent heap ownership ends with the fixture; no production native mapping or DMA was launched. */
	for (index = 0; index < 3U; index++)
		free(images[index].bytes);
	puts("native-pass-host: PASS (complete pass packets, atomic refusal, draw preservation and grouped 4096 tile coverage)");
	return 0;
}

/* Prints one complete initialized command image for independently generated XML field comparison. */
static void
print_image(
	const char *name,
	const struct bcm2711_native_pass_image *image)
{
	uint32_t index;

	/* The observable image includes only the exact active sequence, never untouched allocation padding. */
	printf("pass-image %s ", name);
	for (index = 0; index < image->used; index++)
		printf("%02x", image->bytes[index]);
	putchar('\n');

	/* Succeeded: the independent oracle receives the complete numerical packet image. */
	return;
}

/* Proves every refused pass leaves previous command bytes intact and active counts unpublished. */
static void
verify_refusal(
	struct bcm2711_native_pass *state,
	struct bcm2711_native_pass_image *images,
	int expected)
{
	uint8_t *saved[3];
	uint32_t index;
	int error;
	int same;

	/* Independent snapshots cover every supplied nonempty CPU interval, including retained padding. */
	memset(saved, 0, sizeof(saved));
	for (index = 0; index < 3U; index++) {
		/* A missing CPU image has no bytes to snapshot. */
		if (images[index].bytes == NULL)
			continue;

		/* Every complete prior interval must stay bit-for-bit identical after refusal. */
		saved[index] = malloc(images[index].capacity);
		assert(saved[index] != NULL);
		memcpy(saved[index], images[index].bytes, images[index].capacity);
		images[index].used = 99;
	}

	/* Validation refuses atomically and withdraws any old active list length. */
	error = bcm2711_native_pass_encode(state, 0, &images[0], &images[1], &images[2]);
	assert(error == expected);
	for (index = 0; index < 3U; index++) {
		assert(images[index].used == 0);

		/* Null storage has no prior initialized bytes to compare or release. */
		if (saved[index] == NULL)
			continue;

		/* Unchanged full images prove late interval refusal did not mutate an earlier accepted prefix. */
		same = memcmp(saved[index], images[index].bytes, images[index].capacity);
		assert(same == 0);
		free(saved[index]);
	}

	/* Succeeded: every active count is zero and every previously owned byte survives the failed attempt. */
	return;
}
