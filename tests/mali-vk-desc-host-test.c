/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the Mali executor's descriptor packers
 * (src/drivers/gpu/mali/vk/vk-desc.c): each packer's words against
 * words packed by hand from the field places of Mesa 26.2.6
 * src/panfrost/genxml/v10.xml, and the fixed-function blend equations of
 * a replace, a straight alpha blend and a premultiplied one.
 *
 * cc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude tests/mali-vk-desc-host-test.c \
 *    src/drivers/gpu/mali/vk/vk-desc.c src/drivers/gpu/mali/vk/vk-surface.c -o <out> && <out>
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "drivers/gpu/mali/vk/vk-desc.h"
#include "drivers/gpu/mali/vk/vk-surface.h"

/* How many checks failed and ran. */
static int failures;
static int checks;

static void expect(const char *name, uint64_t got, uint64_t want);

/* Runs every check and reports the count. */
int
main(void)
{
	struct mali_desc_target target = { 0 };
	struct mali_desc_view view;
	uint32_t words[32];
	bool supported;
	uint32_t equation;

	/* A compute program of 40 registers preloading r55..r59 (64 registers a thread). */
	mali_desc_shader(words, MALI_DESC_STAGE_COMPUTE, 40, 0xf80U, 0x123456780ULL);
	expect("shader word 0", words[0], 8U | (1U << 4) | (0U << 30));
	expect("shader preload", words[1], 0xf80U);
	expect("shader code low", words[2], 0x23456780U);
	expect("shader code high", words[3], 0x1U);
	mali_desc_shader(words, MALI_DESC_STAGE_FRAGMENT, 16, 0, 0);
	expect("shader 32 registers", words[0], 8U | (2U << 4) | (1U << 8) | (2U << 30));
	mali_desc_shader(words, MALI_DESC_STAGE_VERTEX, 16, 0, 0);
	expect("shader position half warps", words[0], 8U | (3U << 4) | (1U << 8) | (2U << 30));
	mali_desc_shader(words, MALI_DESC_STAGE_VERTEX | MALI_DESC_STAGE_VARYING, 16, 0, 0);
	expect("shader varying full warps", words[0], 8U | (3U << 4) | (2U << 30));

	/* The storage, a table entry, a buffer. */
	mali_desc_local_storage(words);
	expect("storage no WLS", words[1], 0x1fU);
	mali_desc_resource(words, 0x10000040ULL, 64);
	expect("resource low", words[0], 0x10000040U);
	expect("resource descriptors", words[1], 1U << 24);
	expect("resource size", words[2], 64);
	mali_desc_buffer(words, 0x2000ULL, 300);
	expect("buffer type", words[0], 9U | (1U << 4));
	expect("buffer size", words[1], 300);

	/* A 2D texture of RGBA8 (187, RGBA) 1920 by 1080 and its plane. */
	mali_desc_texture(words, 187U << 12, 1920, 1080, 0x688U, 0x3000ULL);
	expect("texture word 0", words[0], 2U | (2U << 4) | ((187U << 12) << 10));
	expect("texture extent", words[1], 1919U | (1079U << 16));
	expect("texture swizzle", words[2], 0x688U);
	expect("texture planes", words[4], 0x3000U);
	mali_desc_plane(words, 4, 0x4000ULL, 7680, 7680U * 1080U, 1920, 1080);
	expect("plane word 0", words[0], 10U | (1U << 4) | (2U << 8) | (2U << 24));
	expect("plane stride", words[4], 7680);
	expect("plane extent", words[7], 1919U | (1079U << 16));

	/* An attribute of RG32F (175, RGB1) per instance at offset 8 of buffer 1, stride 16. */
	mali_desc_attribute(words, (175U << 12) | 16U, 3, true, 8, 1, 16);
	expect("attribute word 0", words[0], 5U | (1U << 4) | (1U << 8) | (((175U << 12) | 16U) << 10));
	expect("attribute word 1", words[1], 3U | (1U << 6));
	expect("attribute stride", words[4], 16);
	expect("workgroup", mali_desc_workgroup(8, 4, 1), 7U | (3U << 10));

	/* Blend equations: replace, straight alpha, premultiplied. */
	equation = mali_desc_blend_equation(1U | (1U << 13), 0xf, &supported);
	expect("replace supported", supported, 1);
	expect("replace rgb", equation & 0xfffU, 2U | (3U << 4) | (1U << 8));
	equation = mali_desc_blend_equation(6U | (7U << 5) | (6U << 13) | (7U << 18), 0xf, &supported);
	expect("alpha rgb", equation & 0xfffU, 3U | (0U << 4) | (5U << 8));
	expect("alpha mask", equation >> 28, 0xf);
	equation = mali_desc_blend_equation(1U | (7U << 5) | (1U << 13) | (7U << 18), 0x7, &supported);
	expect("premultiplied rgb", equation & 0xfffU, 2U | (3U << 4) | (5U << 8) | (1U << 11));
	expect("premultiplied mask", equation >> 28, 0x7);
	equation = mali_desc_blend_equation(1U | (1U << 5) | (3U << 10), 0xf, &supported);
	expect("min refused", supported, 0);
	mali_desc_blend(words, true, 0x12345678U, 0xf, 2, 187U << 12);
	expect("blend word 0", words[0], 1U | (1U << 9) | (1U << 11));
	expect("blend word 2", words[2], 2U | (3U << 3) | (2U << 16));
	mali_desc_blend(words, false, 0, 0xf, 0, (237U << 12) | (1U << 20));
	expect("blend sRGB word 0", words[0], (1U << 9) | (1U << 10) | (1U << 11));

	/* A render target of RGBA8 written linearly, its clear. */
	target.internal = 1;
	target.writeback = 19;
	target.swizzle = 0x60a;
	target.write = true;
	target.base = 0x5000;
	target.row_stride = 7680;
	target.internal_offset = 1024;
	target.clear[0] = 0xff0000ffU;
	mali_desc_target(words, &target);
	expect("target word 0", words[0], (64U << 4) | (1U << 26));
	expect("target word 1", words[1], 1U | (19U << 3) | (2U << 8) | (0x60aU << 16));
	expect("target base", words[8], 0x5000U);
	expect("target clear", words[12], 0xff0000ffU);

	/* A framebuffer of 1920 by 1080, one target, tiles of 256 pixels, 1 KiB of colour. */
	mali_desc_framebuffer(words, 1920, 1080, 1, 256, 1024, 0x6000ULL);
	expect("framebuffer extent", words[8], 1919U | (1079U << 16));
	expect("framebuffer word 11", words[11], (2U << 6) | (8U << 9) | (1U << 14) | (1U << 24));
	expect("framebuffer tiler", words[14], 0x6000U);
	expect("framebuffer pointer", mali_desc_framebuffer_pointer(0x7040ULL, 2), 0x7040U | 1U | (1U << 2));

	/* The tiler heap and context. */
	mali_desc_tiler_heap(words, 0x100000ULL, 0x800000U);
	expect("heap type", words[0], 9U | (2U << 4));
	expect("heap bottom", words[4], 0x100040U);
	expect("heap top", words[6], 0x900000U);
	mali_desc_tiler(words, 1920, 1080, 0x8000ULL, 0x200000ULL, 0x40000U, 0xfe);
	expect("tiler mask", words[2], 0xfeU | (1U << 18));
	expect("tiler heap", words[6], 0x8000U);
	expect("tiler geometry", words[8], 0x40U | 0x200000U);
	expect("primitive", mali_desc_primitive(MALI_DESC_DRAW_TRIANGLES, 2), 8U | (2U << 8) | (1U << 16) | (1U << 17) | (1U << 22));
	expect("clump 16", mali_desc_clump_format(16), 4);

	/* A cube array of two cubes (twelve layers) of RGBA8, 256 square, and the same image for storage. */
	view.dimension = MALI_DESC_DIMENSION_CUBE;
	view.pixel = 187U << 12;
	view.width = 256;
	view.height = 256;
	view.depth = 1;
	view.levels = 1;
	view.array_size = 2;
	view.swizzle = 0x60aU;
	view.storage = false;
	view.planes = 0x5000ULL;
	mali_desc_texture_view(words, &view);
	expect("cube word 0", words[0], 2U | (0U << 4) | ((187U << 12) << 10));
	expect("cube extent", words[1], 255U | (255U << 16));
	expect("cube swizzle and levels", words[2], 0x60aU);
	expect("cube LOD", words[3], 0);
	expect("cube planes", words[4], 0x5000U);
	expect("cube array size", words[6], 1);
	expect("cube depth", words[7], 0);
	view.dimension = MALI_DESC_DIMENSION_2D;
	view.array_size = 12;
	view.storage = true;
	mali_desc_texture_view(words, &view);
	expect("storage word 0", words[0], 2U | (2U << 4) | ((187U << 12) << 10));
	expect("storage swizzle", words[2], 0x688U);
	expect("storage layers", words[6], 11);

	/*
	 * A view of levels 2..5 of a 300 by 17 RGBA8 image: the extent of
	 * level 2 (75 by 4), four levels (3 at 2:16), the maximum LOD three
	 * levels (768 in 8.8 at 3:16); for storage one level and no LOD.
	 */
	view.dimension = MALI_DESC_DIMENSION_2D;
	view.pixel = 187U << 12;
	view.width = mali_surface_extent(300, 2);
	view.height = mali_surface_extent(17, 2);
	view.levels = 4;
	view.array_size = 1;
	view.swizzle = 0x688U;
	view.storage = false;
	view.planes = 0x6000ULL;
	mali_desc_texture_view(words, &view);
	expect("levels extent", words[1], 74U | (3U << 16));
	expect("levels field", words[2], 0x688U | (3U << 16));
	expect("levels LOD", words[3], 768U << 16);
	view.storage = true;
	mali_desc_texture_view(words, &view);
	expect("levels storage field", words[2], 0x688U);
	expect("levels storage LOD", words[3], 0);

	/*
	 * The plane of level 1 of that image's layer 3 at 0x100000: the
	 * layer's start (3 times the 27840-byte layer) and the level's
	 * offset (20672), its 640-byte rows, its 5120 bytes, 150 by 8.
	 */
	{
		struct mali_surface_level levels[MALI_SURFACE_LEVELS];
		uint64_t layer_bytes;
		uint64_t address;

		layer_bytes = mali_surface_levels(300, 17, 4, 64, 9, levels);
		expect("plane layer bytes", layer_bytes, 27840);
		address = 0x100000ULL + 3U * layer_bytes + levels[1].offset;
		mali_desc_plane(words, 4, address, levels[1].pitch, levels[1].bytes, levels[1].width, levels[1].height);
		expect("level plane word 0", words[0], 10U | (1U << 4) | (2U << 8) | (2U << 24));
		expect("level plane size", words[1], 5120);
		expect("level plane pointer", words[2], 0x100000U + 3U * 27840U + 20672U);
		expect("level plane row", words[4], 640);
		expect("level plane extent", words[7], 149U | (7U << 16));
	}

	/* Thread storage: 256 bytes a thread (16 units, shift 4) at 0x12345000, packed. */
	expect("tls size 16", mali_desc_tls_size(16), 0);
	expect("tls size 17", mali_desc_tls_size(17), 1);
	expect("tls size 256", mali_desc_tls_size(256), 4);
	mali_desc_thread_storage(words, 256, 0x1234567000ULL);
	expect("tls word 0", words[0], 4);
	expect("tls no WLS", words[1], 0x1fU);
	expect("tls base low", words[2], 0x12345670U);
	expect("tls base high and mode", words[3], 0U | (1U << 28));
	mali_desc_thread_storage(words, 0, 0);
	expect("no tls word 0", words[0], 0);
	expect("no tls word 3", words[3], 0);

	/*
	 * Workgroup memory: 300 shared bytes take a 512-byte instance (the
	 * least is 128), eight instances a core (log2 3 at word 1 bits 0..4),
	 * the size scale log2(512) + 1 = 10 at bits 8..12, the base at words
	 * 4..5; the thread local storage words stay as packed.
	 */
	expect("wls bytes none", mali_desc_wls_bytes(0), 0);
	expect("wls bytes least", mali_desc_wls_bytes(1), 128);
	expect("wls bytes 300", mali_desc_wls_bytes(300), 512);
	expect("wls bytes 16384", mali_desc_wls_bytes(16384), 16384);
	mali_desc_thread_storage(words, 256, 0x1234567000ULL);
	mali_desc_workgroup_storage(words, 8, mali_desc_wls_bytes(300), 0x2345678000ULL);
	expect("wls tls kept", words[0], 4);
	expect("wls instances and scale", words[1], 3U | (10U << 8));
	expect("wls tls base kept", words[2], 0x12345670U);
	expect("wls base low", words[4], 0x45678000U);
	expect("wls base high", words[5], 0x23U);
	mali_desc_thread_storage(words, 0, 0);
	mali_desc_workgroup_storage(words, 1, 128, 0x10000ULL);
	expect("wls one instance", words[1], 0U | (8U << 8));
	expect("wls no tls", words[0], 0);

	/* A sampler's compare at word 1 bits 13..15, the sides exchanged: less or equal (3) is greater or equal (6). */
	expect("compare lequal", mali_desc_sampler_compare(3), 6U << 13);
	expect("compare greater", mali_desc_sampler_compare(4), 1U << 13);
	expect("compare equal", mali_desc_sampler_compare(2), 2U << 13);

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
	checks++;
	if (got != want) {
		printf("FAIL %s: got %llx want %llx\n", name, (unsigned long long)got, (unsigned long long)want);
		failures++;
	}
}
