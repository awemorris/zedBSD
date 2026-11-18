/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the Mali executor's depth and stencil surface and
 * mip level packers (src/drivers/gpu/mali/vk/vk-surface.c): the levels'
 * count, extents and places in a layer, the sampler's level of detail
 * words (fixed point, rounding and clamps), and the depth and stencil
 * words against words packed by hand from the field places of Mesa
 * 26.2.6 src/panfrost/genxml/v10.xml (structs "ZS Target", "S Target",
 * "Framebuffer Parameters", "Framebuffer pointer", "Depth/stencil",
 * "Sampler"; enums "ZS Format", "S Format", "Z Internal Format",
 * "Block Format", "Stencil Op", "Func", "Mipmap Mode", "Pixel Kill").
 *
 * cc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude \
 *    tests/mali-vk-surface-host-test.c src/drivers/gpu/mali/vk/vk-surface.c -o <out> && <out>
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/gpu/mali/vk/vk-surface.h"

/* The float bits the checks use. */
#define F_ZERO		0x00000000U
#define F_HALF		0x3f000000U
#define F_ONE		0x3f800000U
#define F_TWO		0x40000000U
#define F_3_25		0x40500000U
#define F_MINUS_ONE	0xbf800000U
#define F_MINUS_1_5	0xbfc00000U
#define F_100		0x42c80000U
#define F_MINUS_200	0xc3480000U
#define F_1000		0x447a0000U
#define F_1_1024	0x3a800000U
#define F_1_512		0x3b000000U
#define F_3_1024	0x3b400000U

/* The Vulkan numbers the checks use (formats, compare and stencil ops, mipmap modes). */
#define VK_D16		124U
#define VK_X8_D24	125U
#define VK_D32		126U
#define VK_S8		127U
#define VK_D24S8	129U
#define VK_RGBA8	37U
#define VK_LESS		1U
#define VK_EQUAL	2U
#define VK_KEEP		0U
#define VK_REPLACE	2U
#define VK_INCR_CLAMP	3U
#define VK_NEAREST	0U
#define VK_LINEAR	1U

/* How many checks failed and ran. */
static int failures;
static int checks;

static void expect(const char *name, uint64_t got, uint64_t want);
static void check_levels(void);
static void check_sampler(void);
static void check_zs(void);
static void check_depth_stencil(void);

/* Runs every check and reports the count. */
int
main(void)
{
	check_levels();
	check_sampler();
	check_zs();
	check_depth_stencil();

	/* The report. */
	if (failures != 0) {
		printf("%d of %d checks failed\n", failures, checks);
		return 1;
	}
	printf("all checks passed (%d)\n", checks);
	return 0;
}

/* The levels: their count, extents and places in a layer of a linear image. */
static void
check_levels(void)
{
	struct mali_surface_level levels[MALI_SURFACE_LEVELS];
	uint64_t layer;
	unsigned index;

	/* The count of a full chain: one more than log2 of the larger edge. */
	expect("count 1x1", mali_surface_level_count(1, 1), 1);
	expect("count 2x1", mali_surface_level_count(2, 1), 2);
	expect("count 300x17", mali_surface_level_count(300, 17), 9);
	expect("count 1x1024", mali_surface_level_count(1, 1024), 11);
	expect("count 8192 square", mali_surface_level_count(8192, 8192), 14);
	expect("count 8192 is the most", mali_surface_level_count(8192, 8192), MALI_SURFACE_LEVELS);

	/* An extent halves a level, down to one. */
	expect("extent 300 at 0", mali_surface_extent(300, 0), 300);
	expect("extent 300 at 3", mali_surface_extent(300, 3), 37);
	expect("extent 5 at 10", mali_surface_extent(5, 10), 1);
	expect("extent at 40", mali_surface_extent(0xffffffffU, 40), 1);

	/* One level: the row of 1920 RGBA8 texels and the 1080 rows, the layer that. */
	memset(levels, 0xa5, sizeof(levels));
	layer = mali_surface_levels(1920, 1080, 4, 64, 1, levels);
	expect("one level offset", levels[0].offset, 0);
	expect("one level pitch", levels[0].pitch, 7680);
	expect("one level bytes", levels[0].bytes, 7680ULL * 1080U);
	expect("one level layer", layer, 7680ULL * 1080U);
	expect("one level width", levels[0].width, 1920);
	expect("one level height", levels[0].height, 1080);
	expect("one level leaves the next", levels[1].pitch, 0xa5a5a5a5U);

	/* A 2-byte row of 33 texels takes 128 bytes (64-byte rows). */
	(void)mali_surface_levels(33, 3, 2, 64, 1, levels);
	expect("d16 pitch", levels[0].pitch, 128);
	expect("d16 bytes", levels[0].bytes, 384);

	/*
	 * The nine levels of 300 by 17 RGBA8 texels, by hand: each pitch the
	 * width's bytes rounded up to 64, each level its pitch times its
	 * height rounded up to 64, after the level before.
	 */
	{
		static const uint32_t width[9] = { 300, 150, 75, 37, 18, 9, 4, 2, 1 };
		static const uint32_t height[9] = { 17, 8, 4, 2, 1, 1, 1, 1, 1 };
		static const uint32_t pitch[9] = { 1216, 640, 320, 192, 128, 64, 64, 64, 64 };
		static const uint64_t bytes[9] = { 20672, 5120, 1280, 384, 128, 64, 64, 64, 64 };
		static const uint64_t offset[9] = { 0, 20672, 25792, 27072, 27456, 27584, 27648, 27712, 27776 };

		layer = mali_surface_levels(300, 17, 4, 64, 9, levels);
		for (index = 0; index < 9U; index++) {
			expect("chain width", levels[index].width, width[index]);
			expect("chain height", levels[index].height, height[index]);
			expect("chain pitch", levels[index].pitch, pitch[index]);
			expect("chain bytes", levels[index].bytes, bytes[index]);
			expect("chain offset", levels[index].offset, offset[index]);
		}
		expect("chain layer", layer, 27840);
	}

	/* A layer's bytes are a multiple of 64 and every level starts 64-aligned. */
	layer = mali_surface_levels(8192, 8192, 16, 64, MALI_SURFACE_LEVELS, levels);
	expect("full chain layer aligned", layer & 63U, 0);
	for (index = 0; index < MALI_SURFACE_LEVELS; index++)
		expect("full chain offset aligned", levels[index].offset & 63U, 0);
	expect("full chain last extent", levels[MALI_SURFACE_LEVELS - 1U].width, 1);
	expect("full chain last offset", levels[MALI_SURFACE_LEVELS - 1U].offset + levels[MALI_SURFACE_LEVELS - 1U].bytes, layer);
}

/* The sampler's level of detail words: mode 0:30..31, LODs 1:0..12 and 1:16..28, bias 2:0..15. */
static void
check_sampler(void)
{
	uint32_t words[8];
	bool levels;

	/* A range of level 0 without a bias leaves the words of no mipmaps alone. */
	memset(words, 0, sizeof(words));
	words[0] = 1U | (1U << 30);
	levels = mali_surface_sampler_lod(words, VK_LINEAR, F_ZERO, F_ZERO, F_ZERO);
	expect("lod none says false", levels, false);
	expect("lod none keeps mode", words[0], 1U | (1U << 30));
	expect("lod none keeps range", words[1], 0);

	/* Nearest to the clamp-none maximum (1000.0): the mode nearest, the maximum the field's most. */
	memset(words, 0, sizeof(words));
	words[0] = 1U | (1U << 30);
	levels = mali_surface_sampler_lod(words, VK_NEAREST, F_ZERO, F_1000, F_ZERO);
	expect("lod nearest says true", levels, true);
	expect("lod nearest mode", words[0], 1U);
	expect("lod nearest range", words[1], 0x1fffU << 16);
	expect("lod nearest bias", words[2], 0);

	/* Linear from 0.5 to 3.25, a bias of -1.5: trilinear, 128 and 832, 0xfe80; the compare and anisotropy kept. */
	memset(words, 0, sizeof(words));
	words[0] = 1U | (1U << 30);
	words[1] = 3U << 13;
	words[2] = 1U << 16;
	levels = mali_surface_sampler_lod(words, VK_LINEAR, F_HALF, F_3_25, F_MINUS_1_5);
	expect("lod linear says true", levels, true);
	expect("lod linear mode", words[0], 1U | (3U << 30));
	expect("lod linear range", words[1], 128U | (3U << 13) | (832U << 16));
	expect("lod linear bias", words[2], 0xfe80U | (1U << 16));

	/* Rounded to nearest: 1/1024 is 0.25 of a unit (0), 1/512 a half (1), 3/1024 0.75 (1). */
	memset(words, 0, sizeof(words));
	(void)mali_surface_sampler_lod(words, VK_NEAREST, F_1_1024, F_1_512, F_3_1024);
	expect("lod round quarter", words[1] & 0x1fffU, 0);
	expect("lod round half", words[1] >> 16, 1);
	expect("lod round three quarters", words[2] & 0xffffU, 1);

	/* Clamped: a negative minimum is 0, a bias of 100 fits (25600), one of -200 is the least (0x8000). */
	memset(words, 0, sizeof(words));
	(void)mali_surface_sampler_lod(words, VK_NEAREST, F_MINUS_ONE, F_TWO, F_100);
	expect("lod clamp minimum", words[1], 512U << 16);
	expect("lod bias 100", words[2], 25600U);
	memset(words, 0, sizeof(words));
	(void)mali_surface_sampler_lod(words, VK_NEAREST, F_ZERO, F_ONE, F_MINUS_200);
	expect("lod bias -200", words[2], 0x8000U);
	expect("lod max one", words[1], 256U << 16);
}

/* The depth and stencil formats, the target extension, the framebuffer's words and its pointer. */
static void
check_zs(void)
{
	const struct mali_surface_zs_format *format;
	struct mali_surface_zs_target target;
	uint32_t words[16];
	uint32_t fbd[16];

	/* The formats: ZS Format D16 1, D24X8 4, D24S8 5, D32 14; S Format S8 1, X24S8 4; Z Internal D16 0, D24 1, D32 2. */
	format = mali_surface_zs_find(VK_D16);
	expect("d16 found", format != NULL, true);
	if (format != NULL) {
		expect("d16 write", format->zs_write, 1);
		expect("d16 internal", format->z_internal, 0);
		expect("d16 aspects", format->aspects, MALI_SURFACE_ASPECT_DEPTH);
	}
	format = mali_surface_zs_find(VK_X8_D24);
	expect("x8d24 write", format != NULL ? format->zs_write : 0, 4);
	format = mali_surface_zs_find(VK_D32);
	expect("d32 write", format != NULL ? format->zs_write : 0, 14);
	expect("d32 internal", format != NULL ? format->z_internal : 0, 2);
	format = mali_surface_zs_find(VK_S8);
	expect("s8 stencil", format != NULL ? format->s_write : 0, 1);
	expect("s8 aspects", format != NULL ? format->aspects : 0, MALI_SURFACE_ASPECT_STENCIL);
	expect("rgba8 not found", mali_surface_zs_find(VK_RGBA8) == NULL, true);
	format = mali_surface_zs_find(VK_D24S8);
	expect("d24s8 found", format != NULL, true);
	if (format == NULL)
		return;
	expect("d24s8 write", format->zs_write, 5);
	expect("d24s8 stencil", format->s_write, 4);
	expect("d24s8 internal", format->z_internal, 1);
	expect("d24s8 aspects", format->aspects, MALI_SURFACE_ASPECT_DEPTH | MALI_SURFACE_ASPECT_STENCIL);

	/*
	 * The extension of a D24S8 target at 0x1234_5678_9000, 4096 bytes a
	 * row, 600 rows, the depth cleared and stored: ZS write 0:0 and
	 * linear (2) 0:4, S write 0:16 and linear 0:20, clean tile 0:31; the
	 * depth's base 8..9, stride 10, surface stride 11; the stencil's at
	 * the same memory 12..15.
	 */
	memset(&target, 0, sizeof(target));
	target.format = format;
	target.base = 0x123456789000ULL;
	target.pitch = 4096;
	target.surface_bytes = 4096ULL * 600U;
	target.depth_clear = true;
	target.depth_store = true;
	target.stencil_load = true;
	target.stencil_store = true;
	target.depth_clear_bits = F_ONE;
	target.stencil_clear_value = 0x5a;
	memset(words, 0xff, sizeof(words));
	mali_surface_zs_extension(words, &target);
	expect("ext word 0", words[0], 5U | (2U << 4) | (4U << 16) | (2U << 20) | (1U << 31));
	expect("ext depth base low", words[8], 0x56789000U);
	expect("ext depth base high", words[9], 0x1234U);
	expect("ext depth stride", words[10], 4096);
	expect("ext depth surface", words[11], 4096U * 600U);
	expect("ext stencil base low", words[12], 0x56789000U);
	expect("ext stencil base high", words[13], 0x1234U);
	expect("ext stencil stride", words[14], 4096);
	expect("ext stencil surface", words[15], 4096U * 600U);
	expect("ext word 1", words[1], 0);

	/* No clean tile write when neither aspect is cleared and stored. */
	target.depth_clear = false;
	mali_surface_zs_extension(words, &target);
	expect("ext no clean tile", words[0] >> 31, 0);

	/*
	 * The framebuffer's word 12: the stencil's write 8 and preload 9 (no
	 * clear value without a clear), the D24 internal format 16, the depth
	 * write 18, the extension 21; word 13 untouched without a depth clear.
	 */
	memset(fbd, 0, sizeof(fbd));
	fbd[13] = 0xdeadbeefU;
	mali_surface_zs_framebuffer(fbd, &target);
	expect("fbd word 12", fbd[12], (1U << 8) | (1U << 9) | (1U << 16) | (1U << 18) | (1U << 21));
	expect("fbd word 13 kept", fbd[13], 0xdeadbeefU);

	/* Cleared aspects: the stencil's clear in 0..7, the depth's float in word 13, no preloads. */
	target.depth_clear = true;
	target.stencil_load = false;
	target.stencil_clear = true;
	memset(fbd, 0, sizeof(fbd));
	fbd[12] = 1U << 30;
	mali_surface_zs_framebuffer(fbd, &target);
	expect("fbd cleared word 12", fbd[12], 0x5aU | (1U << 8) | (1U << 16) | (1U << 18) | (1U << 21) | (1U << 30));
	expect("fbd cleared word 13", fbd[13], F_ONE);

	/* A D32 depth loaded and stored: Z internal 2, write and preload, no stencil bits. */
	memset(&target, 0, sizeof(target));
	target.format = mali_surface_zs_find(VK_D32);
	target.depth_load = true;
	target.depth_store = true;
	memset(fbd, 0, sizeof(fbd));
	mali_surface_zs_framebuffer(fbd, &target);
	expect("fbd d32 word 12", fbd[12], (2U << 16) | (1U << 18) | (1U << 19) | (1U << 21));
	mali_surface_zs_extension(words, &target);
	expect("ext d32 word 0", words[0], 14U | (2U << 4));
	expect("ext d32 no stencil", words[12], 0);

	/* The framebuffer pointer's bit 1. */
	expect("pointer bit", mali_surface_zs_pointer(), 2);
}

/* The depth and stencil descriptor and the kill and update operations. */
static void
check_depth_stencil(void)
{
	struct mali_surface_zs_state state;
	struct mali_surface_kill kill;
	uint32_t words[8];
	uint32_t face;

	/*
	 * Nothing: the type 7, both stencil compares always (7) at 0:4 and
	 * 0:16, the value masks full, depth cull 4:22, clamp [0, 1] (1) 4:23,
	 * the fixed-function source (2) 4:25, the depth function always 4:29.
	 */
	memset(&state, 0, sizeof(state));
	mali_surface_depth_stencil(words, &state);
	expect("zsd none word 0", words[0], 7U | (7U << 4) | (7U << 16));
	expect("zsd none word 1", words[1], 0xffff0000U);
	expect("zsd none word 4", words[4], (1U << 22) | (1U << 23) | (2U << 25) | (7U << 29));
	expect("zsd none writes", mali_surface_zs_writes(&state), false);

	/* A depth test without a depth aspect still passes everything. */
	state.depth_test = true;
	state.depth_write = true;
	state.depth_compare = VK_LESS;
	mali_surface_depth_stencil(words, &state);
	expect("zsd no aspect word 4", words[4], (1U << 22) | (1U << 23) | (2U << 25) | (7U << 29));

	/*
	 * Depth less with write, stencil equal with keep, replace (hardware
	 * 1) on pass and increment and clamp (hardware 6) on depth fail; the
	 * write masks 0x0f, value masks 0xff, references 0x80; a bias.
	 */
	state.has_depth = true;
	state.has_stencil = true;
	state.stencil_test = true;
	state.front.fail_op = VK_KEEP;
	state.front.pass_op = VK_REPLACE;
	state.front.depth_fail_op = VK_INCR_CLAMP;
	state.front.compare_op = VK_EQUAL;
	state.front.compare_mask = 0xff;
	state.front.write_mask = 0x0f;
	state.front.reference = 0x80;
	state.back = state.front;
	state.bias = true;
	state.bias_constant = F_ONE;
	state.bias_slope = F_TWO;
	state.bias_clamp = 0;
	mali_surface_depth_stencil(words, &state);
	face = 2U | (0U << 3) | (6U << 6) | (1U << 9);
	expect("zsd face by hand", face, 0x382U);
	expect("zsd word 0", words[0], 7U | (face << 4) | (face << 16) | (1U << 31));
	expect("zsd word 1", words[1], 0xffff0f0fU);
	expect("zsd word 2", words[2], 0x8080U);
	expect("zsd word 4", words[4], (1U << 22) | (1U << 23) | (2U << 25) | (1U << 27) | (1U << 28) | (1U << 29));
	expect("zsd units", words[5], F_ONE);
	expect("zsd factor", words[6], F_TWO);
	expect("zsd clamp", words[7], 0);
	expect("zsd writes", mali_surface_zs_writes(&state), true);

	/* The program's depth and stencil: source shader (3) at 4:25, stencil from the shader 0:30. */
	state.shader_depth = true;
	state.shader_stencil = true;
	mali_surface_depth_stencil(words, &state);
	expect("zsd shader source", (words[4] >> 25) & 3U, 3);
	expect("zsd shader stencil", (words[0] >> 30) & 1U, 1);

	/* Kill and update (DCD flags 0 bits 2..3, 4..5): tests that may fail are force early (0). */
	memset(&kill, 0, sizeof(kill));
	kill.state = &state;
	state.shader_depth = false;
	state.shader_stencil = false;
	expect("kill early", mali_surface_kill_flags(&kill), 0);

	/* The program's depth: both late (3). */
	state.shader_depth = true;
	expect("kill shader depth", mali_surface_kill_flags(&kill), (3U << 2) | (3U << 4));

	/* A changed coverage with a depth write: the update late, the kill early. */
	state.shader_depth = false;
	kill.coverage = true;
	expect("kill coverage", mali_surface_kill_flags(&kill), 3U << 4);

	/* Tests that always pass: weak early (2); a program writing memory makes the kill late. */
	memset(&state, 0, sizeof(state));
	kill.coverage = false;
	expect("kill weak", mali_surface_kill_flags(&kill), (2U << 2) | (2U << 4));
	kill.writes_memory = true;
	expect("kill memory", mali_surface_kill_flags(&kill), (3U << 2) | (2U << 4));
}

/* Compares one value and records a failure. */
static void
expect(
	const char *name,
	uint64_t got,
	uint64_t want)
{
	checks++;
	if (got != want) {
		printf("FAIL %s: got %llx want %llx\n", name, (unsigned long long)got, (unsigned long long)want);
		failures++;
	}
}
