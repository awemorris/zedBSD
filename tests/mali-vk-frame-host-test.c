/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the Mali executor's frame shader packers
 * (src/drivers/gpu/mali/vk/vk-frame.c): the pre-frame draw, its depth
 * and stencil state, its sampler, and the framebuffer's and render
 * target's words that name it, against words packed by hand from the
 * field places of Mesa 26.2.6 src/panfrost/genxml/v10.xml (structs
 * "Draw", "DCD Flags 0/1/2", "Shader Environment", "Depth/stencil",
 * "Sampler", "Framebuffer Parameters", "RGB Render Target").
 *
 * cc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude tests/mali-vk-frame-host-test.c \
 *    src/drivers/gpu/mali/vk/vk-frame.c src/drivers/gpu/mali/vk/vk-desc.c -o <out> && <out>
 */

#include <stdint.h>
#include <stdio.h>

#include "drivers/gpu/mali/vk/vk-desc.h"
#include "drivers/gpu/mali/vk/vk-frame.h"

/* How many checks failed and ran. */
static int failures;
static int checks;

static void expect(const char *name, uint64_t got, uint64_t want);
static void check_draw(void);
static void check_states(void);
static void check_framebuffer(void);

/* Runs every check and reports the count. */
int
main(void)
{
	/* Each group of descriptors. */
	check_draw();
	check_states();
	check_framebuffer();

	/* The report. */
	if (failures != 0) {
		printf("%d of %d checks failed\n", failures, checks);
		return 1;
	}

	printf("all checks passed (%d)\n", checks);
	return 0;
}

/* Compares one value and records a failure. */
static void
expect(
	const char *name,
	uint64_t got,
	uint64_t want)
{
	/* One check more; a difference is said and counted. */
	checks++;
	if (got != want) {
		printf("FAIL %s: got %llx want %llx\n", name, (unsigned long long)got, (unsigned long long)want);
		failures++;
	}
}

/* The pre-frame draw of render target 0, and of target 2. */
static void
check_draw(void)
{
	struct mali_frame_draw draw = { 0 };
	uint32_t words[32];
	unsigned index;

	/* A draw with every pointer set apart. */
	draw.blend = 0x123400040ULL;
	draw.blend_count = 1U;
	draw.resources = 0x200000004ULL;
	draw.shader = 0x300000080ULL;
	draw.storage = 0x4000000c0ULL;
	draw.depth = 0x500000100ULL;
	draw.target = 0U;
	for (index = 0; index < 32U; index++)
		words[index] = 0xdeadbeefU;
	mali_frame_draw(words, &draw);

	/* Flags 0: forward pixel kill both ways (0, 1), both kills force early (0), clean fragment write (9). */
	expect("draw flags 0", words[0], 0x1U | 0x2U | (1U << 9));

	/* Flags 1: every sample, render target 0. */
	expect("draw flags 1", words[1], 0xffffU | (1U << 16));

	/* The vertex array stays zero. */
	expect("draw vertex array 2", words[2], 0);
	expect("draw vertex array 3", words[3], 0);
	expect("draw vertex array 4", words[4], 0);

	/* Flags 2: render target 0 written (12..23), none read (0..11). */
	expect("draw flags 2", words[5], 1U << 12);

	/* The depth range 0 to 1. */
	expect("draw minimum z", words[6], 0);
	expect("draw maximum z", words[7], 0x3f800000U);

	/* The depth and stencil descriptor in words 10..11. */
	expect("draw depth low", words[10], 0x00000100U);
	expect("draw depth high", words[11], 0x5U);

	/* The blend: one descriptor, its address above bit 4. */
	expect("draw blend low", words[12], 0x23400041U);
	expect("draw blend high", words[13], 0x1U);

	/* No occlusion. */
	expect("draw occlusion low", words[14], 0);
	expect("draw occlusion high", words[15], 0);

	/* The shader environment: no attribute offset, no FAU, then resources, shader, storage. */
	expect("draw attribute offset", words[16], 0);
	expect("draw FAU count", words[17], 0);
	expect("draw resources low", words[24], 0x00000004U);
	expect("draw resources high", words[25], 0x2U);
	expect("draw shader low", words[26], 0x00000080U);
	expect("draw shader high", words[27], 0x3U);
	expect("draw storage low", words[28], 0x000000c0U);
	expect("draw storage high", words[29], 0x4U);
	expect("draw FAU low", words[30], 0);
	expect("draw FAU high", words[31], 0);

	/* Target 2: flags 1 and flags 2 name it. */
	draw.target = 2U;
	mali_frame_draw(words, &draw);
	expect("draw target 2 flags 1", words[1], 0xffffU | (4U << 16));
	expect("draw target 2 flags 2", words[5], 4U << 12);
}

/* The depth and stencil descriptor and the sampler. */
static void
check_states(void)
{
	uint32_t words[8];

	/* Depth/stencil: type 7, both compares always, the value masks; depth: clamp [0, 1], fixed function, always. */
	mali_frame_depth(words);
	expect("depth word 0", words[0], 7U | (7U << 4) | (7U << 16));
	expect("depth word 1", words[1], 0xffff0000U);
	expect("depth word 2", words[2], 0);
	expect("depth word 4", words[4], (1U << 23) | (2U << 25) | (7U << 29));
	expect("depth no cull", (words[4] >> 22) & 1U, 0);
	expect("depth no write", (words[4] >> 27) & 1U, 0);
	expect("depth no stencil test", (words[0] >> 31) & 1U, 0);

	/* Sampler: type 1, clamp to edge (9) three ways, round even, clamp arrays, nearest, no mipmaps, unnormalized. */
	mali_frame_sampler(words);
	expect("sampler word 0", words[0], 1U | (9U << 8) | (9U << 12) | (9U << 16) | (1U << 21) | (1U << 26) | (1U << 27) | (1U << 28) | (1U << 30));
	expect("sampler unnormalized", (words[0] >> 25) & 1U, 0);
	expect("sampler word 1", words[1], 0);
	expect("sampler word 2", words[2], 0);
}

/* The framebuffer's frame shader words and a render target's clean tile write. */
static void
check_framebuffer(void)
{
	struct mali_desc_target target = { 0 };
	uint32_t words[16];

	/* A framebuffer of 1920 by 1080, one target, 16 by 16 tiles, 1 KiB of colour buffer. */
	mali_desc_framebuffer(words, 1920, 1080, 1, 256, 1024, 0x700000000ULL);
	mali_frame_shaders(words, MALI_FRAME_INTERSECT, 0x800000180ULL);
	expect("framebuffer pre-frame 0", words[0] & 7U, 2);
	expect("framebuffer pre-frame 1", (words[0] >> 3) & 7U, 0);
	expect("framebuffer post-frame", (words[0] >> 6) & 7U, 0);
	expect("framebuffer draws low", words[6], 0x00000180U);
	expect("framebuffer draws high", words[7], 0x8U);
	expect("framebuffer extent kept", words[8], 1919U | (1079U << 16));
	expect("framebuffer tiler kept", words[15], 0x7U);

	/* The mode changes in place; the address stays. */
	mali_frame_shaders(words, MALI_FRAME_ALWAYS, 0x800000180ULL);
	expect("framebuffer pre-frame 0 always", words[0] & 7U, 1);

	/* A render target with its clean tile write (1:31); the other bits stay. */
	target.write = true;
	target.internal = 38U;
	mali_desc_target(words, &target);
	expect("target before", words[1] >> 31, 0);
	mali_frame_clean_tile(words);
	expect("target clean tile", words[1] >> 31, 1);
	expect("target write kept", words[1] & 1U, 1);
	expect("target internal kept", words[0] >> 26, 38);
}
