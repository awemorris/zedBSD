/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native records are compared with the pinned XML; the pixel image is decoded by a separate inverse block walker. */
#include <assert.h>
#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/native-state.h"
#include "drivers/gpu/bcm2711/native-viewport.h"
#include "drivers/gpu/bcm2711/native-bin.h"

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
static void verify_bins(void);
static void verify_clippers(void);
static void verify_clipper_case(uint32_t x, uint32_t width, uint32_t minimum, uint32_t maximum);
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
	verify_clippers();
	verify_bins();

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

/* Checks integer clipper preparation against independently evaluated host IEEE centres, fixed rounding and guarded depth arithmetic. */
static void
verify_clippers(
	void)
{
	uint32_t coordinates[12];
	uint32_t dimensions[8];
	uint32_t x;
	uint32_t width;
	uint32_t seed;
	uint32_t index;
	uint32_t left;
	uint32_t right;

	/* Coarse transitions, half-unit rounding, cancellation and tiny signed centres exercise distinct packet representations. */
	coordinates[0] = 0;
	coordinates[1] = 0x80000000U;
	coordinates[2] = 1;
	coordinates[3] = 0x80000001U;
	coordinates[4] = 0x3b000000U;
	coordinates[5] = 0xbb000000U;
	coordinates[6] = 0x42800000U;
	coordinates[7] = 0xc2800000U;
	coordinates[8] = 0xc2800001U;
	coordinates[9] = 0xc27fffffU;
	coordinates[10] = 0x45800000U;
	coordinates[11] = 0xc5800000U;
	dimensions[0] = 1;
	dimensions[1] = 2;
	dimensions[2] = 3;
	dimensions[3] = 0x00ffffffU;
	dimensions[4] = 0x3b800000U;
	dimensions[5] = 0x3f800000U;
	dimensions[6] = 0x43000000U;
	dimensions[7] = 0x45800000U;
	for (left = 0; left < 12; left++) {
		for (right = 0; right < 8; right++) {
			verify_clipper_case(coordinates[left], dimensions[right], 0, 0x3f800000U);
			verify_clipper_case(coordinates[left], dimensions[right], 0x3f800000U, 0x3f800000U);
			verify_clipper_case(coordinates[left], dimensions[right], 0x39800000U, 0);
		}
	}

	/* Finite deterministic centres cover unlike exponent addition/subtraction, without an unbounded fuzz campaign. */
	seed = 0x6e617469U;
	for (index = 0; index < 512; index++) {
		seed = seed * 1664525U + 1013904223U;
		x = seed % 0x45800001U;
		if ((seed & 1U) != 0)
			x |= 0x80000000U;
		seed = seed * 1664525U + 1013904223U;
		width = 1U + seed % 0x45800000U;
		verify_clipper_case(x, width, 0x3f000000U, 0x3f000001U);
	}

	/* Succeeded: offset words and guarded depth fields agree bit-for-bit with the independent host oracle. */
	puts("native-clipper-check PASS (288 boundary and 512 seeded cases)");
	return;
}

/* Compares one finite clipper state to ordinary host IEEE arithmetic and numerical coarse/fine packing. */
static void
verify_clipper_case(
	uint32_t x,
	uint32_t width,
	uint32_t minimum,
	uint32_t maximum)
{
	struct bcm2711_native_viewport_clip clip;
	uint32_t words[6];
	uint32_t expected_offset;
	uint32_t expected_scale;
	uint32_t expected_minimum;
	uint32_t expected_maximum;
	uint32_t temporary;
	volatile float coordinate;
	volatile float dimension;
	volatile float half;
	volatile float centre;
	volatile float edge_low;
	volatile float edge_high;
	volatile float low;
	volatile float high;
	volatile float scale;
	volatile float end;
	float adjustment;
	float absolute_scale;
	uint32_t fine;
	int32_t coarse;
	int error;

	/* X and Y use identical inputs so both independently encoded native words must agree. */
	words[0] = x;
	words[1] = x;
	words[2] = width;
	words[3] = width;
	words[4] = minimum;
	words[5] = maximum;
	error = bcm2711_native_viewport_clip_prepare(words, &clip);
	assert(error == 0);
	memcpy((void *)&coordinate, &x, 4);
	memcpy((void *)&dimension, &width, 4);
	half = dimension * 0.5f;
	centre = half + coordinate;
	edge_low = centre - half;
	edge_high = centre + half;
	assert(clip.bounds[0] == (int32_t)edge_low && clip.bounds[1] == (int32_t)edge_low);
	assert(clip.bounds[2] == (int32_t)edge_high && clip.bounds[3] == (int32_t)edge_high);
	coarse = 0;
	if (centre < 0) {
		/* Coarse block count is a mathematical ceiling; double avoids underflow when a tiny negative float is divided by sixty-four. */
		adjustment = (float)ceil(fabs((double)centre) / 64.0);
		coarse = -(int32_t)adjustment;
		centre = centre + adjustment * 64.0f;
	}

	/* Native unsigned fixed fields round halfway positive values upward. */
	fine = (uint32_t)lroundf(centre * 256.0f);
	expected_offset = (((uint32_t)coarse & 1023U) << 22) | fine;
	if (clip.x_offset != expected_offset || clip.y_offset != expected_offset)
		fprintf(stderr, "clipper mismatch x=%08x width=%08x actual=%08x expected=%08x\n", x, width, clip.x_offset, expected_offset);
	assert(clip.x_offset == expected_offset && clip.y_offset == expected_offset);
	memcpy((void *)&low, &minimum, 4);
	memcpy((void *)&high, &maximum, 4);
	scale = high - low;
	absolute_scale = fabsf(scale);
	if (absolute_scale < 0.0005f) {
		if (scale < 0)
			scale = -0.0005f;
		else
			scale = 0.0005f;
	}

	/* Plane endpoints use the same guarded transform, including sign reversal. */
	end = low + scale;
	memcpy(&expected_scale, (const void *)&scale, 4);
	memcpy(&expected_minimum, (const void *)&low, 4);
	memcpy(&expected_maximum, (const void *)&end, 4);
	if (scale < 0) {
		temporary = expected_minimum;
		expected_minimum = expected_maximum;
		expected_maximum = temporary;
	}

	/* Every guarded endpoint and its scale must match the independently evaluated host values. */
	assert(clip.depth_scale == expected_scale && clip.depth_offset == minimum);
	assert(clip.minimum == expected_minimum && clip.maximum == expected_maximum);

	/* Succeeded: all native fields match the independent host operations for this exact finite case. */
	return;
}

/* Emits a complete synthetic native bin sequence for the pinned XML oracle and checks late atomic refusal. */
static void
verify_bins(
	void)
{
	struct bcm2711_native_bin state;
	uint32_t viewport[6];
	uint8_t bytes[BCM2711_NATIVE_BIN_BYTES + 4U];
	uint8_t saved[BCM2711_NATIVE_BIN_BYTES + 4U];
	uint32_t index;
	int error;
	int same;

	/* Numerical native addresses and state are synthetic; an enclosing real job must own and clean their complete intervals. */
	memset(&state, 0, sizeof(state));
	viewport[0] = 0xc2000000U;
	viewport[1] = 0xc2800000U;
	viewport[2] = 0x41800000U;
	viewport[3] = 0x41000000U;
	viewport[4] = 0;
	viewport[5] = 0x3f800000U;
	error = bcm2711_native_viewport_prepare(viewport, &state.viewport);
	assert(error == 0);
	error = bcm2711_native_viewport_clip_prepare(viewport, &state.clipper);
	assert(error == 0);
	state.window[0] = 3;
	state.window[1] = 4;
	state.window[2] = 5;
	state.window[3] = 6;
	state.shader = 0x12345000U;
	state.attributes = 3;
	state.vertices = 9;
	state.reverse = 1;
	state.clockwise = 1;
	state.flat = 0x81000001U;
	state.noperspective = 0x40000002U;
	memset(bytes, 0xa5, sizeof(bytes));
	error = bcm2711_native_bin_encode(&state, bytes, sizeof(bytes));
	assert(error == 0);
	print_bytes("bin", bytes, BCM2711_NATIVE_BIN_BYTES);
	for (index = BCM2711_NATIVE_BIN_BYTES; index < sizeof(bytes); index++)
		assert(bytes[index] == 0xa5);
	memcpy(saved, bytes, sizeof(saved));

	/* Short capacity, mandatory dummy fetch omission and a wrapped complete shader-record span leave all previous bytes unchanged. */
	error = bcm2711_native_bin_encode(&state, bytes, BCM2711_NATIVE_BIN_BYTES - 1U);
	assert(error == ENOSPC);
	same = memcmp(bytes, saved, sizeof(saved));
	assert(same == 0);
	state.attributes = 0;
	error = bcm2711_native_bin_encode(&state, bytes, sizeof(bytes));
	assert(error == EINVAL);
	same = memcmp(bytes, saved, sizeof(saved));
	assert(same == 0);
	state.attributes = 16;
	state.shader = 0xffffffc0U;
	error = bcm2711_native_bin_encode(&state, bytes, sizeof(bytes));
	assert(error == EINVAL);
	same = memcmp(bytes, saved, sizeof(saved));
	assert(same == 0);

	/* Succeeded: the independent XML oracle can compare every complete packet byte, relocation, fixed value and varying flag. */
	return;
}
