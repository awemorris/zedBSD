/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lowering of a compute program's storage image accesses: imageLoad,
 * imageStore and the atomics on a texel.
 *
 * The driver keeps an image as rows of texels (a raster) and converts it
 * for sampling, so a program reads and writes a storage image's texels
 * directly in its raster through the general memory TMU, as it does a
 * storage buffer's words: the texel's address is the view's base plus
 * x × texel bytes + y × pitch + layer × layer stride, clamped into the
 * view's bytes, because one GPU page table serves every session.  A read
 * converts the texel's words into the four components of its format, and
 * a write converts four components into the texel's words, in the
 * program, with the ALU's integer, float and half-float operations.
 *
 * Mesa's v3dv reaches its UIF images with the TMU's texture operations
 * (v3d40_vir_emit_image_load_store in src/broadcom/compiler/
 * v3d40_tex.c); the formats and their conversions are those Mesa's
 * nir_format_convert.h gives a format of each kind (unorm and snorm
 * scaling with rounding and clamping, integer clamping, half floats).
 *
 * Included by shader.c after shader-compute.inc, whose helpers it uses and
 * whose list declares its functions.
 */

/* The float bits the conversions use: 0.0, 1.0, -1.0, 255.0, 1/255, 127.0 and 1/127. */
#define IMAGE_FLOAT_ZERO 0x00000000U
#define IMAGE_FLOAT_ONE 0x3f800000U
#define IMAGE_FLOAT_MINUS_ONE 0xbf800000U
#define IMAGE_FLOAT_255 0x437f0000U
#define IMAGE_FLOAT_INVERSE_255 0x3b808081U
#define IMAGE_FLOAT_127 0x42fe0000U
#define IMAGE_FLOAT_INVERSE_127 0x3c010204U

/*
 * The VkFormat numbers of the storage formats (the Vulkan specification's
 * VkFormat; the SPIR-V parser's uniform `format` holds them).
 */
#define IMAGE_RGBA8_UNORM 37U
#define IMAGE_RGBA8_SNORM 38U
#define IMAGE_RGBA8_UINT 41U
#define IMAGE_RGBA8_SINT 42U
#define IMAGE_RGBA16_UINT 95U
#define IMAGE_RGBA16_SINT 96U
#define IMAGE_RGBA16_SFLOAT 97U
#define IMAGE_R32_UINT 98U
#define IMAGE_R32_SINT 99U
#define IMAGE_R32_SFLOAT 100U
#define IMAGE_RG32_UINT 101U
#define IMAGE_RG32_SINT 102U
#define IMAGE_RG32_SFLOAT 103U
#define IMAGE_RGBA32_UINT 107U
#define IMAGE_RGBA32_SINT 108U
#define IMAGE_RGBA32_SFLOAT 109U

/*
 * Gives the bytes of a texel of a storage image format the lowering
 * converts: the formats Vulkan requires of storage images without
 * shaderStorageImageExtendedFormats; zero for any other.
 */
uint32_t
bcm2711_shader_storage_texel(
	uint32_t format)
{
	/* Each format by its texel's size. */
	switch (format) {
	case IMAGE_RGBA8_UNORM:
	case IMAGE_RGBA8_SNORM:
	case IMAGE_RGBA8_UINT:
	case IMAGE_RGBA8_SINT:
	case IMAGE_R32_UINT:
	case IMAGE_R32_SINT:
	case IMAGE_R32_SFLOAT:
		return 4;
	case IMAGE_RGBA16_UINT:
	case IMAGE_RGBA16_SINT:
	case IMAGE_RGBA16_SFLOAT:
	case IMAGE_RG32_UINT:
	case IMAGE_RG32_SINT:
	case IMAGE_RG32_SFLOAT:
		return 8;
	case IMAGE_RGBA32_UINT:
	case IMAGE_RGBA32_SINT:
	case IMAGE_RGBA32_SFLOAT:
		return 16;
	default:
		break;
	}

	/* Succeeded: no other format is converted. */
	return 0;
}

/*
 * Lowers an imageLoad: the texel's words, loaded one after another from
 * its clamped address, converted into the four components of the image's
 * format (those it lacks read 0, 0, 0 and one).  A predicated read reads
 * on every lane: the clamped address stays in the image, and a lane
 * outside the predicate never reads the result.
 */
static int
lower_image_read(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	const struct drv_gpu_shader_ir_uniform *image;
	struct bcm2711_shader_value *scalar;
	struct bcm2711_qpu_source none;
	uint32_t words[4];
	uint32_t count;
	uint32_t address;
	uint32_t moved;
	uint32_t index;
	uint32_t destination;
	int error;

	/* The texel's address and its words. */
	image = &compiler->ir->uniforms[instruction->location];
	count = bcm2711_shader_storage_texel(image->format) / 4U;
	if (count == 0)
		return EINVAL;
	error = temporary(compiler, &address);
	if (error != 0)
		return error;
	error = image_address(compiler, instruction, count * 4U, address);
	if (error != 0)
		return error;

	/* Each word: its address launches the lookup, the switch lets the other threads run until it completes. */
	none = bcm2711_shader_bits(0);
	for (index = 0; index < count; index++) {
		error = tmu_room(compiler);
		if (error != 0)
			return error;
		moved = address;
		if (index != 0) {
			error = temporary(compiler, &moved);
			if (error != 0)
				return error;
			error = emit(compiler, BCM2711_QPU_INTEGER_ADD, moved, bcm2711_shader_rf(address), bcm2711_shader_bits(4U * index));
			if (error != 0)
				return error;
		}
		error = emit_magic(compiler, BCM2711_QPU_MOVE, COMPUTE_TMUA, bcm2711_shader_rf(moved), none, 0, 0);
		if (error != 0)
			return error;
		error = bcm2711_shader_switch(compiler, BCM2711_SHADER_SWITCH_SAMPLE);
		if (error != 0)
			return error;
		error = temporary(compiler, &words[index]);
		if (error != 0)
			return error;
		error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_TEXTURE, words[index], 0);
		if (error != 0)
			return error;
	}

	/* The four components, each in a register of its own value. */
	for (index = 0; index < 4U; index++) {
		error = bcm2711_shader_register(compiler, &destination);
		if (error != 0)
			return error;
		scalar = &compiler->values[instruction->dst + index];
		scalar->number = destination;
		scalar->register_live = 1;
		error = image_unpack(compiler, image->format, words, index, destination);
		if (error != 0)
			return error;
	}

	/* Succeeded: the texel's components are in the destinations. */
	return 0;
}

/*
 * Lowers an imageStore: four components converted into the texel's words,
 * stored one after another at its clamped address.  A predicated store
 * writes conditionally and is jumped over when no dispatched lane is in
 * its predicate, as a predicated storage buffer store is.
 */
static int
lower_image_write(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	const struct drv_gpu_shader_ir_uniform *image;
	struct bcm2711_qpu_source none;
	struct bcm2711_qpu_source predicate;
	uint32_t words[4];
	uint32_t addresses[4];
	uint32_t count;
	uint32_t index;
	uint32_t record;
	uint32_t write_predicate;
	int error;

	/* The texel's words and their addresses, all before any branch. */
	image = &compiler->ir->uniforms[instruction->location];
	count = bcm2711_shader_storage_texel(image->format) / 4U;
	if (count == 0)
		return EINVAL;
	error = temporary(compiler, &addresses[0]);
	if (error != 0)
		return error;
	error = image_address(compiler, instruction, count * 4U, addresses[0]);
	if (error != 0)
		return error;
	for (index = 0; index < count; index++) {
		error = temporary(compiler, &words[index]);
		if (error != 0)
			return error;
		error = image_pack(compiler, image->format, instruction->src[2], index, words[index]);
		if (error != 0)
			return error;
		if (index == 0)
			continue;
		error = temporary(compiler, &addresses[index]);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_INTEGER_ADD, addresses[index], bcm2711_shader_rf(addresses[0]), bcm2711_shader_bits(4U * index));
		if (error != 0)
			return error;
	}

	/* Room in the TMU queue for every word. */
	none = bcm2711_shader_bits(0);
	if (compiler->pending_stores + count > COMPUTE_STORES_BEFORE_WAIT) {
		error = wait_stores(compiler);
		if (error != 0)
			return error;
	}

	/* An unpredicated store writes on every lane. */
	write_predicate = 0;
	record = BCM2711_SHADER_ABSENT;
	if (instruction->component == 1) {
		/* Flag A = the predicate is false; no stores are outstanding at the branch. */
		error = bcm2711_shader_operand(compiler, instruction->src[3], BCM2711_SHADER_SCRATCH_WORK, &predicate);
		if (error != 0)
			return error;
		error = emit_magic(compiler, BCM2711_QPU_MOVE, COMPUTE_DISCARD, predicate, none, 0, COMPUTE_PUSH_ZERO);
		if (error != 0)
			return error;
		error = wait_stores(compiler);
		if (error != 0)
			return error;

		/* Jumps over the stores when every dispatched lane is outside the predicate; three idle delay words. */
		error = bcm2711_shader_branch(compiler, BCM2711_QPU_BRANCH_ALL_A, &record);
		if (error != 0)
			return error;
		error = bcm2711_shader_nops(compiler, 3);
		if (error != 0)
			return error;
		write_predicate = COMPUTE_IF_NOT_A;
	}

	/* Each word's data, then the address that sends its store. */
	for (index = 0; index < count; index++) {
		error = emit_magic(compiler, BCM2711_QPU_MOVE, COMPUTE_TMUD, bcm2711_shader_rf(words[index]), none, write_predicate, 0);
		if (error != 0)
			return error;
		error = emit_magic(compiler, BCM2711_QPU_MOVE, COMPUTE_TMUA, bcm2711_shader_rf(addresses[index]), none, write_predicate, 0);
		if (error != 0)
			return error;
		compiler->pending_stores++;
	}

	/* No TMU operation crosses into the block the branch lands in: the skipped stores are waited for inside, then the branch lands. */
	if (record != BCM2711_SHADER_ABSENT) {
		error = wait_stores(compiler);
		if (error != 0)
			return error;
		bcm2711_shader_branch_target(compiler, record);
	}

	/* Succeeded: the texel's stores are sent. */
	return 0;
}

/*
 * Computes the clamped address of an access of `bytes` bytes at a storage
 * image's texel into a register: base + umin(x × texel + y × pitch +
 * layer × stride, limit), with the texel's x, y and layer the three
 * consecutive values from src[0] and the rest from the uniform stream.
 */
static int
image_address(
	struct bcm2711_shader_compiler *compiler,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t bytes,
	uint32_t address)
{
	const struct drv_gpu_shader_ir_uniform *image;
	const struct bcm2711_shader_value *layer;
	struct bcm2711_qpu_source operand;
	uint32_t texel;
	uint32_t shift;
	uint32_t number;
	uint32_t stride;
	uint32_t product;
	int error;

	/* x times the texel's bytes, a power of two. */
	image = &compiler->ir->uniforms[instruction->location];
	texel = bcm2711_shader_storage_texel(image->format);
	if (power_of_two(texel, &shift) == 0)
		return EINVAL;
	error = value_register(compiler, instruction->src[0], &number);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_SHIFT_LEFT, address, bcm2711_shader_rf(number), bcm2711_shader_bits(shift));
	if (error != 0)
		return error;

	/* Plus y times the pitch (both halves of the multiply: a pitch has more than 24 bits' worth of rows in a large image). */
	error = temporary(compiler, &stride);
	if (error != 0)
		return error;
	error = temporary(compiler, &product);
	if (error != 0)
		return error;
	error = storage_uniform(compiler, BCM2711_SHADER_IMAGE_PITCH, image, 0, stride);
	if (error != 0)
		return error;
	error = bcm2711_shader_operand(compiler, instruction->src[0] + 1U, BCM2711_SHADER_SCRATCH_RIGHT, &operand);
	if (error != 0)
		return error;
	error = emit_magic(compiler, BCM2711_QPU_MULTIPLY_TOP, COMPUTE_DISCARD, bcm2711_shader_rf(stride), operand, 0, 0);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_MULTIPLY_24, product, bcm2711_shader_rf(stride), operand);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_INTEGER_ADD, address, bcm2711_shader_rf(address), bcm2711_shader_rf(product));
	if (error != 0)
		return error;

	/* Plus the layer times the layer stride, unless the layer is the constant zero (an image of one layer). */
	layer = &compiler->values[instruction->src[0] + 2U];
	if (layer->constant == 0 || layer->number != 0) {
		error = storage_uniform(compiler, BCM2711_SHADER_IMAGE_LAYER, image, 0, stride);
		if (error != 0)
			return error;
		error = bcm2711_shader_operand(compiler, instruction->src[0] + 2U, BCM2711_SHADER_SCRATCH_RIGHT, &operand);
		if (error != 0)
			return error;
		error = emit_magic(compiler, BCM2711_QPU_MULTIPLY_TOP, COMPUTE_DISCARD, bcm2711_shader_rf(stride), operand, 0, 0);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_MULTIPLY_24, product, bcm2711_shader_rf(stride), operand);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_INTEGER_ADD, address, bcm2711_shader_rf(address), bcm2711_shader_rf(product));
		if (error != 0)
			return error;
	}

	/* Clamped to the view's last start for the access, then added to its base. */
	error = storage_uniform(compiler, BCM2711_SHADER_IMAGE_LIMIT, image, bytes, stride);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_UNSIGNED_MINIMUM, address, bcm2711_shader_rf(stride), bcm2711_shader_rf(address));
	if (error != 0)
		return error;
	error = storage_uniform(compiler, BCM2711_SHADER_IMAGE_BASE, image, 0, stride);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_INTEGER_ADD, address, bcm2711_shader_rf(stride), bcm2711_shader_rf(address));
	if (error != 0)
		return error;

	/* Succeeded: the address is inside the image's view. */
	return 0;
}

/*
 * Converts the loaded words of a texel into its component `index`
 * (0 red to 3 alpha) in the destination: a 32-bit format's words as they
 * are, a 16-bit one's halves (half floats, or integers zero- or
 * sign-extended), an 8-bit one's bytes (integers extended, or unorm and
 * snorm scaled to floats, an snorm's -128 read as -1.0).
 */
static int
image_unpack(
	struct bcm2711_shader_compiler *compiler,
	uint32_t format,
	const uint32_t *words,
	uint32_t index,
	uint32_t destination)
{
	struct bcm2711_qpu_source constant;
	struct bcm2711_qpu_source word;
	uint32_t texel;
	uint32_t missing;
	int error;

	/* The components a 32-bit format lacks: zero, and an alpha of one. */
	texel = bcm2711_shader_storage_texel(format);
	missing = 0;
	if ((format == IMAGE_R32_UINT || format == IMAGE_R32_SINT || format == IMAGE_R32_SFLOAT) && index != 0)
		missing = 1;
	if ((format == IMAGE_RG32_UINT || format == IMAGE_RG32_SINT || format == IMAGE_RG32_SFLOAT) && index > 1U)
		missing = 1;
	if (missing != 0) {
		constant = bcm2711_shader_bits(0);
		if (index == 3U && (format == IMAGE_R32_SFLOAT || format == IMAGE_RG32_SFLOAT)) {
			error = bits_source(compiler, IMAGE_FLOAT_ONE, BCM2711_SHADER_SCRATCH_RIGHT, &constant);
			if (error != 0)
				return error;
		} else if (index == 3U) {
			constant = bcm2711_shader_bits(1);
		}
		error = emit(compiler, BCM2711_QPU_MOVE, destination, constant, bcm2711_shader_bits(0));
		if (error != 0)
			return error;
		return 0;
	}

	/* A 32-bit component is its word. */
	if (texel == 16U || format == IMAGE_R32_UINT || format == IMAGE_R32_SINT || format == IMAGE_R32_SFLOAT ||
	    format == IMAGE_RG32_UINT || format == IMAGE_RG32_SINT || format == IMAGE_RG32_SFLOAT) {
		error = emit(compiler, BCM2711_QPU_MOVE, destination, bcm2711_shader_rf(words[index]), bcm2711_shader_bits(0));
		if (error != 0)
			return error;
		return 0;
	}

	/* A 16-bit component is a half of word index / 2. */
	if (texel == 8U) {
		word = bcm2711_shader_rf(words[index / 2U]);
		error = image_half(compiler, format, word, index & 1U, destination);
		if (error != 0)
			return error;
		return 0;
	}

	/* An 8-bit component is byte index of the word. */
	error = image_byte(compiler, format, bcm2711_shader_rf(words[0]), index, destination);
	if (error != 0)
		return error;

	/* Succeeded: the component is in the destination. */
	return 0;
}

/* Converts the low (high 0) or high (1) half of a word of a 16-bit format into a component. */
static int
image_half(
	struct bcm2711_shader_compiler *compiler,
	uint32_t format,
	struct bcm2711_qpu_source word,
	uint32_t high,
	uint32_t destination)
{
	struct bcm2711_qpu_source mask;
	int error;

	/* A half float is unpacked by the float move. */
	if (format == IMAGE_RGBA16_SFLOAT) {
		if (high != 0)
			error = emit(compiler, BCM2711_QPU_HALF_HIGH, destination, word, bcm2711_shader_bits(0));
		else
			error = emit(compiler, BCM2711_QPU_HALF_LOW, destination, word, bcm2711_shader_bits(0));
		if (error != 0)
			return error;
		return 0;
	}

	/* A signed half moves to the top, then shifts back down with its sign; an unsigned one is masked or shifted down. */
	if (format == IMAGE_RGBA16_SINT) {
		if (high == 0) {
			error = emit(compiler, BCM2711_QPU_SHIFT_LEFT, destination, word, bcm2711_shader_bits(shift_bits(16)));
			if (error != 0)
				return error;
			word = bcm2711_shader_rf(destination);
		}
		error = emit(compiler, BCM2711_QPU_SHIFT_SIGNED, destination, word, bcm2711_shader_bits(shift_bits(16)));
		if (error != 0)
			return error;
		return 0;
	}
	if (high != 0) {
		error = emit(compiler, BCM2711_QPU_SHIFT_RIGHT, destination, word, bcm2711_shader_bits(shift_bits(16)));
		if (error != 0)
			return error;
		return 0;
	}
	error = bits_source(compiler, 0xffffU, BCM2711_SHADER_SCRATCH_RIGHT, &mask);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_BITS_AND, destination, word, mask);
	if (error != 0)
		return error;

	/* Succeeded: the half's component is in the destination. */
	return 0;
}

/* Converts byte `index` of a word of an 8-bit format into a component. */
static int
image_byte(
	struct bcm2711_shader_compiler *compiler,
	uint32_t format,
	struct bcm2711_qpu_source word,
	uint32_t index,
	uint32_t destination)
{
	struct bcm2711_qpu_source constant;
	struct bcm2711_qpu_source shifted;
	int error;

	/* A signed byte moves to the top and shifts back down with its sign; an unsigned one shifts down and is masked. */
	if (format == IMAGE_RGBA8_SINT || format == IMAGE_RGBA8_SNORM) {
		shifted = word;
		if (index != 3U) {
			error = emit(compiler, BCM2711_QPU_SHIFT_LEFT, destination, word, bcm2711_shader_bits(shift_bits(24U - 8U * index)));
			if (error != 0)
				return error;
			shifted = bcm2711_shader_rf(destination);
		}
		error = emit(compiler, BCM2711_QPU_SHIFT_SIGNED, destination, shifted, bcm2711_shader_bits(shift_bits(24)));
		if (error != 0)
			return error;
	} else {
		shifted = word;
		if (index != 0) {
			error = emit(compiler, BCM2711_QPU_SHIFT_RIGHT, destination, word, bcm2711_shader_bits(shift_bits(8U * index)));
			if (error != 0)
				return error;
			shifted = bcm2711_shader_rf(destination);
		}
		if (index != 3U) {
			error = bits_source(compiler, 0xffU, BCM2711_SHADER_SCRATCH_RIGHT, &constant);
			if (error != 0)
				return error;
			error = emit(compiler, BCM2711_QPU_BITS_AND, destination, shifted, constant);
			if (error != 0)
				return error;
		}
	}

	/* An integer byte is the component. */
	if (format == IMAGE_RGBA8_UINT || format == IMAGE_RGBA8_SINT)
		return 0;

	/* A unorm byte scales by 1/255. */
	if (format == IMAGE_RGBA8_UNORM) {
		error = emit(compiler, BCM2711_QPU_UNSIGNED_TO_FLOAT, destination, bcm2711_shader_rf(destination), bcm2711_shader_bits(0));
		if (error != 0)
			return error;
		error = bits_source(compiler, IMAGE_FLOAT_INVERSE_255, BCM2711_SHADER_SCRATCH_RIGHT, &constant);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_FLOAT_MULTIPLY, destination, bcm2711_shader_rf(destination), constant);
		if (error != 0)
			return error;
		return 0;
	}

	/* An snorm byte scales by 1/127, no smaller than -1.0. */
	error = emit(compiler, BCM2711_QPU_SIGNED_TO_FLOAT, destination, bcm2711_shader_rf(destination), bcm2711_shader_bits(0));
	if (error != 0)
		return error;
	error = bits_source(compiler, IMAGE_FLOAT_INVERSE_127, BCM2711_SHADER_SCRATCH_RIGHT, &constant);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_FLOAT_MULTIPLY, destination, bcm2711_shader_rf(destination), constant);
	if (error != 0)
		return error;
	error = bits_source(compiler, IMAGE_FLOAT_MINUS_ONE, BCM2711_SHADER_SCRATCH_RIGHT, &constant);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_FLOAT_MAXIMUM, destination, bcm2711_shader_rf(destination), constant);
	if (error != 0)
		return error;

	/* Succeeded: the byte's component is in the destination. */
	return 0;
}

/*
 * Converts the four components from value `first` into word `index` of a
 * texel of the format: a 32-bit format's component as it is, two 16-bit
 * components (half floats, or integers clamped to the half's range), or
 * all four 8-bit ones (integers clamped, or unorm and snorm floats clamped,
 * scaled and rounded to the nearest).
 */
static int
image_pack(
	struct bcm2711_shader_compiler *compiler,
	uint32_t format,
	uint32_t first,
	uint32_t index,
	uint32_t word)
{
	struct bcm2711_qpu_source low;
	struct bcm2711_qpu_source high;
	uint32_t texel;
	uint32_t component;
	uint32_t number;
	int error;

	/* A 32-bit component is the word. */
	texel = bcm2711_shader_storage_texel(format);
	if (texel == 16U || format == IMAGE_R32_UINT || format == IMAGE_R32_SINT || format == IMAGE_R32_SFLOAT ||
	    format == IMAGE_RG32_UINT || format == IMAGE_RG32_SINT || format == IMAGE_RG32_SFLOAT) {
		error = bcm2711_shader_operand(compiler, first + index, BCM2711_SHADER_SCRATCH_LEFT, &low);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_MOVE, word, low, bcm2711_shader_bits(0));
		if (error != 0)
			return error;
		return 0;
	}

	/* Two half floats are packed by the float pack, the first in the low half. */
	if (format == IMAGE_RGBA16_SFLOAT) {
		error = value_register(compiler, first + 2U * index, &number);
		if (error != 0)
			return error;
		low = bcm2711_shader_rf(number);
		error = bcm2711_shader_operand(compiler, first + 2U * index + 1U, BCM2711_SHADER_SCRATCH_RIGHT, &high);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_PACK_HALVES, word, low, high);
		if (error != 0)
			return error;
		return 0;
	}

	/* Two 16-bit integers: each clamped and masked, the second shifted up. */
	if (texel == 8U) {
		error = image_integer(compiler, format, first + 2U * index, 0, word);
		if (error != 0)
			return error;
		error = image_integer(compiler, format, first + 2U * index + 1U, 16, word);
		if (error != 0)
			return error;
		return 0;
	}

	/* Four 8-bit components, each in its byte. */
	for (component = 0; component < 4U; component++) {
		if (format == IMAGE_RGBA8_UNORM || format == IMAGE_RGBA8_SNORM)
			error = image_normalized(compiler, format, first + component, 8U * component, word);
		else
			error = image_integer(compiler, format, first + component, 8U * component, word);
		if (error != 0)
			return error;
	}

	/* Succeeded: the word holds its components. */
	return 0;
}

/*
 * Places an integer component, clamped to the range of a 16- or 8-bit
 * channel of the format and masked to its bits, at bit `shift` of the
 * word: the word is the component at shift zero, and ORed with it after.
 */
static int
image_integer(
	struct bcm2711_shader_compiler *compiler,
	uint32_t format,
	uint32_t value,
	uint32_t shift,
	uint32_t word)
{
	struct bcm2711_qpu_source source;
	struct bcm2711_qpu_source constant;
	uint32_t number;
	uint32_t largest;
	uint32_t smallest;
	uint32_t mask;
	int error;

	/* The channel's range and bits. */
	mask = 0xffffU;
	largest = 0xffffU;
	smallest = 0;
	if (format == IMAGE_RGBA16_SINT) {
		largest = 0x7fffU;
		smallest = 0xffff8000U;
	} else if (format == IMAGE_RGBA8_UINT) {
		mask = 0xffU;
		largest = 0xffU;
	} else if (format == IMAGE_RGBA8_SINT) {
		mask = 0xffU;
		largest = 0x7fU;
		smallest = 0xffffff80U;
	}

	/* The component clamped to the range, in the work scratch register. */
	error = value_register(compiler, value, &number);
	if (error != 0)
		return error;
	source = bcm2711_shader_rf(number);
	error = bits_source(compiler, largest, BCM2711_SHADER_SCRATCH_RIGHT, &constant);
	if (error != 0)
		return error;
	if (smallest == 0) {
		error = emit(compiler, BCM2711_QPU_UNSIGNED_MINIMUM, BCM2711_SHADER_SCRATCH_WORK, source, constant);
		if (error != 0)
			return error;
	} else {
		error = emit(compiler, BCM2711_QPU_INTEGER_MINIMUM, BCM2711_SHADER_SCRATCH_WORK, source, constant);
		if (error != 0)
			return error;
		error = bits_source(compiler, smallest, BCM2711_SHADER_SCRATCH_RIGHT, &constant);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_INTEGER_MAXIMUM, BCM2711_SHADER_SCRATCH_WORK, bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK), constant);
		if (error != 0)
			return error;
		error = bits_source(compiler, mask, BCM2711_SHADER_SCRATCH_RIGHT, &constant);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_BITS_AND, BCM2711_SHADER_SCRATCH_WORK, bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK), constant);
		if (error != 0)
			return error;
	}

	/* The bits placed in the word. */
	error = image_place(compiler, shift, word);
	if (error != 0)
		return error;

	/* Succeeded: the component is in the word. */
	return 0;
}

/*
 * Places a unorm (0.0 to 1.0) or snorm (-1.0 to 1.0) component, clamped,
 * scaled by 255 or 127 and rounded to the nearest, in the byte at bit
 * `shift` of the word.
 */
static int
image_normalized(
	struct bcm2711_shader_compiler *compiler,
	uint32_t format,
	uint32_t value,
	uint32_t shift,
	uint32_t word)
{
	struct bcm2711_qpu_source constant;
	struct bcm2711_qpu_source work;
	uint32_t number;
	uint32_t lowest;
	uint32_t scale;
	int error;

	/* The range and the scale. */
	lowest = IMAGE_FLOAT_ZERO;
	scale = IMAGE_FLOAT_255;
	if (format == IMAGE_RGBA8_SNORM) {
		lowest = IMAGE_FLOAT_MINUS_ONE;
		scale = IMAGE_FLOAT_127;
	}

	/* Clamped into the range, scaled and rounded, in the work scratch register. */
	work = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK);
	error = value_register(compiler, value, &number);
	if (error != 0)
		return error;
	error = bits_source(compiler, lowest, BCM2711_SHADER_SCRATCH_RIGHT, &constant);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_FLOAT_MAXIMUM, BCM2711_SHADER_SCRATCH_WORK, bcm2711_shader_rf(number), constant);
	if (error != 0)
		return error;
	error = bits_source(compiler, IMAGE_FLOAT_ONE, BCM2711_SHADER_SCRATCH_RIGHT, &constant);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_FLOAT_MINIMUM, BCM2711_SHADER_SCRATCH_WORK, work, constant);
	if (error != 0)
		return error;
	error = bits_source(compiler, scale, BCM2711_SHADER_SCRATCH_RIGHT, &constant);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_FLOAT_MULTIPLY, BCM2711_SHADER_SCRATCH_WORK, work, constant);
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_FLOAT_ROUND, BCM2711_SHADER_SCRATCH_WORK, work, bcm2711_shader_bits(0));
	if (error != 0)
		return error;

	/* The integer: an unorm's 0 to 255, an snorm's -127 to 127 in its low byte. */
	if (format == IMAGE_RGBA8_SNORM) {
		error = emit(compiler, BCM2711_QPU_FLOAT_TO_SIGNED, BCM2711_SHADER_SCRATCH_WORK, work, bcm2711_shader_bits(0));
		if (error != 0)
			return error;
		error = bits_source(compiler, 0xffU, BCM2711_SHADER_SCRATCH_RIGHT, &constant);
		if (error != 0)
			return error;
		error = emit(compiler, BCM2711_QPU_BITS_AND, BCM2711_SHADER_SCRATCH_WORK, work, constant);
		if (error != 0)
			return error;
	} else {
		error = emit(compiler, BCM2711_QPU_FLOAT_TO_UNSIGNED, BCM2711_SHADER_SCRATCH_WORK, work, bcm2711_shader_bits(0));
		if (error != 0)
			return error;
	}

	/* The byte placed in the word. */
	error = image_place(compiler, shift, word);
	if (error != 0)
		return error;

	/* Succeeded: the component is in the word. */
	return 0;
}

/* Places the bits in the work scratch register at bit `shift` of the word: the word itself at shift zero, ORed in after. */
static int
image_place(
	struct bcm2711_shader_compiler *compiler,
	uint32_t shift,
	uint32_t word)
{
	struct bcm2711_qpu_source work;
	int error;

	/* The first component starts the word. */
	work = bcm2711_shader_rf(BCM2711_SHADER_SCRATCH_WORK);
	if (shift == 0) {
		error = emit(compiler, BCM2711_QPU_MOVE, word, work, bcm2711_shader_bits(0));
		if (error != 0)
			return error;
		return 0;
	}

	/* A later one is shifted up and ORed in. */
	error = emit(compiler, BCM2711_QPU_SHIFT_LEFT, BCM2711_SHADER_SCRATCH_WORK, work, bcm2711_shader_bits(shift_bits(shift)));
	if (error != 0)
		return error;
	error = emit(compiler, BCM2711_QPU_BITS_OR, word, bcm2711_shader_rf(word), work);
	if (error != 0)
		return error;

	/* Succeeded: the bits are in the word. */
	return 0;
}
