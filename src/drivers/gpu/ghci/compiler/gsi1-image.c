/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GSI1 compiler's storage images, storage texel buffers and input
 * attachments: reads and writes through memory, with the format converted
 * by the program.
 *
 * GSI1 has no image store, and the texture slots are the sampled images',
 * so these are buffers to the program: each one has a resource table
 * entry, and the program reads its descriptor's fields, computes the
 * texel's address and moves its words with lw and sw.  A storage image's
 * and an input attachment's entry is a texture descriptor (the address,
 * width and height, layers, row and layer bytes, samples; the layers of
 * an array, the slices of a 3D image, the faces of a cube), a storage
 * texel buffer's a buffer descriptor (its elements are its bytes over its
 * stride); the texel's bytes come from the format.  A texel outside the
 * image reads zero and is not written.  An input attachment is read at the
 * fragment's own pixel (FragCoord's integer part plus the offset) and, if
 * it is multisampled, at the sample asked for, in the multisampled layout:
 * pixel x's samples are consecutive.
 *
 * The formats are those Vulkan 1.0 requires for storage images without
 * shaderStorageImageExtendedFormats (read and written), and for input
 * attachments the colour attachment and depth formats (read).  A
 * normalized value is converted with the reciprocal and one correction
 * step of a fused multiply-add, so that c / (2^b - 1) is rounded as the
 * division would be.
 */

#include "gsi1-private.h"

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

/* The number types of a format's components. */
#define GSI1_NUMBER_UNORM		1U
#define GSI1_NUMBER_SNORM		2U
#define GSI1_NUMBER_UINT		3U
#define GSI1_NUMBER_SINT		4U
#define GSI1_NUMBER_SFLOAT		5U
#define GSI1_NUMBER_SRGB		6U

/* The most words a texel takes (RGBA32). */
#define GSI1_TEXEL_WORDS		4U

/*
 * The float constants of the conversions: 2^b - 1 and its reciprocal for
 * the normalized widths, 2^(b-1) - 1 and its reciprocal for the signed
 * ones, and the sRGB transfer function's.
 *
 * Values referenced: IEEE 754-2019 (the binary32 encodings, rounded to
 * nearest, of the values named); Khronos Data Format Specification 1.3,
 * section 13.3 "sRGB transfer functions" (the EOTF: x / 12.92 up to
 * 0.04045, ((x + 0.055) / 1.055) ^ 2.4 above).
 */
#define GSI1_SRGB_LINEAR_SCALE		0x3d9e8391U	/* 1 / 12.92 */
#define GSI1_SRGB_SCALE			0x3f72a76eU	/* 1 / 1.055 */
#define GSI1_SRGB_OFFSET		0x3d558919U	/* 0.055 / 1.055 */
#define GSI1_SRGB_EXPONENT		0x4019999aU	/* 2.4 */
#define GSI1_SRGB_THRESHOLD		0x3d25aee6U	/* 0.04045 */

/*
 * One format: its VkFormat, its texel's bytes, its components' number
 * type, and each component's (R, G, B, A) width and bit position in the
 * texel (width 0: the format lacks it).  An sRGB format's alpha is UNORM.
 */
struct gsi1_format {
	uint32_t format;
	uint8_t bytes;
	uint8_t number;
	uint8_t bits[4];
	uint8_t shift[4];
	uint8_t writable;
};

/*
 * The normalized widths' divisors 2^b - 1 and their reciprocals, as fp32
 * bits, by the width.
 */
struct gsi1_scale {
	uint32_t bits;
	uint32_t divisor;
	uint32_t reciprocal;
};

/*
 * The formats a storage image or an input attachment may have.
 *
 * Values referenced: the Vulkan specification, VkFormat (the values) and
 * "Compatible Formats" / the packed formats' bit layouts (each component's
 * width and its bits, the first-named in the high bits of a packed
 * format, the first-named in the low bytes of an unpacked one); the
 * depth/stencil formats' depth aspect in the low bits.
 */
static const struct gsi1_format gsi1_formats[] = {
	{ 4U, 2U, GSI1_NUMBER_UNORM, { 5U, 6U, 5U, 0U }, { 11U, 5U, 0U, 0U }, 0U },
	{ 8U, 2U, GSI1_NUMBER_UNORM, { 5U, 5U, 5U, 1U }, { 10U, 5U, 0U, 15U }, 0U },
	{ 9U, 1U, GSI1_NUMBER_UNORM, { 8U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U }, 0U },
	{ 13U, 1U, GSI1_NUMBER_UINT, { 8U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U }, 0U },
	{ 14U, 1U, GSI1_NUMBER_SINT, { 8U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U }, 0U },
	{ 16U, 2U, GSI1_NUMBER_UNORM, { 8U, 8U, 0U, 0U }, { 0U, 8U, 0U, 0U }, 0U },
	{ 20U, 2U, GSI1_NUMBER_UINT, { 8U, 8U, 0U, 0U }, { 0U, 8U, 0U, 0U }, 0U },
	{ 21U, 2U, GSI1_NUMBER_SINT, { 8U, 8U, 0U, 0U }, { 0U, 8U, 0U, 0U }, 0U },
	{ 37U, 4U, GSI1_NUMBER_UNORM, { 8U, 8U, 8U, 8U }, { 0U, 8U, 16U, 24U }, 1U },
	{ 38U, 4U, GSI1_NUMBER_SNORM, { 8U, 8U, 8U, 8U }, { 0U, 8U, 16U, 24U }, 1U },
	{ 41U, 4U, GSI1_NUMBER_UINT, { 8U, 8U, 8U, 8U }, { 0U, 8U, 16U, 24U }, 1U },
	{ 42U, 4U, GSI1_NUMBER_SINT, { 8U, 8U, 8U, 8U }, { 0U, 8U, 16U, 24U }, 1U },
	{ 43U, 4U, GSI1_NUMBER_SRGB, { 8U, 8U, 8U, 8U }, { 0U, 8U, 16U, 24U }, 0U },
	{ 44U, 4U, GSI1_NUMBER_UNORM, { 8U, 8U, 8U, 8U }, { 16U, 8U, 0U, 24U }, 0U },
	{ 50U, 4U, GSI1_NUMBER_SRGB, { 8U, 8U, 8U, 8U }, { 16U, 8U, 0U, 24U }, 0U },
	{ 51U, 4U, GSI1_NUMBER_UNORM, { 8U, 8U, 8U, 8U }, { 0U, 8U, 16U, 24U }, 0U },
	{ 52U, 4U, GSI1_NUMBER_SNORM, { 8U, 8U, 8U, 8U }, { 0U, 8U, 16U, 24U }, 0U },
	{ 55U, 4U, GSI1_NUMBER_UINT, { 8U, 8U, 8U, 8U }, { 0U, 8U, 16U, 24U }, 0U },
	{ 56U, 4U, GSI1_NUMBER_SINT, { 8U, 8U, 8U, 8U }, { 0U, 8U, 16U, 24U }, 0U },
	{ 57U, 4U, GSI1_NUMBER_SRGB, { 8U, 8U, 8U, 8U }, { 0U, 8U, 16U, 24U }, 0U },
	{ 64U, 4U, GSI1_NUMBER_UNORM, { 10U, 10U, 10U, 2U }, { 0U, 10U, 20U, 30U }, 0U },
	{ 68U, 4U, GSI1_NUMBER_UINT, { 10U, 10U, 10U, 2U }, { 0U, 10U, 20U, 30U }, 0U },
	{ 74U, 2U, GSI1_NUMBER_UINT, { 16U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U }, 0U },
	{ 75U, 2U, GSI1_NUMBER_SINT, { 16U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U }, 0U },
	{ 76U, 2U, GSI1_NUMBER_SFLOAT, { 16U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U }, 0U },
	{ 81U, 4U, GSI1_NUMBER_UINT, { 16U, 16U, 0U, 0U }, { 0U, 16U, 0U, 0U }, 0U },
	{ 82U, 4U, GSI1_NUMBER_SINT, { 16U, 16U, 0U, 0U }, { 0U, 16U, 0U, 0U }, 0U },
	{ 83U, 4U, GSI1_NUMBER_SFLOAT, { 16U, 16U, 0U, 0U }, { 0U, 16U, 0U, 0U }, 0U },
	{ 95U, 8U, GSI1_NUMBER_UINT, { 16U, 16U, 16U, 16U }, { 0U, 16U, 32U, 48U }, 1U },
	{ 96U, 8U, GSI1_NUMBER_SINT, { 16U, 16U, 16U, 16U }, { 0U, 16U, 32U, 48U }, 1U },
	{ 97U, 8U, GSI1_NUMBER_SFLOAT, { 16U, 16U, 16U, 16U }, { 0U, 16U, 32U, 48U }, 1U },
	{ 98U, 4U, GSI1_NUMBER_UINT, { 32U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U }, 1U },
	{ 99U, 4U, GSI1_NUMBER_SINT, { 32U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U }, 1U },
	{ 100U, 4U, GSI1_NUMBER_SFLOAT, { 32U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U }, 1U },
	{ 101U, 8U, GSI1_NUMBER_UINT, { 32U, 32U, 0U, 0U }, { 0U, 32U, 0U, 0U }, 1U },
	{ 102U, 8U, GSI1_NUMBER_SINT, { 32U, 32U, 0U, 0U }, { 0U, 32U, 0U, 0U }, 1U },
	{ 103U, 8U, GSI1_NUMBER_SFLOAT, { 32U, 32U, 0U, 0U }, { 0U, 32U, 0U, 0U }, 1U },
	{ 107U, 16U, GSI1_NUMBER_UINT, { 32U, 32U, 32U, 32U }, { 0U, 32U, 64U, 96U }, 1U },
	{ 108U, 16U, GSI1_NUMBER_SINT, { 32U, 32U, 32U, 32U }, { 0U, 32U, 64U, 96U }, 1U },
	{ 109U, 16U, GSI1_NUMBER_SFLOAT, { 32U, 32U, 32U, 32U }, { 0U, 32U, 64U, 96U }, 1U },
	{ 124U, 2U, GSI1_NUMBER_UNORM, { 16U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U }, 0U },
	{ 125U, 4U, GSI1_NUMBER_UNORM, { 24U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U }, 0U },
	{ 126U, 4U, GSI1_NUMBER_SFLOAT, { 32U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U }, 0U },
	{ 129U, 4U, GSI1_NUMBER_UNORM, { 24U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U }, 0U }
};

/*
 * The normalized widths' divisors and reciprocals.
 *
 * Values referenced: IEEE 754-2019 (the binary32 encodings of 2^b - 1 and
 * of 1 / (2^b - 1), rounded to nearest, for the widths listed).
 */
static const struct gsi1_scale gsi1_unorm_scales[] = {
	{ 1U, 0x3f800000U, 0x3f800000U },
	{ 2U, 0x40400000U, 0x3eaaaaabU },
	{ 5U, 0x41f80000U, 0x3d042108U },
	{ 6U, 0x427c0000U, 0x3c820821U },
	{ 8U, 0x437f0000U, 0x3b808081U },
	{ 10U, 0x447fc000U, 0x3a802008U },
	{ 16U, 0x477fff00U, 0x37800080U },
	{ 24U, 0x4b7fffffU, 0x33800001U }
};

/*
 * The signed normalized widths' divisors 2^(b-1) - 1 and reciprocals.
 *
 * Values referenced: IEEE 754-2019 (the binary32 encodings of 127, 1 /
 * 127, 32767 and 1 / 32767, rounded to nearest).
 */
static const struct gsi1_scale gsi1_snorm_scales[] = {
	{ 8U, 0x42fe0000U, 0x3c010204U },
	{ 16U, 0x46fffe00U, 0x38000100U }
};

static const struct gsi1_format *find_format(uint32_t format);
static int find_scale(const struct gsi1_scale *table, uint32_t count, uint32_t bits, const struct gsi1_scale **scale);
static int image_address(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, const struct gsi1_binding *binding, const struct gsi1_format *format, uint32_t *address, uint32_t *inside);
static int attachment_address(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, const struct gsi1_binding *binding, const struct gsi1_format *format, uint32_t *address);
static int bounds_check(struct gsi1_lowering *lowering, uint32_t inside, uint32_t coordinate, uint32_t limit, int first);
static int multiply_add(struct gsi1_lowering *lowering, uint32_t sum, uint32_t factor, uint32_t multiplier, int multiplier_is_constant);
static int load_texel(struct gsi1_lowering *lowering, const struct gsi1_format *format, uint32_t address, uint32_t *words);
static int unpack_component(struct gsi1_lowering *lowering, const struct gsi1_format *format, uint32_t component, const uint32_t *words, uint32_t dst);
static int extract_bits(struct gsi1_lowering *lowering, uint32_t dst, uint32_t word, uint32_t shift, uint32_t bits, int sign);
static int normalized_to_float(struct gsi1_lowering *lowering, uint32_t dst, uint32_t bits, int sign);
static int srgb_to_linear(struct gsi1_lowering *lowering, uint32_t dst);
static int pack_texel(struct gsi1_lowering *lowering, const struct gsi1_format *format, uint32_t first_value, uint32_t *words);
static int pack_component(struct gsi1_lowering *lowering, const struct gsi1_format *format, uint32_t component, uint32_t value, uint32_t dst);
static int constant_register(struct gsi1_lowering *lowering, uint32_t bits, uint32_t *reg);

/* Lowers a read or a write of a storage image, a storage texel buffer or an input attachment. */
int
gsi1_image_lower(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	const struct gsi1_binding *binding;
	const struct gsi1_format *format;
	uint32_t words[GSI1_TEXEL_WORDS];
	uint32_t address;
	uint32_t inside;
	uint32_t predicate;
	uint32_t save;
	uint32_t component;
	uint32_t index;
	uint32_t word_count;
	uint32_t attachment;
	int error;

	/* The image the instruction names. */
	if (instruction->location >= lowering->ir->uniform_count) {
		error = gsi1_refuse(lowering, EINVAL, "an image access to something that is not an image of the program");
		return error;
	}
	binding = &lowering->binding[instruction->location];

	/* An input attachment's format is the render pass's, from the options. */
	if (binding->kind == GSI1_BINDING_INPUT_ATTACHMENT) {
		attachment = lowering->ir->uniforms[instruction->location].input_attachment_index;
		format = NULL;
		if (attachment < GHCI_GSI1_INPUT_ATTACHMENTS)
			format = find_format(lowering->options.input_attachment_format[attachment]);
	} else if (binding->kind == GSI1_BINDING_STORAGE_IMAGE || binding->kind == GSI1_BINDING_TEXEL_BUFFER) {
		format = find_format(binding->format);
	} else {
		error = gsi1_refuse(lowering, EINVAL, "an image access to something that is not an image of the program");
		return error;
	}

	/* The format must be one the compiler converts. */
	if (format == NULL) {
		error = gsi1_refuse(lowering, ENOTSUP, "an image of a format the compiler does not convert");
		return error;
	}

	/* A write needs a format the compiler packs. */
	if (instruction->op == DRV_GPU_IR_IMAGE_WRITE && format->writable == 0U) {
		error = gsi1_refuse(lowering, ENOTSUP, "a write to an image of a format the compiler does not pack");
		return error;
	}

	/* The texel's address, and for a storage image whether it is inside. */
	address = GSI1_NO_OPERAND;
	inside = GSI1_NO_OPERAND;
	if (binding->kind == GSI1_BINDING_INPUT_ATTACHMENT) {
		error = attachment_address(lowering, instruction, binding, format, &address);
	} else {
		error = image_address(lowering, instruction, binding, format, &address, &inside);
	}
	if (error != 0)
		return error;

	/* The predicate and the bounds narrow EXEC for the access. */
	save = GSI1_NO_OPERAND;
	if (instruction->component != 0U || inside != GSI1_NO_OPERAND) {
		error = gsi1_scratch(lowering, &save);
		if (error != 0)
			return error;
		gsi1_emit_exec(lowering, GSI1_EXEC_SAVE, save);

		/* The block's predicate. */
		if (instruction->component != 0U) {
			predicate = instruction->src[2];
			if (instruction->op == DRV_GPU_IR_IMAGE_WRITE)
				predicate = instruction->src[3];
			error = gsi1_source(lowering, predicate, &component);
			if (error != 0)
				return error;
			gsi1_emit_exec(lowering, GSI1_EXEC_AND, component);
		}

		/* The texel inside the image. */
		if (inside != GSI1_NO_OPERAND)
			gsi1_emit_exec(lowering, GSI1_EXEC_AND, inside);
	}

	/* A write: the texel packed into its words, then stored word by word. */
	if (instruction->op == DRV_GPU_IR_IMAGE_WRITE) {
		error = pack_texel(lowering, format, instruction->src[2], words);
		if (error != 0)
			return error;
		word_count = (format->bytes + 3U) / 4U;
		for (index = 0U; index < word_count; index++)
			gsi1_emit(lowering, gsi1_encode_s(GSI1_OP_STORE, GSI1_F3_SW, address, words[index], (int32_t)(4U * index)));
		if (save != GSI1_NO_OPERAND)
			gsi1_emit_exec(lowering, GSI1_EXEC_SET, save);
		return 0;
	}

	/* A read: zero where the texel is outside, then the words and their components. */
	if (inside != GSI1_NO_OPERAND) {
		for (component = 0U; component < 4U; component++)
			gsi1_emit_move(lowering, dst + component, GSI1_ZERO);
	}
	error = load_texel(lowering, format, address, words);
	if (error != 0)
		return error;
	for (component = 0U; component < 4U; component++) {
		error = unpack_component(lowering, format, component, words, dst + component);
		if (error != 0)
			return error;
	}

	/* EXEC comes back. */
	if (save != GSI1_NO_OPERAND)
		gsi1_emit_exec(lowering, GSI1_EXEC_SET, save);

	/* Succeeded: the access is lowered. */
	return 0;
}

/*
 * Computes the address of a 32-bit texel of a storage image or texel
 * buffer an atomic works on (x, y and the layer are the instruction's
 * three consecutive values from src[0]), and the Boolean of its being
 * inside the image.
 */
int
gsi1_image_texel(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	const struct gsi1_binding *binding,
	uint32_t *address,
	uint32_t *inside)
{
	const struct gsi1_format *format;
	int error;

	/* A 32-bit integer format. */
	format = find_format(binding->format);
	if (format == NULL || format->bytes != 4U || format->bits[0] != 32U) {
		error = gsi1_refuse(lowering, ENOTSUP, "an image atomic on a format that is not a 32-bit integer");
		return error;
	}

	/* The texel's address and whether it is inside. */
	error = image_address(lowering, instruction, binding, format, address, inside);
	if (error != 0)
		return error;

	/* Succeeded: the texel's address is in a scratch register. */
	return 0;
}

/* Finds a format's description; NULL for one the compiler does not convert. */
static const struct gsi1_format *
find_format(
	uint32_t format)
{
	uint32_t index;

	/* Each format of the table. */
	for (index = 0U; index < sizeof(gsi1_formats) / sizeof(gsi1_formats[0]); index++) {
		if (gsi1_formats[index].format == format)
			return &gsi1_formats[index];
	}

	/* No such format. */
	return NULL;
}

/* Finds the divisor and reciprocal of a normalized width; EINVAL for one the tables lack. */
static int
find_scale(
	const struct gsi1_scale *table,
	uint32_t count,
	uint32_t bits,
	const struct gsi1_scale **scale)
{
	uint32_t index;

	/* Each width of the table. */
	for (index = 0U; index < count; index++) {
		if (table[index].bits == bits) {
			*scale = &table[index];
			return 0;
		}
	}

	/* No such width. */
	return EINVAL;
}

/*
 * Computes a storage image's (or storage texel buffer's) texel address
 * into a scratch register, and the Boolean of the coordinates being inside
 * it: x below the width (the elements), y below the height, the layer below
 * the layers.  The address of a texel outside is not used.
 */
static int
image_address(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	const struct gsi1_binding *binding,
	const struct gsi1_format *format,
	uint32_t *address,
	uint32_t *inside)
{
	uint32_t x;
	uint32_t y;
	uint32_t layer;
	uint32_t word;
	uint32_t limit;
	uint32_t reg;
	int error;

	/* The coordinates. */
	error = gsi1_source(lowering, instruction->src[0], &x);
	if (error != 0)
		return error;

	/* Finds y. */
	error = gsi1_source(lowering, instruction->src[0] + 1U, &y);
	if (error != 0)
		return error;

	/* Finds the layer. */
	error = gsi1_source(lowering, instruction->src[0] + 2U, &layer);
	if (error != 0)
		return error;

	/* Takes the registers of the Boolean and of the address. */
	error = gsi1_scratch(lowering, inside);
	if (error != 0)
		return error;

	/* Takes the address's. */
	error = gsi1_scratch(lowering, &reg);
	if (error != 0)
		return error;

	/* A texel buffer: x below the elements; the address base + x x bytes. */
	if (binding->kind == GSI1_BINDING_TEXEL_BUFFER) {
		error = gsi1_resource_field(lowering, binding, GSI1_FIELD_ELEMENTS, &limit);
		if (error != 0)
			return error;
		gsi1_emit_alu(lowering, GSI1_F3_SLTU, GSI1_F7_BASE, *inside, x, limit);

		/* The address. */
		error = gsi1_resource_field(lowering, binding, GSI1_FIELD_BASE, &word);
		if (error != 0)
			return error;
		gsi1_emit_move(lowering, reg, word);
		error = multiply_add(lowering, reg, x, format->bytes, 1);
		if (error != 0)
			return error;

		/* Succeeded: the texel buffer's address. */
		*address = reg;
		return 0;
	}

	/* The width and the height from their packed word. */
	error = gsi1_resource_field(lowering, binding, GSI1_FIELD_EXTENT, &word);
	if (error != 0)
		return error;

	/* Takes a register for each limit in turn. */
	error = gsi1_scratch(lowering, &limit);
	if (error != 0)
		return error;

	/* x below the width. */
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SLL, GSI1_F7_BASE, limit, word, 16U));
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SRL, GSI1_F7_BASE, limit, limit, 16U));
	error = bounds_check(lowering, *inside, x, limit, 1);
	if (error != 0)
		return error;

	/* y below the height. */
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SRL, GSI1_F7_BASE, limit, word, 16U));
	error = bounds_check(lowering, *inside, y, limit, 0);
	if (error != 0)
		return error;

	/* The layer below the layers. */
	error = gsi1_resource_field(lowering, binding, GSI1_FIELD_LAYERS, &word);
	if (error != 0)
		return error;
	error = bounds_check(lowering, *inside, layer, word, 0);
	if (error != 0)
		return error;

	/* The address: base + layer x layer bytes + y x row bytes + x x texel bytes. */
	error = gsi1_resource_field(lowering, binding, GSI1_FIELD_BASE, &word);
	if (error != 0)
		return error;
	gsi1_emit_move(lowering, reg, word);

	/* The layer's bytes. */
	error = gsi1_resource_field(lowering, binding, GSI1_FIELD_LAYER_BYTES, &word);
	if (error != 0)
		return error;
	error = multiply_add(lowering, reg, layer, word, 0);
	if (error != 0)
		return error;

	/* The row's bytes. */
	error = gsi1_resource_field(lowering, binding, GSI1_FIELD_ROW_BYTES, &word);
	if (error != 0)
		return error;
	error = multiply_add(lowering, reg, y, word, 0);
	if (error != 0)
		return error;

	/* The texel's bytes. */
	error = multiply_add(lowering, reg, x, format->bytes, 1);
	if (error != 0)
		return error;

	/* Succeeded: the image's address. */
	*address = reg;
	return 0;
}

/*
 * Computes an input attachment's texel address into a scratch register:
 * the pixel is FragCoord's integer part plus the offset, and a
 * multisampled attachment's sample s of pixel x is texel x S + s of its
 * row.
 */
static int
attachment_address(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	const struct gsi1_binding *binding,
	const struct gsi1_format *format,
	uint32_t *address)
{
	uint32_t pixel_x;
	uint32_t pixel_y;
	uint32_t offset;
	uint32_t word;
	uint32_t samples;
	uint32_t reg;
	int error;

	/* Takes the registers of the pixel and of the address. */
	error = gsi1_scratch(lowering, &pixel_x);
	if (error != 0)
		return error;

	/* Takes y's. */
	error = gsi1_scratch(lowering, &pixel_y);
	if (error != 0)
		return error;

	/* Takes the address's. */
	error = gsi1_scratch(lowering, &reg);
	if (error != 0)
		return error;

	/* The pixel: r4 and r5 are x + 0.5 and y + 0.5, so truncation gives x and y. */
	gsi1_emit_float(lowering, GSI1_F7_FCVT_W_S, GSI1_RM_RTZ, pixel_x, lowering->input_reg[GSI1_INPUT_X], 0U, 0U);
	gsi1_emit_float(lowering, GSI1_F7_FCVT_W_S, GSI1_RM_RTZ, pixel_y, lowering->input_reg[GSI1_INPUT_Y], 0U, 0U);

	/* Plus the offset. */
	error = gsi1_source(lowering, instruction->src[0], &offset);
	if (error != 0)
		return error;
	gsi1_emit_alu(lowering, GSI1_F3_ADD, GSI1_F7_BASE, pixel_x, pixel_x, offset);
	error = gsi1_source(lowering, instruction->src[0] + 1U, &offset);
	if (error != 0)
		return error;
	gsi1_emit_alu(lowering, GSI1_F3_ADD, GSI1_F7_BASE, pixel_y, pixel_y, offset);

	/* A multisampled attachment: x S + s. */
	if (binding->samples != 0U) {
		error = gsi1_resource_field(lowering, binding, GSI1_FIELD_SAMPLES, &samples);
		if (error != 0)
			return error;
		gsi1_emit_alu(lowering, GSI1_F3_MUL, GSI1_F7_MULDIV, pixel_x, pixel_x, samples);
		error = gsi1_source(lowering, instruction->src[0] + 2U, &offset);
		if (error != 0)
			return error;
		gsi1_emit_alu(lowering, GSI1_F3_ADD, GSI1_F7_BASE, pixel_x, pixel_x, offset);
	}

	/* The address: base + y x row bytes + x x texel bytes. */
	error = gsi1_resource_field(lowering, binding, GSI1_FIELD_BASE, &word);
	if (error != 0)
		return error;
	gsi1_emit_move(lowering, reg, word);

	/* The row's bytes. */
	error = gsi1_resource_field(lowering, binding, GSI1_FIELD_ROW_BYTES, &word);
	if (error != 0)
		return error;
	error = multiply_add(lowering, reg, pixel_y, word, 0);
	if (error != 0)
		return error;

	/* The texel's bytes. */
	error = multiply_add(lowering, reg, pixel_x, format->bytes, 1);
	if (error != 0)
		return error;

	/* Succeeded: the attachment's address. */
	*address = reg;
	return 0;
}

/*
 * Narrows the Boolean `inside` by coordinate < limit (unsigned, so a
 * negative coordinate is outside); the first check sets it.
 */
static int
bounds_check(
	struct gsi1_lowering *lowering,
	uint32_t inside,
	uint32_t coordinate,
	uint32_t limit,
	int first)
{
	uint32_t reg;
	uint32_t mark;
	int error;

	/* The first check writes the Boolean. */
	if (first != 0) {
		gsi1_emit_alu(lowering, GSI1_F3_SLTU, GSI1_F7_BASE, inside, coordinate, limit);
		return 0;
	}

	/* Takes a register for the check, given back after it. */
	mark = gsi1_scratch_mark(lowering);
	error = gsi1_scratch(lowering, &reg);
	if (error != 0)
		return error;

	/* The check, and-ed in. */
	gsi1_emit_alu(lowering, GSI1_F3_SLTU, GSI1_F7_BASE, reg, coordinate, limit);
	gsi1_emit_alu(lowering, GSI1_F3_AND, GSI1_F7_BASE, inside, inside, reg);
	gsi1_scratch_release(lowering, mark);

	/* Succeeded: the Boolean holds the check. */
	return 0;
}

/*
 * Adds factor x multiplier to a sum: the multiplier is an operand, or
 * (multiplier_is_constant) a power of two or a small constant.
 */
static int
multiply_add(
	struct gsi1_lowering *lowering,
	uint32_t sum,
	uint32_t factor,
	uint32_t multiplier,
	int multiplier_is_constant)
{
	uint32_t product;
	uint32_t shift;
	uint32_t mark;
	int error;

	/* Takes a register for the product, given back after the sum. */
	mark = gsi1_scratch_mark(lowering);
	error = gsi1_scratch(lowering, &product);
	if (error != 0)
		return error;

	/* A constant power of two is a shift. */
	if (multiplier_is_constant != 0) {
		shift = 0U;
		while ((1U << shift) < multiplier && shift < 31U)
			shift++;

		/* A power of two shifts; anything else multiplies by its constant. */
		if ((1U << shift) == multiplier) {
			gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SLL, GSI1_F7_BASE, product, factor, shift));
		} else {
			gsi1_emit_constant(lowering, product, multiplier);
			gsi1_emit_alu(lowering, GSI1_F3_MUL, GSI1_F7_MULDIV, product, product, factor);
		}
	} else {
		gsi1_emit_alu(lowering, GSI1_F3_MUL, GSI1_F7_MULDIV, product, factor, multiplier);
	}

	/* sum += product. */
	gsi1_emit_alu(lowering, GSI1_F3_ADD, GSI1_F7_BASE, sum, sum, product);
	gsi1_scratch_release(lowering, mark);

	/* Succeeded: the product is added. */
	return 0;
}

/*
 * Loads a texel's words into scratch registers; a texel of one or two
 * bytes is shifted down from the word that holds it.
 */
static int
load_texel(
	struct gsi1_lowering *lowering,
	const struct gsi1_format *format,
	uint32_t address,
	uint32_t *words)
{
	uint32_t count;
	uint32_t index;
	uint32_t shift;
	int error;

	/* Each word of the texel (lw takes the word the address is in). */
	count = (format->bytes + 3U) / 4U;
	for (index = 0U; index < count; index++) {
		error = gsi1_scratch(lowering, &words[index]);
		if (error != 0)
			return error;
		gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_LOAD, GSI1_F3_LW, words[index], address, (int32_t)(4U * index), 0U));
	}

	/* A whole-word texel is as loaded. */
	if (format->bytes >= 4U)
		return 0;

	/* Takes a register for the byte position. */
	error = gsi1_scratch(lowering, &shift);
	if (error != 0)
		return error;

	/* A smaller one starts at byte (address & 3) of its word. */
	gsi1_emit_alu_immediate(lowering, GSI1_F3_AND, shift, address, 3);
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SLL, GSI1_F7_BASE, shift, shift, 3U));
	gsi1_emit_alu(lowering, GSI1_F3_SRL, GSI1_F7_BASE, words[0], words[0], shift);

	/* Succeeded: the texel's words are loaded. */
	return 0;
}

/*
 * Unpacks one component of a texel into dst: the format's value, or what
 * a missing component reads (0, and 1 for alpha: 1.0 for a float format,
 * 1 for an integer one).
 */
static int
unpack_component(
	struct gsi1_lowering *lowering,
	const struct gsi1_format *format,
	uint32_t component,
	const uint32_t *words,
	uint32_t dst)
{
	struct gsi1_extra extra;
	uint32_t bits;
	uint32_t shift;
	uint32_t word;
	uint32_t number;
	int integer;
	int sign;
	int error;

	/* The component's width and place. */
	bits = format->bits[component];
	shift = format->shift[component] % 32U;
	word = words[format->shift[component] / 32U];
	number = format->number;
	integer = 0;
	if (number == GSI1_NUMBER_UINT || number == GSI1_NUMBER_SINT)
		integer = 1;

	/* A missing component: zero, or one for alpha. */
	if (bits == 0U) {
		if (component != 3U) {
			gsi1_emit_move(lowering, dst, GSI1_ZERO);
		} else if (integer != 0) {
			gsi1_emit_move(lowering, dst, GSI1_ONEI);
		} else {
			gsi1_emit_move(lowering, dst, GSI1_ONE);
		}
		return 0;
	}

	/* A 32-bit component is the word. */
	if (bits == 32U) {
		gsi1_emit_move(lowering, dst, word);
		return 0;
	}

	/* A 16-bit float: unpackh of the half the component is in. */
	if (number == GSI1_NUMBER_SFLOAT) {
		kern_memset(&extra, 0, sizeof(extra));
		extra.sub = shift / 16U;
		gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_CUSTOM2, GSI1_F3_PACK, GSI1_F7_UNPACKH, dst, word, 0U, &extra));
		return 0;
	}

	/* The signed numbers are sign-extended. */
	sign = 0;
	if (number == GSI1_NUMBER_SINT) {
		sign = 1;
	} else if (number == GSI1_NUMBER_SNORM) {
		sign = 1;
	}

	/* The component's bits. */
	error = extract_bits(lowering, dst, word, shift, bits, sign);
	if (error != 0)
		return error;

	/* An integer is as extracted. */
	if (integer != 0)
		return 0;

	/* A normalized value to a float. */
	error = normalized_to_float(lowering, dst, bits, sign);
	if (error != 0)
		return error;

	/* sRGB's colour components through the transfer function (its alpha is linear). */
	if (number == GSI1_NUMBER_SRGB && component != 3U) {
		error = srgb_to_linear(lowering, dst);
		if (error != 0)
			return error;
	}

	/* Succeeded: the component is in dst. */
	return 0;
}

/* Extracts `bits` bits at `shift` of a word into dst, sign-extended or not. */
static int
extract_bits(
	struct gsi1_lowering *lowering,
	uint32_t dst,
	uint32_t word,
	uint32_t shift,
	uint32_t bits,
	int sign)
{
	uint32_t funct7;

	/* The bits to the top, then back down: arithmetic for a signed value. */
	funct7 = GSI1_F7_BASE;
	if (sign != 0)
		funct7 = GSI1_F7_ALT;
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SLL, GSI1_F7_BASE, dst, word, 32U - shift - bits));
	gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SRL, funct7, dst, dst, 32U - bits));

	/* Succeeded: the bits are in dst. */
	return 0;
}

/*
 * Converts the integer in dst to a normalized float: c / (2^b - 1), or
 * max(c / (2^(b-1) - 1), -1) for a signed one, the quotient rounded as a
 * division: q = c r, then q + (c - q d) r.
 */
static int
normalized_to_float(
	struct gsi1_lowering *lowering,
	uint32_t dst,
	uint32_t bits,
	int sign)
{
	const struct gsi1_scale *scale;
	uint32_t divisor;
	uint32_t reciprocal;
	uint32_t quotient;
	uint32_t mark;
	int error;

	/* The width's divisor and reciprocal. */
	if (sign != 0) {
		error = find_scale(gsi1_snorm_scales, sizeof(gsi1_snorm_scales) / sizeof(gsi1_snorm_scales[0]), bits, &scale);
	} else {
		error = find_scale(gsi1_unorm_scales, sizeof(gsi1_unorm_scales) / sizeof(gsi1_unorm_scales[0]), bits, &scale);
	}
	if (error != 0) {
		error = gsi1_refuse(lowering, ENOTSUP, "a normalized width the compiler does not convert");
		return error;
	}

	/* The divisor and the reciprocal in registers, given back after the conversion. */
	mark = gsi1_scratch_mark(lowering);
	error = constant_register(lowering, scale->divisor, &divisor);
	if (error != 0)
		return error;

	/* Puts the reciprocal in a register. */
	error = constant_register(lowering, scale->reciprocal, &reciprocal);
	if (error != 0)
		return error;

	/* Takes a register for the quotient. */
	error = gsi1_scratch(lowering, &quotient);
	if (error != 0)
		return error;

	/* c as a float. */
	if (sign != 0) {
		gsi1_emit_float(lowering, GSI1_F7_FCVT_S_W, GSI1_RM_RNE, dst, dst, 0U, 0U);
	} else {
		gsi1_emit_float(lowering, GSI1_F7_FCVT_S_W, GSI1_RM_RNE, dst, dst, 1U, 0U);
	}

	/* q = c r; the remainder c - q d; q + remainder r. */
	gsi1_emit_float(lowering, GSI1_F7_FMUL, GSI1_RM_RNE, quotient, dst, reciprocal, 0U);
	gsi1_emit_fused(lowering, GSI1_OP_NMSUB, dst, quotient, divisor, dst, 0U);
	gsi1_emit_fused(lowering, GSI1_OP_MADD, dst, dst, reciprocal, quotient, 0U);

	/* A signed one is at least -1 (the most negative code). */
	if (sign != 0)
		gsi1_emit_float(lowering, GSI1_F7_FMINMAX, 1U, dst, dst, GSI1_MONE, 0U);
	gsi1_scratch_release(lowering, mark);

	/* Succeeded: the float is in dst. */
	return 0;
}

/*
 * Turns an sRGB-encoded value in dst into a linear one: x / 12.92 up to
 * 0.04045, ((x + 0.055) / 1.055) ^ 2.4 above (exp2 and log2).
 */
static int
srgb_to_linear(
	struct gsi1_lowering *lowering,
	uint32_t dst)
{
	struct gsi1_extra extra;
	uint32_t constant;
	uint32_t curve;
	uint32_t linear;
	uint32_t above;
	uint32_t mark;
	int error;

	/* Takes the registers of the two branches and of the choice, given back after it. */
	mark = gsi1_scratch_mark(lowering);
	error = gsi1_scratch(lowering, &curve);
	if (error != 0)
		return error;

	/* Takes the linear branch's. */
	error = gsi1_scratch(lowering, &linear);
	if (error != 0)
		return error;

	/* Takes the choice's. */
	error = gsi1_scratch(lowering, &above);
	if (error != 0)
		return error;

	/* The linear branch: x / 12.92. */
	error = constant_register(lowering, GSI1_SRGB_LINEAR_SCALE, &constant);
	if (error != 0)
		return error;
	gsi1_emit_float(lowering, GSI1_F7_FMUL, GSI1_RM_RNE, linear, dst, constant, 0U);

	/* The curve: x / 1.055 + 0.055 / 1.055, then its 2.4th power. */
	gsi1_emit_constant(lowering, constant, GSI1_SRGB_SCALE);
	gsi1_emit_constant(lowering, curve, GSI1_SRGB_OFFSET);
	gsi1_emit_fused(lowering, GSI1_OP_MADD, curve, dst, constant, curve, 0U);
	gsi1_emit_transcendental(lowering, GSI1_XU_LOG2, curve, curve);
	gsi1_emit_constant(lowering, constant, GSI1_SRGB_EXPONENT);
	gsi1_emit_float(lowering, GSI1_F7_FMUL, GSI1_RM_RNE, curve, curve, constant, 0U);
	gsi1_emit_transcendental(lowering, GSI1_XU_EXP2, curve, curve);

	/* The choice: x above the threshold takes the curve. */
	gsi1_emit_constant(lowering, constant, GSI1_SRGB_THRESHOLD);
	gsi1_emit_float(lowering, GSI1_F7_FCMP, GSI1_F3_FLT, above, constant, dst, 0U);
	kern_memset(&extra, 0, sizeof(extra));
	gsi1_emit(lowering, gsi1_encode_r4(GSI1_OP_CUSTOM3, GSI1_F3_SEL, dst, above, curve, linear, &extra));
	gsi1_scratch_release(lowering, mark);

	/* Succeeded: the linear value is in dst. */
	return 0;
}

/*
 * Packs the four values from `first_value` into a texel's words in
 * scratch registers.
 */
static int
pack_texel(
	struct gsi1_lowering *lowering,
	const struct gsi1_format *format,
	uint32_t first_value,
	uint32_t *words)
{
	uint32_t packed;
	uint32_t count;
	uint32_t index;
	uint32_t component;
	uint32_t value;
	uint32_t low;
	int error;

	/* The words, zero to start with. */
	count = (format->bytes + 3U) / 4U;
	for (index = 0U; index < count; index++) {
		error = gsi1_scratch(lowering, &words[index]);
		if (error != 0)
			return error;
		gsi1_emit_move(lowering, words[index], GSI1_ZERO);
	}

	/* Takes a register for each packed component. */
	error = gsi1_scratch(lowering, &packed);
	if (error != 0)
		return error;

	/* A 16-bit float format: packh of each pair. */
	if (format->number == GSI1_NUMBER_SFLOAT && format->bits[0] == 16U) {
		for (index = 0U; index < count; index++) {
			error = gsi1_source(lowering, first_value + 2U * index, &low);
			if (error != 0)
				return error;
			error = gsi1_source(lowering, first_value + 2U * index + 1U, &value);
			if (error != 0)
				return error;
			gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_CUSTOM2, GSI1_F3_PACK, GSI1_F7_PACKH, words[index], low, value, NULL));
		}
		return 0;
	}

	/* Each component the format has, into its bits. */
	for (component = 0U; component < 4U; component++) {
		if (format->bits[component] == 0U)
			continue;
		error = gsi1_source(lowering, first_value + component, &value);
		if (error != 0)
			return error;

		/* The component packed, then or-ed into its word at its place. */
		error = pack_component(lowering, format, component, value, packed);
		if (error != 0)
			return error;
		index = format->shift[component] / 32U;
		if (format->shift[component] % 32U != 0U) {
			gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SLL, GSI1_F7_BASE, packed, packed, format->shift[component] % 32U));
		}
		gsi1_emit_alu(lowering, GSI1_F3_OR, GSI1_F7_BASE, words[index], words[index], packed);
	}

	/* Succeeded: the texel's words are packed. */
	return 0;
}

/*
 * Packs one component's value into dst's low bits: a 32-bit one as it is,
 * an integer's low bits, a normalized one clamped, scaled and rounded to
 * nearest even.
 */
static int
pack_component(
	struct gsi1_lowering *lowering,
	const struct gsi1_format *format,
	uint32_t component,
	uint32_t value,
	uint32_t dst)
{
	const struct gsi1_scale *scale;
	uint32_t bits;
	uint32_t divisor;
	uint32_t mark;
	int32_t mask;
	int error;

	/* The component's width. */
	bits = format->bits[component];
	mask = (int32_t)((1U << (bits & 31U)) - 1U);

	/* A 32-bit component is the value. */
	if (bits == 32U) {
		gsi1_emit_move(lowering, dst, value);
		return 0;
	}

	/* An integer keeps its low bits. */
	if (format->number == GSI1_NUMBER_UINT || format->number == GSI1_NUMBER_SINT) {
		gsi1_emit_alu_immediate(lowering, GSI1_F3_AND, dst, value, mask);
		return 0;
	}

	/* A normalized width's divisor. */
	if (format->number == GSI1_NUMBER_SNORM) {
		error = find_scale(gsi1_snorm_scales, sizeof(gsi1_snorm_scales) / sizeof(gsi1_snorm_scales[0]), bits, &scale);
	} else {
		error = find_scale(gsi1_unorm_scales, sizeof(gsi1_unorm_scales) / sizeof(gsi1_unorm_scales[0]), bits, &scale);
	}
	if (error != 0) {
		error = gsi1_refuse(lowering, ENOTSUP, "a normalized width the compiler does not pack");
		return error;
	}

	/* Puts the divisor in a register, given back after the component. */
	mark = gsi1_scratch_mark(lowering);
	error = constant_register(lowering, scale->divisor, &divisor);
	if (error != 0)
		return error;

	/* Signed: clamped to [-1, 1], scaled, rounded, its low bits. */
	if (format->number == GSI1_NUMBER_SNORM) {
		gsi1_emit_float(lowering, GSI1_F7_FMINMAX, 1U, dst, value, GSI1_MONE, 0U);
		gsi1_emit_float(lowering, GSI1_F7_FMINMAX, 0U, dst, dst, GSI1_ONE, 0U);
		gsi1_emit_float(lowering, GSI1_F7_FMUL, GSI1_RM_RNE, dst, dst, divisor, 0U);
		gsi1_emit_float(lowering, GSI1_F7_FCVT_W_S, GSI1_RM_RNE, dst, dst, 0U, 0U);
		gsi1_emit_alu_immediate(lowering, GSI1_F3_AND, dst, dst, mask);
		gsi1_scratch_release(lowering, mark);
		return 0;
	}

	/* Unsigned: clamped to [0, 1] (NaN to 0), scaled, rounded. */
	gsi1_emit_float(lowering, GSI1_F7_FADD, GSI1_RM_RNE, dst, value, GSI1_ZERO, GSI1_W1_SAT);
	gsi1_emit_float(lowering, GSI1_F7_FMUL, GSI1_RM_RNE, dst, dst, divisor, 0U);
	gsi1_emit_float(lowering, GSI1_F7_FCVT_W_S, GSI1_RM_RNE, dst, dst, 1U, 0U);
	gsi1_scratch_release(lowering, mark);

	/* Succeeded: the component's bits are in dst. */
	return 0;
}

/* Puts a constant into a scratch register. */
static int
constant_register(
	struct gsi1_lowering *lowering,
	uint32_t bits,
	uint32_t *reg)
{
	int error;

	/* Takes the scratch register. */
	error = gsi1_scratch(lowering, reg);
	if (error != 0)
		return error;

	/* Sets it to the bits. */
	gsi1_emit_constant(lowering, *reg, bits);

	/* Succeeded: the constant is in the register. */
	return 0;
}
