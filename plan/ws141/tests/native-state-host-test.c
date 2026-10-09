/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native records are compared with the pinned XML; the pixel image is decoded by a separate inverse block walker. */
#include <assert.h>
#include <fenv.h>
#include <stdio.h>
#include <string.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/native-state.h"
#include "drivers/gpu/bcm2711/native-viewport.h"

/* Unaligned output reservations retain canaries before and after their exact record lengths. */
static uint8_t record_bytes[80];

/* A synthetic compiler product establishes record fields without claiming actual QPU execution. */
static uint64_t fixture_code = 0x3c003186bb800000ULL;
static struct bcm2711_shader_uniform fixture_uniform;

/* Padded raster rows and native tails are kept for the independent converter oracle. */
static uint8_t raster_bytes[160U * 131U];
static uint8_t tiled_bytes[34816U + 16U];

static void print_bytes(const char *name, const uint8_t *bytes, size_t count);
static void verify_shader(void);
static void verify_attributes(void);
static void verify_textures(void);
static void verify_copy(void);
static void unchanged_record(void);
static void verify_viewports(void);
static void verify_viewport_case(uint32_t width, uint32_t height, uint32_t minimum, uint32_t maximum);

/*
 * Produces complete native byte images and checks atomic refusal and pixel ownership boundaries.
 */
int
main(
	void)
{
	/* Every test exercises production encoders without a GPU, MMIO, uploaded code or queue submission. */
	verify_shader();
	verify_attributes();
	verify_textures();
	verify_copy();
	verify_viewports();

	/* The external XML and inverse-pixel oracles consume these actual encoder outputs. */
	puts("native-state-host-test PASS");
	return 0;
}

/* Emits each complete record or scratch image for an independent schema/layout oracle. */
static void
print_bytes(
	const char *name,
	const uint8_t *bytes,
	size_t count)
{
	size_t index;

	/* Exact byte strings expose every field and padding byte, including nonzero native address relocations. */
	printf("native-image %s ", name);
	for (index = 0; index < count; index++)
		printf("%02x", bytes[index]);
	putchar('\n');
}

/* Checks shared VPM sizing, stage threading flags and all uploaded stream addresses. */
static void
verify_shader(
	void)
{
	struct bcm2711_native_shader shader;
	struct bcm2711_shader_binary programs[3];
	uint32_t stage;
	int error;

	/* Independent retained native intervals are synthetic here; production owners must establish actual backing. */
	memset(&shader, 0, sizeof(shader));
	memset(programs, 0, sizeof(programs));
	for (stage = 0; stage < 3; stage++) {
		/* Every slot has a matching compiler stage and finite native code/uniform representation. */
		programs[stage].stage = (enum bcm2711_shader_stage)stage;
		programs[stage].threads = 2;
		programs[stage].code = &fixture_code;
		programs[stage].code_count = 1;
		programs[stage].uniforms = &fixture_uniform;
		programs[stage].uniform_count = 1;
		shader.programs[stage].binary = &programs[stage];
		shader.programs[stage].code = 0x12345000U + stage * 4096U;
		shader.programs[stage].uniforms = 0x56789000U + stage * 4096U;
	}

	/* Large finite interfaces exercise five-sector vertex output while using the conservative two-batch cache. */
	programs[0].input_count = 32;
	programs[0].vpm_output_words = 6;
	programs[0].starts_final = 1;
	programs[1].input_count = 32;
	programs[1].varying_count = 32;
	programs[1].vpm_output_words = 36;
	programs[1].starts_final = 1;
	programs[2].input_count = 32;
	shader.defaults = 0xabcde000U;
	shader.vpm_bytes = 16384;
	memset(record_bytes, 0xa5, sizeof(record_bytes));
	error = bcm2711_native_shader_encode(&shader, record_bytes + 1, 36);
	assert(error == 0);
	assert(record_bytes[0] == 0xa5 && record_bytes[37] == 0xa5);
	print_bytes("shader", record_bytes + 1, 36);

	/* Insufficient VPM never emits a record claiming more resident batches than the hardware can hold. */
	memset(record_bytes, 0xa5, sizeof(record_bytes));
	shader.vpm_bytes = 8192;
	error = bcm2711_native_shader_encode(&shader, record_bytes + 1, 36);
	assert(error == ENOTSUP);
	unchanged_record();
	shader.vpm_bytes = 16384;

	/* Code alignment, stage identity and native address wrap are checked before any output byte is replaced. */
	shader.programs[1].code++;
	error = bcm2711_native_shader_encode(&shader, record_bytes + 1, 36);
	assert(error == EINVAL);
	unchanged_record();
	shader.programs[1].code--;
	programs[2].stage = BCM2711_SHADER_VERTEX;
	error = bcm2711_native_shader_encode(&shader, record_bytes + 1, 36);
	assert(error == EINVAL);
	unchanged_record();
	programs[2].stage = BCM2711_SHADER_FRAGMENT;
	shader.programs[0].code = 0xfffffff8U;
	programs[0].code_count = 2;
	error = bcm2711_native_shader_encode(&shader, record_bytes + 1, 36);
	assert(error == EINVAL);
	unchanged_record();
	shader.programs[0].code = 0x12345000U;
	programs[0].code_count = 1;

	/* Short reservations and unsupported fragment threading cannot partially publish a shader state. */
	error = bcm2711_native_shader_encode(&shader, record_bytes + 1, 35);
	assert(error == ENOSPC);
	unchanged_record();
	programs[2].starts_final = 1;
	error = bcm2711_native_shader_encode(&shader, record_bytes + 1, 36);
	assert(error == ENOTSUP);
	unchanged_record();
}

/* Checks exact float-vector widths, per-stage FIFO consumption and finite fetch bounds. */
static void
verify_attributes(
	void)
{
	struct bcm2711_native_attribute attribute;
	uint32_t components;
	char name[24];
	int error;

	/* Every admitted format exposes its native vector-size representation, including four encoded as zero. */
	memset(&attribute, 0, sizeof(attribute));
	attribute.address = 0x98765430U;
	attribute.stride = 2048;
	attribute.maximum_index = 0x12345678U;
	for (components = 1; components <= 4; components++) {
		/* Both stages consume only the leading scalar components the compiled interface declares. */
		attribute.components = components;
		attribute.coordinate_values = components;
		attribute.vertex_values = components - 1;
		memset(record_bytes, 0xa5, sizeof(record_bytes));
		error = bcm2711_native_attribute_encode(&attribute, record_bytes + 1, 16);
		assert(error == 0);
		assert(record_bytes[0] == 0xa5 && record_bytes[17] == 0xa5);
		snprintf(name, sizeof(name), "attribute%u", components);
		print_bytes(name, record_bytes + 1, 16);
	}

	/* A shader cannot consume values beyond its actual float format or from an unaligned address. */
	memset(record_bytes, 0xa5, sizeof(record_bytes));
	attribute.vertex_values = 5;
	error = bcm2711_native_attribute_encode(&attribute, record_bytes + 1, 16);
	assert(error == EINVAL);
	unchanged_record();
	attribute.vertex_values = 4;
	attribute.address++;
	error = bcm2711_native_attribute_encode(&attribute, record_bytes + 1, 16);
	assert(error == EINVAL);
	unchanged_record();
}

/* Checks strict non-XOR UIF descriptors and nearest/linear wrap/filter records. */
static void
verify_textures(
	void)
{
	struct bcm2711_native_texture texture;
	struct bcm2711_native_sampler sampler;
	uint32_t bytes;
	int error;

	/* A tiny image still uses strict UIF rather than automatic small-image tiling. */
	memset(&texture, 0, sizeof(texture));
	texture.address = 0x23456000U;
	texture.width = 1;
	texture.height = 1;
	error = bcm2711_native_texture_size(1, 1, &bytes);
	assert(error == 0 && bytes == 1024);
	texture.bytes = bytes;
	memset(record_bytes, 0xa5, sizeof(record_bytes));
	error = bcm2711_native_texture_encode(&texture, record_bytes + 1, 24);
	assert(error == 0);
	assert(record_bytes[0] == 0xa5 && record_bytes[25] == 0xa5);
	print_bytes("texture-rgba", record_bytes + 1, 24);

	/* A tall BGRA image crosses both the four-block column and the optional-XOR row boundary. */
	texture.width = 37;
	texture.height = 131;
	texture.swap_red_blue = 1;
	error = bcm2711_native_texture_size(37, 131, &bytes);
	assert(error == 0 && bytes == 34816);
	texture.bytes = bytes;
	error = bcm2711_native_texture_encode(&texture, record_bytes + 1, 24);
	assert(error == 0);
	print_bytes("texture-bgra", record_bytes + 1, 24);

	/* Byte-short padded storage, wrapping GPU intervals and base-control alias bits remain unpublished. */
	memset(record_bytes, 0xa5, sizeof(record_bytes));
	texture.bytes--;
	error = bcm2711_native_texture_encode(&texture, record_bytes + 1, 24);
	assert(error == EINVAL);
	unchanged_record();
	texture.bytes++;
	texture.address = 0xffffffc0U;
	error = bcm2711_native_texture_encode(&texture, record_bytes + 1, 24);
	assert(error == EINVAL);
	unchanged_record();
	texture.address = 0x23456001U;
	error = bcm2711_native_texture_encode(&texture, record_bytes + 1, 24);
	assert(error == EINVAL);
	unchanged_record();

	/* Mixed nearest/linear and mirror/clamp modes expose independent hardware filter and wrap fields. */
	memset(&sampler, 0, sizeof(sampler));
	sampler.nearest_mag = 1;
	sampler.wrap_u = 2;
	sampler.wrap_v = 1;
	error = bcm2711_native_sampler_encode(&sampler, record_bytes + 1, 24);
	assert(error == 0);
	print_bytes("sampler", record_bytes + 1, 24);

	/* Unsupported wrap modes and short output storage leave whole reservations unchanged. */
	memset(record_bytes, 0xa5, sizeof(record_bytes));
	sampler.wrap_v = 3;
	error = bcm2711_native_sampler_encode(&sampler, record_bytes + 1, 24);
	assert(error == ENOTSUP);
	unchanged_record();
	sampler.wrap_v = 1;
	error = bcm2711_native_sampler_encode(&sampler, record_bytes + 1, 23);
	assert(error == ENOSPC);
	unchanged_record();
}

/* Checks complete native scratch conversion, raster padding preservation and atomic interval refusals. */
static void
verify_copy(
	void)
{
	uint32_t x;
	uint32_t y;
	uint32_t offset;
	uint32_t index;
	int error;

	/* Every visible word has an independent coordinate label; row padding carries unrelated canary data. */
	memset(raster_bytes, 0x5a, sizeof(raster_bytes));
	for (y = 0; y < 131; y++) {
		/* Byte-wise labels make the oracle independent of the host's native integer representation. */
		for (x = 0; x < 37; x++) {
			/* One word encodes its exact source x and y without any tiling arithmetic. */
			offset = y * 160U + x * 4U;
			raster_bytes[offset] = (uint8_t)x;
			raster_bytes[offset + 1] = 0;
			raster_bytes[offset + 2] = (uint8_t)y;
			raster_bytes[offset + 3] = 0;
		}
	}

	/* Failed input and output extents are refused before zero padding or visible pixel writes. */
	memset(tiled_bytes, 0xa5, sizeof(tiled_bytes));
	error = bcm2711_native_texture_copy(37, 131, raster_bytes, 160, 20947, tiled_bytes, 34816);
	assert(error == EINVAL);
	error = bcm2711_native_texture_copy(37, 131, raster_bytes, 160, sizeof(raster_bytes), tiled_bytes, 34815);
	assert(error == ENOSPC);
	for (index = 0; index < sizeof(tiled_bytes); index++)
		assert(tiled_bytes[index] == 0xa5);

	/* Overlapping representations cannot erase source words through destination zeroing. */
	error = bcm2711_native_texture_copy(1, 1, tiled_bytes, 4, 4, tiled_bytes + 1, 1024);
	assert(error == EINVAL);
	for (index = 0; index < sizeof(tiled_bytes); index++)
		assert(tiled_bytes[index] == 0xa5);

	/* The successful image contains only visible labeled pixels and deterministically zero native padding. */
	error = bcm2711_native_texture_copy(37, 131, raster_bytes, 160, sizeof(raster_bytes), tiled_bytes, 34816);
	assert(error == 0);
	for (index = 34816; index < sizeof(tiled_bytes); index++)
		assert(tiled_bytes[index] == 0xa5);
	print_bytes("tiled", tiled_bytes, 34816);

	/* Raster output remains byte-for-byte visible to the independent oracle, including its untouched row padding. */
	print_bytes("raster", raster_bytes, sizeof(raster_bytes));
}

/* Confirms complete reservation canaries after every deliberately refused record. */
static void
unchanged_record(
	void)
{
	uint32_t index;

	/* Even bytes outside the nominal record capacity retain the original caller storage contents. */
	for (index = 0; index < sizeof(record_bytes); index++)
		assert(record_bytes[index] == 0xa5);
}

/* Compares bounded native viewport arithmetic with the host's independent nearest-even IEEE operations. */
static void
verify_viewports(
	void)
{
	static const uint32_t endpoints[16] = {
	    0, 0x80000000U, 1, 2, 0x003fffffU, 0x007fffffU, 0x00800000U, 0x00800001U,
	    0x3e7fffffU, 0x3e800000U, 0x3effffffU, 0x3f000000U, 0x3f000001U, 0x3f7ffffeU, 0x3f7fffffU, 0x3f800000U};
	struct bcm2711_native_viewport viewport;
	struct bcm2711_native_viewport saved;
	uint32_t words[6];
	uint32_t left;
	uint32_t right;
	uint32_t width;
	uint32_t height;
	uint32_t minimum;
	uint32_t maximum;
	uint32_t seed;
	uint32_t index;
	int error;
	int same;

	/* Host rounding is explicitly set rather than inheriting an unspecified floating-point environment. */
	error = fesetround(FE_TONEAREST);
	assert(error == 0 && sizeof(float) == sizeof(uint32_t));

	/* The complete boundary grid covers cancellation, reversed ranges, signed zero, subnormal transitions and tie-adjacent normals. */
	for (left = 0; left < 16; left++) {
		/* Widths also exercise positive subnormal normalization and exact ordinary scales. */
		width = endpoints[left] & 0x7fffffffU;
		if (width == 0)
			width = 1;
		for (right = 0; right < 16; right++) {
			/* One ordinary maximal dimension checks the largest advertised native XY scale. */
			verify_viewport_case(width, 0x45800000U, endpoints[left], endpoints[right]);
		}
	}

	/* A fixed finite sample crosses all admitted exponent distances without turning this check into a stress run. */
	seed = 0x31415926U;
	for (index = 0; index < 1024; index++) {
		/* Independent generated input words include exact IEEE magnitudes rather than pre-rounded expected arithmetic. */
		seed = seed * 1664525U + 1013904223U;
		width = seed % 0x45800000U + 1U;
		seed = seed * 1664525U + 1013904223U;
		height = seed % 0x45800000U + 1U;
		seed = seed * 1664525U + 1013904223U;
		minimum = seed % 0x3f800001U;
		seed = seed * 1664525U + 1013904223U;
		maximum = seed % 0x3f800001U;
		verify_viewport_case(width, height, minimum, maximum);
	}

	/* Invalid finite-interface words leave the whole output struct unchanged. */
	memset(&viewport, 0xa5, sizeof(viewport));
	saved = viewport;
	memset(words, 0, sizeof(words));
	words[2] = 0x3f800000U;
	words[3] = 0x3f800000U;
	words[5] = 0x3f800000U;
	words[4] = 0xbf800000U;
	error = bcm2711_native_viewport_prepare(words, &viewport);
	assert(error == EINVAL);
	same = memcmp(&viewport, &saved, sizeof(viewport));
	assert(same == 0);
	words[4] = 0;
	words[2] = 0x7f800000U;
	error = bcm2711_native_viewport_prepare(words, &viewport);
	assert(error == EINVAL);
	same = memcmp(&viewport, &saved, sizeof(viewport));
	assert(same == 0);

	/* All once-rounded integer results matched independent host arithmetic for the fixed finite sample. */
	puts("native-viewport-check PASS (256 boundary pairs and 1024 seeded finite pairs)");
}

/* Checks one raw finite viewport using ordinary host IEEE multiplication and subtraction as the independent oracle. */
static void
verify_viewport_case(
	uint32_t width,
	uint32_t height,
	uint32_t minimum,
	uint32_t maximum)
{
	struct bcm2711_native_viewport viewport;
	struct bcm2711_native_viewport expected;
	uint32_t words[6];
	float dimension;
	float minimum_number;
	float maximum_number;
	float copied;
	volatile float calculated;
	int error;
	int same;

	/* Native inputs preserve signs and complete bits, including either zero endpoint representation. */
	memset(words, 0, sizeof(words));
	words[2] = width;
	words[3] = height;
	words[4] = minimum;
	words[5] = maximum;
	error = bcm2711_native_viewport_prepare(words, &viewport);
	assert(error == 0);

	/* Host multiplication rounds to an actual stored float word before its bits are read. */
	memcpy(&dimension, &width, sizeof(dimension));
	calculated = dimension * 128.0f;
	copied = calculated;
	memcpy(&expected.x_scale, &copied, sizeof(copied));
	memcpy(&dimension, &height, sizeof(dimension));
	calculated = dimension * 128.0f;
	copied = calculated;
	memcpy(&expected.y_scale, &copied, sizeof(copied));

	/* Host subtraction independently supplies reversed-range, cancellation, subnormal and signed-zero behavior. */
	memcpy(&minimum_number, &minimum, sizeof(minimum_number));
	memcpy(&maximum_number, &maximum, sizeof(maximum_number));
	calculated = maximum_number - minimum_number;
	copied = calculated;
	memcpy(&expected.depth_scale, &copied, sizeof(copied));
	expected.depth_offset = minimum;
	same = memcmp(&viewport, &expected, sizeof(viewport));

	/* A disagreement reports exact input/output words without treating numerical proximity as a match. */
	if (same != 0) {
		printf("viewport mismatch %08x %08x %08x %08x: native %08x %08x %08x %08x expected %08x %08x %08x %08x\n", width, height, minimum, maximum, viewport.x_scale, viewport.y_scale, viewport.depth_scale, viewport.depth_offset, expected.x_scale, expected.y_scale, expected.depth_scale, expected.depth_offset);
		assert(same == 0);
	}
}
