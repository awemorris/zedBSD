/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPIR-V reader's interface: where the vertex inputs, varyings,
 * fragment colours and built-ins live, the reads at the entry and the
 * hand-over at the exit, and the packing and blending of a colour into
 * its attachment's pixel output.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/compiler/shader-ir.h"
#include "drivers/gpu/powervr/compiler/shader-private.h"
#include "drivers/gpu/powervr/compiler/shader-spirv.h"

/* The blend factors that are the same for every colour channel (the alpha and constant alpha ones), each worked out once. */
#define BLEND_FACTORS			(FACTOR_SOURCE_ALPHA_SATURATE + 1U)

struct blend_factors {
	struct powervr_ir_operand value[BLEND_FACTORS];
	uint8_t known[BLEND_FACTORS];
};

/*
 * How a colour attachment format's channels lie in its pixel output
 * registers: packed by one of the core's pack formats, as integer bit
 * fields the reader puts together, or a whole register a channel.
 */
#define PIXEL_PACKED			0U
#define PIXEL_FIELDS			1U
#define PIXEL_WORDS			2U

/* The range of a format's channels. */
#define PIXEL_FLOAT			0U
#define PIXEL_UNORM			1U
#define PIXEL_SNORM			2U
#define PIXEL_UINT			3U
#define PIXEL_SINT			4U

/* The bits of -1.0 as a float (the lower bound of a signed normalised channel). */
#define FLOAT_MINUS_ONE			0xbf800000U

/*
 * One colour attachment format the pixel output takes: its Vulkan
 * format, the pixel output registers a pixel of it fills, how its
 * channels are packed (with the pack format for PIXEL_PACKED), how many
 * channels it has and how wide each is in the registers (channel c from
 * bit c * bits on, across the registers; the last channel of a 10-bit
 * format is 2 bits), its channels' range, and whether the 8-bit and
 * half-float pack of the colour's first form (with the blends of the
 * sum of products) writes it.  The table is constant.
 */
struct pixel_format {
	uint8_t format;
	uint8_t registers;
	uint8_t kind;
	uint8_t code;
	uint8_t channels;
	uint8_t bits;
	uint8_t range;
	uint8_t classic;
};

/*
 * The colour attachment formats the pixel output takes, and what the
 * pixel back end reads from the registers for each.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_formats.c (the pbe
 * formats' accumulation formats: U8 and S8 for the 8-bit normalised
 * formats, UINT8 and SINT8, U16 and S16, UINT16 and SINT16, F16 for the
 * 16-bit float formats, the sRGB ones, A2B10G10R10_UNORM and
 * B10G11R11_UFLOAT, U1010102 for A2B10G10R10_UINT, UINT32, SINT32 and
 * F32), src/imagination/vulkan/pvr_formats.c
 * (pvr_get_pbe_accum_format_size_in_bytes: a pixel's bytes, so its
 * registers) and src/imagination/pco/pco_nir_pvfio.c (to_pbe_format:
 * A2B10G10R10_UNORM as four half floats, B10G11R11_UFLOAT as three, the
 * sRGB formats as four saturated; pack_to_format: the 32-bit channels
 * a register each, the integer channels as bit fields at the format's
 * channel offsets, the others packed by the pack format of their
 * channel width and type, as many to a register as fit); the Khronos
 * Vulkan 1.3 specification (VkFormat values; the packed formats'
 * channel bits).
 */
static const struct pixel_format pixel_formats[] = {
	{ 9, 1, PIXEL_PACKED, POWERVR_IR_PACK_U8888, 1, 8, PIXEL_UNORM, 0 },	/* R8_UNORM */
	{ 10, 1, PIXEL_PACKED, POWERVR_IR_PACK_S8888, 1, 8, PIXEL_SNORM, 0 },	/* R8_SNORM */
	{ 13, 1, PIXEL_FIELDS, 0, 1, 8, PIXEL_UINT, 0 },			/* R8_UINT */
	{ 14, 1, PIXEL_FIELDS, 0, 1, 8, PIXEL_SINT, 0 },			/* R8_SINT */
	{ 16, 1, PIXEL_PACKED, POWERVR_IR_PACK_U8888, 2, 8, PIXEL_UNORM, 0 },	/* R8G8_UNORM */
	{ 17, 1, PIXEL_PACKED, POWERVR_IR_PACK_S8888, 2, 8, PIXEL_SNORM, 0 },	/* R8G8_SNORM */
	{ 20, 1, PIXEL_FIELDS, 0, 2, 8, PIXEL_UINT, 0 },			/* R8G8_UINT */
	{ 21, 1, PIXEL_FIELDS, 0, 2, 8, PIXEL_SINT, 0 },			/* R8G8_SINT */
	{ 37, 1, PIXEL_PACKED, POWERVR_IR_PACK_U8888, 4, 8, PIXEL_UNORM, 1 },	/* R8G8B8A8_UNORM */
	{ 38, 1, PIXEL_PACKED, POWERVR_IR_PACK_S8888, 4, 8, PIXEL_SNORM, 0 },	/* R8G8B8A8_SNORM */
	{ 41, 1, PIXEL_FIELDS, 0, 4, 8, PIXEL_UINT, 0 },			/* R8G8B8A8_UINT */
	{ 42, 1, PIXEL_FIELDS, 0, 4, 8, PIXEL_SINT, 0 },			/* R8G8B8A8_SINT */
	{ 43, 2, PIXEL_PACKED, POWERVR_IR_PACK_F16F16, 4, 16, PIXEL_UNORM, 1 },	/* R8G8B8A8_SRGB */
	{ 44, 1, PIXEL_PACKED, POWERVR_IR_PACK_U8888, 4, 8, PIXEL_UNORM, 1 },	/* B8G8R8A8_UNORM */
	{ 50, 2, PIXEL_PACKED, POWERVR_IR_PACK_F16F16, 4, 16, PIXEL_UNORM, 1 },	/* B8G8R8A8_SRGB */
	{ 64, 2, PIXEL_PACKED, POWERVR_IR_PACK_F16F16, 4, 16, PIXEL_UNORM, 0 },	/* A2B10G10R10_UNORM_PACK32 */
	{ 68, 1, PIXEL_FIELDS, 0, 4, 10, PIXEL_UINT, 0 },			/* A2B10G10R10_UINT_PACK32 */
	{ 70, 1, PIXEL_PACKED, POWERVR_IR_PACK_U1616, 1, 16, PIXEL_UNORM, 0 },	/* R16_UNORM */
	{ 71, 1, PIXEL_PACKED, POWERVR_IR_PACK_S1616, 1, 16, PIXEL_SNORM, 0 },	/* R16_SNORM */
	{ 74, 1, PIXEL_FIELDS, 0, 1, 16, PIXEL_UINT, 0 },			/* R16_UINT */
	{ 75, 1, PIXEL_FIELDS, 0, 1, 16, PIXEL_SINT, 0 },			/* R16_SINT */
	{ 76, 1, PIXEL_PACKED, POWERVR_IR_PACK_F16F16, 1, 16, PIXEL_FLOAT, 0 },	/* R16_SFLOAT */
	{ 77, 1, PIXEL_PACKED, POWERVR_IR_PACK_U1616, 2, 16, PIXEL_UNORM, 0 },	/* R16G16_UNORM */
	{ 78, 1, PIXEL_PACKED, POWERVR_IR_PACK_S1616, 2, 16, PIXEL_SNORM, 0 },	/* R16G16_SNORM */
	{ 81, 1, PIXEL_FIELDS, 0, 2, 16, PIXEL_UINT, 0 },			/* R16G16_UINT */
	{ 82, 1, PIXEL_FIELDS, 0, 2, 16, PIXEL_SINT, 0 },			/* R16G16_SINT */
	{ 83, 1, PIXEL_PACKED, POWERVR_IR_PACK_F16F16, 2, 16, PIXEL_FLOAT, 0 },	/* R16G16_SFLOAT */
	{ 91, 2, PIXEL_PACKED, POWERVR_IR_PACK_U1616, 4, 16, PIXEL_UNORM, 0 },	/* R16G16B16A16_UNORM */
	{ 92, 2, PIXEL_PACKED, POWERVR_IR_PACK_S1616, 4, 16, PIXEL_SNORM, 0 },	/* R16G16B16A16_SNORM */
	{ 95, 2, PIXEL_FIELDS, 0, 4, 16, PIXEL_UINT, 0 },			/* R16G16B16A16_UINT */
	{ 96, 2, PIXEL_FIELDS, 0, 4, 16, PIXEL_SINT, 0 },			/* R16G16B16A16_SINT */
	{ 97, 2, PIXEL_PACKED, POWERVR_IR_PACK_F16F16, 4, 16, PIXEL_FLOAT, 1 },	/* R16G16B16A16_SFLOAT */
	{ 98, 1, PIXEL_WORDS, 0, 1, 32, PIXEL_UINT, 0 },			/* R32_UINT */
	{ 99, 1, PIXEL_WORDS, 0, 1, 32, PIXEL_SINT, 0 },			/* R32_SINT */
	{ 100, 1, PIXEL_WORDS, 0, 1, 32, PIXEL_FLOAT, 0 },			/* R32_SFLOAT */
	{ 101, 2, PIXEL_WORDS, 0, 2, 32, PIXEL_UINT, 0 },			/* R32G32_UINT */
	{ 102, 2, PIXEL_WORDS, 0, 2, 32, PIXEL_SINT, 0 },			/* R32G32_SINT */
	{ 103, 2, PIXEL_WORDS, 0, 2, 32, PIXEL_FLOAT, 0 },			/* R32G32_SFLOAT */
	{ 107, 4, PIXEL_WORDS, 0, 4, 32, PIXEL_UINT, 0 },			/* R32G32B32A32_UINT */
	{ 108, 4, PIXEL_WORDS, 0, 4, 32, PIXEL_SINT, 0 },			/* R32G32B32A32_SINT */
	{ 109, 4, PIXEL_WORDS, 0, 4, 32, PIXEL_FLOAT, 0 },			/* R32G32B32A32_SFLOAT */
	{ 122, 2, PIXEL_PACKED, POWERVR_IR_PACK_F16F16, 3, 16, PIXEL_FLOAT, 0 },	/* B10G11R11_UFLOAT_PACK32 */
};

static int reader_pixel_output(struct shader_spirv_reader *reader, const struct shader_spirv_id *variable, uint32_t words);
static struct powervr_ir_operand blend_factor(struct shader_spirv_reader *reader, uint32_t factor, uint32_t channel, const struct powervr_ir_operand *source, const struct powervr_ir_operand *destination, uint32_t constants, int *error);
static struct powervr_ir_operand blend_factor_kept(struct shader_spirv_reader *reader, struct blend_factors *kept, uint32_t factor, uint32_t channel, const struct powervr_ir_operand *source, const struct powervr_ir_operand *destination, uint32_t constants, int *error);
static struct powervr_ir_operand blend_sum(struct shader_spirv_reader *reader, uint32_t operation, const struct powervr_ir_operand *source, const struct powervr_ir_operand *source_factor, const struct powervr_ir_operand *destination, const struct powervr_ir_operand *destination_factor);
static int is_constant(const struct powervr_ir_operand *operand, uint32_t bits);
static int blend_sum_kind(const struct powervr_shader_blend *blend, uint32_t *kind);
static struct powervr_ir_operand saturate(struct shader_spirv_reader *reader, struct powervr_ir_operand x);
static int attribute_words(struct shader_spirv_reader *reader, uint32_t location, uint32_t words, uint32_t *format_words);
static uint32_t attribute_one(const struct shader_spirv_reader *reader, uint32_t location);
static const struct pixel_format *pixel_format_find(uint32_t format);
static int reader_pixel_output_packed(struct shader_spirv_reader *reader, const struct shader_spirv_id *variable, uint32_t words, const struct pixel_format *layout);
static int blend_colour(struct shader_spirv_reader *reader, const struct powervr_shader_blend *blend, const struct powervr_ir_operand *source, const struct powervr_ir_operand *destination, struct powervr_ir_operand *result);
static struct powervr_ir_operand pixel_clamp(struct shader_spirv_reader *reader, struct powervr_ir_operand x, uint32_t range);
static void pixel_unpack(struct shader_spirv_reader *reader, const struct pixel_format *layout, const struct powervr_ir_operand *raw, struct powervr_ir_operand *destination);
static struct powervr_ir_operand pixel_pack_word(struct shader_spirv_reader *reader, const struct pixel_format *layout, uint32_t word, const struct powervr_ir_operand *colour);
static uint32_t pixel_word_bits(const struct pixel_format *layout, uint32_t word, uint32_t channels);
static uint32_t pixel_word_place(uint32_t word, uint32_t tile_request);
static int fragment_only_tiles(const struct powervr_shader_program *program);
static uint32_t fragment_depth(const struct shader_spirv_reader *reader);
static void pixel_channel_place(const struct pixel_format *layout, uint32_t channel, uint32_t *word, uint32_t *shift, uint32_t *width);
static int module_kills(const struct shader_spirv_reader *reader);
static int compute_local(struct shader_spirv_reader *reader);
static int fragment_built_in(struct shader_spirv_reader *reader, const struct shader_spirv_id *id, uint32_t words);
static int reads_built_in(const struct shader_spirv_reader *reader, uint32_t builtin);
static uint32_t point_size_word(struct shader_spirv_reader *reader, const struct shader_spirv_id *variable);
static int compute_shared(struct shader_spirv_reader *reader, uint32_t invocations);
static int module_has(const struct shader_spirv_reader *reader, uint32_t wanted);

/*
 * A test switch (0 as built): a kill does nothing, so a shader that
 * discards draws opaque and its draws do not pass punch-through, to
 * see on the board what the punch-through pass costs.  Wrong images;
 * for timing only.
 */
#ifndef POWERVR_SHADER_IGNORE_KILL_TRIAL
#define POWERVR_SHADER_IGNORE_KILL_TRIAL	0
#endif

/*
 * Places the interface variables: each register-class variable gets
 * values for its words, vertex inputs their registers, varyings their
 * output words or coefficient sets.
 */
int
shader_spirv_reader_layout_interface(
	struct shader_spirv_reader *reader)
{
	struct powervr_shader_program *program;
	struct shader_spirv_id *id;
	uint32_t location;
	uint32_t best;
	uint32_t index;
	uint32_t words;
	uint32_t next_input;
	uint32_t next_output;
	uint32_t next_coefficient;
	uint32_t any_smooth;
	uint32_t done[SHADER_SPIRV_IDS / 32U];
	uint32_t entry;
	struct powervr_shader_pixel_place pixel_places[POWERVR_SHADER_OUTPUTS];
	uint32_t pixel_registers;
	uint32_t pixel_tile_buffers;
	uint32_t tile_request;
	int pixel_laid;
	int error;
	uint32_t scalars;

	/* Gives every register-class global its words as values. */
	program = reader->program;
	for (index = 1; index < reader->bound; index++) {
		id = &reader->ids[index];
		if (id->kind != ID_POINTER || id->variable != index)
			continue;
		if (id->storage != STORAGE_INPUT && id->storage != STORAGE_OUTPUT && id->storage != STORAGE_PRIVATE)
			continue;
		words = shader_spirv_scalar_count(reader, id->type);
		if (words == 0 || words > COMPOSITE_MAX * 2U)
			return ENOTSUP;
		id->first = shader_spirv_new_value(reader, 1);
		for (location = 1; location < words; location++)
			(void)shader_spirv_new_value(reader, 1);
		if (reader->error != 0)
			return reader->error;
	}

	/* Does a fragment shader iterate anything with perspective? */
	any_smooth = 0;
	for (index = 1; index < reader->bound; index++) {
		id = &reader->ids[index];
		if (id->kind == ID_POINTER &&
		    id->variable == index &&
		    id->storage == STORAGE_INPUT &&
		    (id->flags & FLAG_LOCATION) != 0 &&
		    (id->flags & FLAG_FLAT) == 0)
			any_smooth = 1;
	}

	/* The w coefficients come first when a varying needs them. */
	next_coefficient = 0;
	if (reader->stage == POWERVR_SHADER_FRAGMENT && any_smooth != 0) {
		program->uses_w = 1;
		program->w_coefficient = 0;
		next_coefficient = 4;
	}

	/*
	 * A point's coordinates come next, two coefficient sets, when the
	 * fragment shader reads them.
	 *
	 * Procedure referenced:
	 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_pipeline.c
	 * (pvr_alloc_fs_varyings: the point coordinates after z and w, before
	 * the varyings; the data sequencer iterates them as a point sprite's).
	 */
	if (reader->stage == POWERVR_SHADER_FRAGMENT && reads_built_in(reader, BUILT_IN_POINT_COORD)) {
		program->point_coord_coefficient = next_coefficient;
		next_coefficient += 8U;
	}

	/* Places located inputs and outputs in location order. */
	for (index = 0; index < SHADER_SPIRV_IDS / 32U; index++)
		done[index] = 0;
	next_input = 0;
	next_output = 4;
	pixel_laid = 0;
	for (index = 0; index < POWERVR_SHADER_OUTPUTS; index++)
		reader->pixel_tile_request[index] = POWERVR_SHADER_NO_REG;
	for (;;) {
		/* Finds the unplaced located interface variable of the lowest location. */
		best = 0;
		for (index = 1; index < reader->bound; index++) {
			id = &reader->ids[index];
			if (id->kind != ID_POINTER || id->variable != index || (id->flags & FLAG_LOCATION) == 0)
				continue;
			if (id->storage != STORAGE_INPUT && id->storage != STORAGE_OUTPUT)
				continue;
			if ((done[index / 32U] & (1U << (index % 32U))) != 0)
				continue;
			if (best == 0 || id->location < reader->ids[best].location)
				best = index;
		}
		if (best == 0)
			break;

		/* Places it by stage and direction. */
		done[best / 32U] |= 1U << (best % 32U);
		id = &reader->ids[best];
		words = shader_spirv_scalar_count(reader, id->type);
		entry = 0;
		if (id->storage == STORAGE_INPUT && reader->stage == POWERVR_SHADER_VERTEX) {
			if (program->input_count >= POWERVR_SHADER_INPUTS)
				return ENOTSUP;
			entry = program->input_count++;
			id->words = words;
			error = attribute_words(reader, id->location, words, &id->words);
			if (error != 0)
				return error;
			program->inputs[entry].location = id->location;
			program->inputs[entry].components = id->words;
			program->inputs[entry].reg = next_input;
			id->interface = next_input;
			next_input += id->words;
		} else if (id->storage == STORAGE_OUTPUT && reader->stage == POWERVR_SHADER_VERTEX) {
			if (program->varying_count >= POWERVR_SHADER_VARYINGS)
				return ENOTSUP;
			entry = program->varying_count++;
			program->varyings[entry].location = id->location;
			program->varyings[entry].components = words;
			program->varyings[entry].flat = (id->flags & FLAG_FLAT) != 0;
			program->varyings[entry].reg = next_output;
			id->interface = next_output;
			next_output += words;
		} else if (id->storage == STORAGE_INPUT && reader->stage == POWERVR_SHADER_FRAGMENT) {
			if (program->varying_count >= POWERVR_SHADER_VARYINGS)
				return ENOTSUP;
			entry = program->varying_count++;
			program->varyings[entry].location = id->location;
			program->varyings[entry].components = words;
			program->varyings[entry].flat = (id->flags & FLAG_FLAT) != 0;
			program->varyings[entry].linear = id->linear != 0 && (id->flags & FLAG_FLAT) == 0;
			program->varyings[entry].reg = next_coefficient;
			id->interface = next_coefficient;
			next_coefficient += words * 4U;
		} else if (id->storage == STORAGE_OUTPUT && reader->stage == POWERVR_SHADER_FRAGMENT) {
			if (program->output_count >= POWERVR_SHADER_OUTPUTS || words > 4)
				return ENOTSUP;
			if (reader->key == NULL) {
				/* Floats in temporaries, by location. */
				entry = program->output_count++;
				program->outputs[entry].location = id->location;
				program->outputs[entry].tile_buffer = POWERVR_SHADER_NO_REG;
				id->interface = id->location * 4U;
			} else if (id->location >= POWERVR_SHADER_OUTPUTS || reader->key->output_formats[id->location] == 0) {
				/* No attachment: the colour is dropped. */
				id->interface = UINT32_MAX;
			} else {
				/* The attachments' places, laid out once from the key's formats. */
				if (!pixel_laid) {
					error = powervr_shader_pixel_places(reader->key->output_formats, pixel_places, &pixel_registers, &pixel_tile_buffers);
					if (error != 0)
						return error;
					pixel_laid = 1;
				}

				/* A packed colour in its attachment's registers or tile buffer words; refuses a format the compiler does not pack. */
				if (pixel_places[id->location].reg == POWERVR_SHADER_NO_REG)
					return ENOTSUP;
				entry = program->output_count++;
				program->outputs[entry].location = id->location;
				program->outputs[entry].format = reader->key->output_formats[id->location];
				program->outputs[entry].reg = pixel_places[id->location].reg;
				program->outputs[entry].tile_buffer = pixel_places[id->location].tile_buffer;
				id->interface = pixel_places[id->location].reg;

				/* A tile buffer's words: its address in shared registers, from the driver. */
				if (pixel_places[id->location].tile_buffer != POWERVR_SHADER_NO_REG) {
					error = shader_spirv_request_add(reader, POWERVR_SHADER_REQUEST_TILE_BUFFER, 0, 0, pixel_places[id->location].tile_buffer, 2, &tile_request);
					if (error != 0)
						return error;
					reader->pixel_tile_request[id->location] = tile_request;
				}
			}
		} else {
			/* A located compute variable is not a thing. */
			return ENOTSUP;
		}
	}

	/*
	 * The vertex and instance index, each one vertex input register after
	 * the attributes, written by the driver's data sequencer program.
	 *
	 * Procedure referenced:
	 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_pipeline.c
	 * (pvr_alloc_vs_sysvals, pvr_pds_vertex_attrib_programs_create: the
	 * vertex id and instance id are one vertex input register each, the
	 * data sequencer told their registers) and
	 * src/imagination/pco/pco_nir.c (the compiler options: the instance
	 * id includes the base instance, the vertex id is not zero based).
	 */
	if (reader->stage == POWERVR_SHADER_VERTEX) {
		for (index = 1; index < reader->bound; index++) {
			id = &reader->ids[index];
			if (id->kind != ID_POINTER || id->variable != index || id->storage != STORAGE_INPUT)
				continue;
			if ((id->flags & FLAG_BUILT_IN) == 0)
				continue;
			if (id->builtin != BUILT_IN_VERTEX_INDEX && id->builtin != BUILT_IN_INSTANCE_INDEX)
				continue;
			scalars = shader_spirv_scalar_count(reader, id->type);
			if (scalars != 1)
				return ENOTSUP;
			id->interface = next_input;
			if (id->builtin == BUILT_IN_VERTEX_INDEX)
				program->vertex_index_reg = next_input;
			else
				program->instance_index_reg = next_input;
			next_input++;
		}
	}

	/* A point's size, the last vertex output word, when the draws are points. */
	if (reader->stage == POWERVR_SHADER_VERTEX && reader->key != NULL && (reader->key->forms & POWERVR_SHADER_FORM_POINT_SIZE) != 0) {
		program->point_size_reg = next_output;
		next_output++;
	}

	/* Records the register counts of the interface. */
	program->vertex_inputs = next_input;
	program->vertex_outputs = next_output;
	if (reader->stage != POWERVR_SHADER_VERTEX)
		program->vertex_outputs = 0;
	program->coefficients = next_coefficient;

	/* Succeeded: the interface is placed. */
	return 0;
}

/*
 * Reads the stage's inputs into the input variables' values before the
 * first block, so every later read is a plain copy.
 */
int
shader_spirv_reader_entry_prologue(
	struct shader_spirv_reader *reader)
{
	struct powervr_ir_operand group;
	struct powervr_ir_operand base;
	struct powervr_ir_operand size;
	struct powervr_ir_operand word;
	struct shader_spirv_id *id;
	uint32_t index;
	uint32_t component;
	uint32_t words;
	uint32_t request;
	uint32_t base_request;
	uint32_t sum;
	uint32_t builtin;
	uint32_t invocations;
	uint32_t point_size;
	int error;

	/* Reads each input variable's words. */
	for (index = 1; index < reader->bound; index++) {
		id = &reader->ids[index];
		if (id->kind != ID_POINTER || id->variable != index || id->storage != STORAGE_INPUT)
			continue;
		words = shader_spirv_scalar_count(reader, id->type);

		/* A located input: a vertex input register or an iterated varying. */
		if ((id->flags & FLAG_LOCATION) != 0) {
			if (id->linear != 0 && (id->flags & FLAG_FLAT) == 0 && reader->stage == POWERVR_SHADER_FRAGMENT) {
				/* A varying without perspective, iterated linearly. */
				for (component = 0; component < words; component++) {
					shader_spirv_emit(reader, POWERVR_IR_ITERATE, id->first + component, NULL, NULL, NULL, id->interface + component * 4U, 0);
					reader->shader->instructions[reader->shader->instruction_count - 1U].flags = POWERVR_IR_LINEAR;
				}
				continue;
			}
			for (component = 0; component < words; component++) {
				if (reader->stage == POWERVR_SHADER_VERTEX && component >= id->words) {
					/* A word the attribute's format lacks: 0, or 1 for w (an integer 1 for an integer format). */
					word = shader_spirv_constant_operand(component == 3 ? attribute_one(reader, id->location) : 0);
					shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first + component, &word, NULL, NULL, 0, 0);
				} else if (reader->stage == POWERVR_SHADER_VERTEX) {
					shader_spirv_emit(reader, POWERVR_IR_INPUT, id->first + component, NULL, NULL, NULL, id->interface + component, 0);
				} else {
					shader_spirv_emit(reader, POWERVR_IR_ITERATE, id->first + component, NULL, NULL, NULL, id->interface + component * 4U, 0);
					if ((id->flags & FLAG_FLAT) != 0)
						reader->shader->instructions[reader->shader->instruction_count - 1U].flags = POWERVR_IR_FLAT;
				}
			}
			continue;
		}

		/* Refuses an input that is neither located nor a known built-in. */
		if ((id->flags & FLAG_BUILT_IN) == 0)
			return ENOTSUP;
		builtin = id->builtin;

		/* The vertex or instance index, in the register the layout gave it. */
		if (reader->stage == POWERVR_SHADER_VERTEX && (builtin == BUILT_IN_VERTEX_INDEX || builtin == BUILT_IN_INSTANCE_INDEX)) {
			shader_spirv_emit(reader, POWERVR_IR_INPUT, id->first, NULL, NULL, NULL, id->interface, 0);
			continue;
		}

		/* The view a multiview render renders, a word the draw's shared registers hold. */
		if (builtin == BUILT_IN_VIEW_INDEX && (reader->stage == POWERVR_SHADER_VERTEX || reader->stage == POWERVR_SHADER_FRAGMENT)) {
			error = shader_spirv_request_add(reader, POWERVR_SHADER_REQUEST_VIEW_INDEX, 0, 0, 0, 1, &request);
			if (error != 0)
				return error;
			word = shader_spirv_shared_word(reader, request, 0);
			shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first, &word, NULL, NULL, 0, 0);
			continue;
		}

		/*
		 * A subgroup is one invocation in every stage: its size 1, the
		 * invocation its first, and its ballots the invocation's bit
		 * (bit 0 of the first word) alone, from and up to it and none
		 * before or after it.
		 */
		if (builtin == BUILT_IN_SUBGROUP_SIZE ||
		    builtin == BUILT_IN_SUBGROUP_LOCAL_INVOCATION_ID ||
		    (builtin >= BUILT_IN_SUBGROUP_EQ_MASK && builtin <= BUILT_IN_SUBGROUP_LT_MASK)) {
			for (component = 0; component < words; component++) {
				word = shader_spirv_constant_operand(0);
				if (builtin == BUILT_IN_SUBGROUP_SIZE)
					word = shader_spirv_constant_operand(1);
				if (component == 0 &&
				    (builtin == BUILT_IN_SUBGROUP_EQ_MASK ||
				     builtin == BUILT_IN_SUBGROUP_GE_MASK ||
				     builtin == BUILT_IN_SUBGROUP_LE_MASK))
					word = shader_spirv_constant_operand(1);
				shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first + component, &word, NULL, NULL, 0, 0);
			}
			continue;
		}

		/* A fragment's built-in inputs. */
		if (reader->stage == POWERVR_SHADER_FRAGMENT) {
			error = fragment_built_in(reader, id, words);
			if (error != 0)
				return error;
			continue;
		}
		if (reader->stage != POWERVR_SHADER_COMPUTE)
			return ENOTSUP;

		/* The compute built-ins, built from the local index and the workgroup. */
		error = compute_local(reader);
		if (error != 0)
			return error;
		if (builtin == BUILT_IN_LOCAL_INVOCATION_INDEX || builtin == BUILT_IN_SUBGROUP_ID) {
			/* The local index; each invocation is a subgroup of its own, so its subgroup's id too. */
			shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first, &reader->local[3], NULL, NULL, 0, 0);
		} else if (builtin == BUILT_IN_NUM_SUBGROUPS) {
			/* A subgroup is one invocation: as many subgroups as the workgroup's invocations. */
			word = shader_spirv_constant_operand(reader->program->workgroup[0] * reader->program->workgroup[1] * reader->program->workgroup[2]);
			shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first, &word, NULL, NULL, 0, 0);
		} else if (builtin == BUILT_IN_LOCAL_INVOCATION_ID) {
			for (component = 0; component < words && component < 3; component++)
				shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first + component, &reader->local[component], NULL, NULL, 0, 0);
		} else if (builtin == BUILT_IN_WORKGROUP_ID || builtin == BUILT_IN_NUM_WORKGROUPS || builtin == BUILT_IN_GLOBAL_INVOCATION_ID) {
			/* The workgroup id or count, three words in shared registers. */
			error = shader_spirv_request_add(reader, builtin == BUILT_IN_NUM_WORKGROUPS ? POWERVR_SHADER_REQUEST_NUM_WORKGROUPS : POWERVR_SHADER_REQUEST_WORKGROUP_ID, 0, 0, 0, 3, &request);
			if (error != 0)
				return error;
			base_request = 0;
			if (builtin != BUILT_IN_NUM_WORKGROUPS && reader->key != NULL && (reader->key->forms & POWERVR_SHADER_FORM_DISPATCH_BASE) != 0) {
				error = shader_spirv_request_add(reader, POWERVR_SHADER_REQUEST_WORKGROUP_BASE, 0, 0, 0, 3, &base_request);
				if (error != 0)
					return error;
				base_request++;
			}
			for (component = 0; component < words && component < 3; component++) {
				group = shader_spirv_shared_word(reader, request, component);

				/* A dispatch's ids count from its first workgroup. */
				if (base_request != 0) {
					base = shader_spirv_shared_word(reader, base_request - 1U, component);
					sum = shader_spirv_new_value(reader, 1);
					shader_spirv_emit(reader, POWERVR_IR_INTEGER_ADD, sum, &group, &base, NULL, 0, 0);
					group = shader_spirv_value_operand(sum, 0);
				}
				if (builtin != BUILT_IN_GLOBAL_INVOCATION_ID) {
					shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first + component, &group, NULL, NULL, 0, 0);
					continue;
				}

				/* A global id is the group times the workgroup's size plus the local id, on each axis. */
				size = shader_spirv_constant_operand(reader->program->workgroup[component]);
				shader_spirv_emit(reader, POWERVR_IR_INTEGER_MULTIPLY, id->first + component, &group, &size, NULL, 0, 0);
				word = shader_spirv_value_operand(id->first + component, 0);
				shader_spirv_emit(reader, POWERVR_IR_INTEGER_ADD, id->first + component, &word, &reader->local[component], NULL, 0, 0);
			}
		} else {
			return ENOTSUP;
		}
	}

	/* Clears the output variables, so an unwritten word is zero rather than stale (a point's size one). */
	for (index = 1; index < reader->bound; index++) {
		id = &reader->ids[index];
		if (id->kind != ID_POINTER || id->variable != index)
			continue;
		if (id->storage != STORAGE_OUTPUT && id->storage != STORAGE_PRIVATE)
			continue;
		words = shader_spirv_scalar_count(reader, id->type);
		point_size = point_size_word(reader, id);
		for (component = 0; component < words; component++) {
			word = shader_spirv_constant_operand(component == point_size ? FLOAT_ONE : 0U);
			shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first + component, &word, NULL, NULL, 0, 0);
		}
	}

	/*
	 * A fragment shader that discards keeps its discard in a value of its
	 * own, false from the start; a kill makes it true and the end gives
	 * it to the depth and visibility unit (see the epilogue).
	 *
	 * Procedure referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_nir_pvfio.c (lower_isp_fb and
	 * its discard_cond_reg: false before the first block, true at each
	 * demote, a discard being a demote there (pco_nir.c
	 * discard_is_demote), read by the one isp_feedback_pco).
	 */
	if (reader->stage == POWERVR_SHADER_FRAGMENT && !POWERVR_SHADER_IGNORE_KILL_TRIAL && module_kills(reader)) {
		reader->discard = shader_spirv_new_value(reader, 1);
		word = shader_spirv_constant_operand(0);
		shader_spirv_emit(reader, POWERVR_IR_MOVE, reader->discard, &word, NULL, NULL, 0, 0);
		reader->program->discards = 1;
	}

	/*
	 * A compute workgroup whose size is not whole tasks: the executor
	 * starts whole tasks, and the instances past the workgroup run
	 * nothing (the epilogue ends the check).
	 *
	 * Procedure referenced:
	 * Mesa 26.2.6 src/imagination/pco/pco_nir_compute.c
	 * (pco_nir_compute_instance_check: the entry point inside an if of
	 * the local index below the workgroup's size) and
	 * src/imagination/vulkan/pvr_arch_cmd_buffer.c
	 * (pvr_compute_update_kernel: a workgroup padded to whole tasks).
	 */
	if (reader->stage == POWERVR_SHADER_COMPUTE) {
		invocations = reader->program->workgroup[0] * reader->program->workgroup[1] * reader->program->workgroup[2];
		if (invocations == 0)
			return ENOTSUP;
		error = compute_shared(reader, invocations);
		if (error != 0)
			return error;
		if (invocations % POWERVR_SHADER_TASK_INSTANCES != 0) {
			error = compute_local(reader);
			if (error != 0)
				return error;
			size = shader_spirv_constant_operand(invocations);
			word = shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_LESS, &reader->local[3], &size, NULL);
			shader_spirv_emit(reader, POWERVR_IR_IF, 0, &word, NULL, NULL, 0, 0);
			reader->instance_check = 1;
		}
	}

	/* Reports what the reads came to. */
	return reader->error;
}

/*
 * Hands the output variables' words over after the last block: the
 * vertex position and varyings to their output words, the fragment
 * colours to their outputs.
 */
int
shader_spirv_reader_entry_epilogue(
	struct shader_spirv_reader *reader)
{
	struct powervr_ir_operand word;
	struct shader_spirv_member *member;
	struct shader_spirv_id *type;
	struct shader_spirv_id *id;
	uint32_t index;
	uint32_t member_index;
	uint32_t component;
	uint32_t words;
	uint32_t point_size;
	uint32_t kept;
	uint32_t depth;
	uint32_t memory_depth;
	uint32_t feedback;
	struct powervr_ir_operand discard;
	struct powervr_ir_operand clamped;
	struct powervr_ir_operand bound;
	int error;

	/* A compute shader's body ends where its instance check does. */
	if (reader->instance_check != 0) {
		shader_spirv_emit(reader, POWERVR_IR_END_IF, 0, NULL, NULL, NULL, 0, 0);
		reader->instance_check = 0;
	}

	/*
	 * The discard and the depth the shader wrote, before the colours, as
	 * the feedback that keeps or drops the fragment and gives its depth
	 * (gl_FragDepth clamped to [0, 1]).
	 *
	 * Procedure referenced:
	 * Mesa 26.2.5 src/imagination/pco/pco_nir_pvfio.c (lower_pfo: the
	 * FRAG_RESULT_DEPTH store's value through fsat as the depth feedback
	 * source; lower_isp_fb: the isp_feedback_pco of the discard and the
	 * depth before the first output store).
	 */
	depth = fragment_depth(reader);
	memory_depth = 0;

	/*
	 * A fragment shader that writes memory and whose tests come after it
	 * gives the iterated depth as its feedback too, so that its draws pass
	 * depth feedback: the core then runs it for every fragment in the
	 * draws' order, and none is dropped by the hidden surface removal
	 * before its stores and atomics run.  The depth is the fragment's
	 * position's z, iterated without perspective from a coefficient set
	 * of its own after the others.
	 *
	 * Procedure referenced:
	 * Mesa 26.2.6 src/imagination/pco/pco_nir_pvfio.c (lower_isp_fb: a
	 * shader that writes memory and gives no depth takes the position's
	 * z, noperspective, as the depth feedback source);
	 * src/imagination/vulkan/pvr_arch_pipeline.c (pvr_fragment_state_save:
	 * depth feedback unless the shader has early fragment tests).
	 */
	if (reader->stage == POWERVR_SHADER_FRAGMENT &&
	    reader->program->stores != 0 &&
	    depth == 0 &&
	    reader->early_fragment_tests == 0) {
		reader->program->uses_z = 1;
		reader->program->z_coefficient = reader->program->coefficients;
		reader->program->coefficients += 4U;
		memory_depth = shader_spirv_new_value(reader, 1);
		shader_spirv_emit(reader, POWERVR_IR_ITERATE, memory_depth, NULL, NULL, NULL, reader->program->z_coefficient, 0);
		reader->shader->instructions[reader->shader->instruction_count - 1U].flags = POWERVR_IR_LINEAR;
	}

	if (reader->discard != 0 || depth != 0 || memory_depth != 0) {
		feedback = 0;
		discard = shader_spirv_constant_operand(0U);
		if (reader->discard != 0) {
			feedback |= POWERVR_IR_FEEDBACK_DISCARD;
			discard = shader_spirv_value_operand(reader->discard, 0);
		}
		clamped = shader_spirv_constant_operand(0U);
		if (depth != 0) {
			feedback |= POWERVR_IR_FEEDBACK_DEPTH;
			word = shader_spirv_value_operand(depth, 0);
			bound = shader_spirv_constant_operand(0U);
			clamped = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MAXIMUM, &word, &bound, NULL);
			bound = shader_spirv_constant_operand(FLOAT_ONE);
			clamped = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &clamped, &bound, NULL);
			reader->program->depth_feedback = 1;
		}
		if (memory_depth != 0) {
			feedback |= POWERVR_IR_FEEDBACK_DEPTH;
			clamped = shader_spirv_value_operand(memory_depth, 0);
			reader->program->depth_feedback = 1;
		}
		shader_spirv_emit(reader, POWERVR_IR_FEEDBACK, 0, &discard, &clamped, NULL, feedback, 0);
	}

	/* A point's size: the shader's, or one when it has none. */
	if (reader->program->point_size_reg != POWERVR_SHADER_NO_REG) {
		word = shader_spirv_constant_operand(FLOAT_ONE);
		for (index = 1; index < reader->bound; index++) {
			id = &reader->ids[index];
			if (id->kind != ID_POINTER || id->variable != index || id->storage != STORAGE_OUTPUT)
				continue;
			point_size = point_size_word(reader, id);
			if (point_size != UINT32_MAX)
				word = shader_spirv_value_operand(id->first + point_size, 0);
		}
		shader_spirv_emit(reader, POWERVR_IR_OUTPUT, 0, &word, NULL, NULL, reader->program->point_size_reg, 0);
	}

	/* Each output variable. */
	for (index = 1; index < reader->bound; index++) {
		id = &reader->ids[index];
		if (id->kind != ID_POINTER || id->variable != index || id->storage != STORAGE_OUTPUT)
			continue;
		words = shader_spirv_scalar_count(reader, id->type);

		/* A colour packed into its attachment's pixel output, blended. */
		if ((id->flags & FLAG_LOCATION) != 0 && reader->stage == POWERVR_SHADER_FRAGMENT && reader->key != NULL) {
			if (id->interface == UINT32_MAX)
				continue;
			error = reader_pixel_output(reader, id, words);
			if (error != 0)
				return error;
			continue;
		}

		/* A located output: a varying's or a colour's words. */
		if ((id->flags & FLAG_LOCATION) != 0) {
			for (component = 0; component < words; component++) {
				word = shader_spirv_value_operand(id->first + component, 0);
				shader_spirv_emit(reader, POWERVR_IR_OUTPUT, 0, &word, NULL, NULL, id->interface + component, 0);
			}
			continue;
		}

		/* A built-in position, alone or in the per-vertex block. */
		if ((id->flags & FLAG_BUILT_IN) != 0) {
			if (id->builtin != BUILT_IN_POSITION || reader->stage != POWERVR_SHADER_VERTEX)
				continue;
			for (component = 0; component < 4; component++) {
				word = shader_spirv_value_operand(id->first + component, 0);
				shader_spirv_emit(reader, POWERVR_IR_OUTPUT, 0, &word, NULL, NULL, component, 0);
			}
			continue;
		}

		/* The per-vertex block: its position member (the point size is above); clip distances are dropped. */
		type = &reader->ids[id->type];
		if (type->type_kind != TYPE_STRUCT)
			return ENOTSUP;
		for (member_index = 0; member_index < type->count; member_index++) {
			member = &reader->members[type->first + member_index];
			if (member->has_builtin == 0 || member->builtin != BUILT_IN_POSITION)
				continue;
			if (reader->stage != POWERVR_SHADER_VERTEX)
				return ENOTSUP;
			for (component = 0; component < 4; component++) {
				word = shader_spirv_value_operand(id->first + member->scalar_offset + component, 0);
				shader_spirv_emit(reader, POWERVR_IR_OUTPUT, 0, &word, NULL, NULL, component, 0);
			}
		}
	}

	/*
	 * A fragment program whose colours all went to tile buffers still
	 * writes a pixel output register, as it is, under the overlap check.
	 *
	 * Procedure referenced:
	 * Mesa 26.2.6 src/imagination/vulkan/pvr_usc.c (pvr_uscgen_loadop
	 * and pvr_uscgen_clear_attach: nir_dummy_load_store_pco when no
	 * render target is an output register) and
	 * src/imagination/pco/pco_trans_nir.c (dummy_load_store_pco: a move
	 * of pixel output 0 to itself with olchk).
	 */
	if (reader->stage == POWERVR_SHADER_FRAGMENT && reader->key != NULL && fragment_only_tiles(reader->program)) {
		kept = shader_spirv_new_value(reader, 1);
		shader_spirv_emit(reader, POWERVR_IR_PIXEL_LOAD, kept, NULL, NULL, NULL, 0, POWERVR_IR_PIXEL_WORD);
		word = shader_spirv_value_operand(kept, 0);
		shader_spirv_emit(reader, POWERVR_IR_PIXEL_STORE, 0, &word, NULL, NULL, 0, POWERVR_IR_PIXEL_WORD);
	}

	/* Reports what the hand-over came to. */
	return reader->error;
}

/*
 * Finds the value a fragment shader's gl_FragDepth output holds at the
 * end: its variable's word, or 0 for a shader that has none (or is not a
 * fragment shader).
 */
static uint32_t
fragment_depth(
	const struct shader_spirv_reader *reader)
{
	const struct shader_spirv_id *id;
	uint32_t index;

	/* Only a fragment shader's built-in output. */
	if (reader->stage != POWERVR_SHADER_FRAGMENT)
		return 0;
	for (index = 1; index < reader->bound; index++) {
		id = &reader->ids[index];
		if (id->kind != ID_POINTER || id->variable != index || id->storage != STORAGE_OUTPUT)
			continue;
		if ((id->flags & FLAG_BUILT_IN) != 0 && id->builtin == BUILT_IN_FRAG_DEPTH)
			return id->first;
	}

	/* None. */
	return 0;
}

/* Says whether a fragment program's colours are all in tile buffers (and it has one). */
static int
fragment_only_tiles(
	const struct powervr_shader_program *program)
{
	uint32_t entry;
	int tiles;

	/* Any colour in a register says no; at least one in a tile buffer is needed. */
	tiles = 0;
	for (entry = 0; entry < program->output_count; entry++) {
		if (program->outputs[entry].tile_buffer == POWERVR_SHADER_NO_REG)
			return 0;
		tiles = 1;
	}

	/* Succeeded: whether only tile buffers are written. */
	return tiles;
}

/*
 * Lays out the colour attachments' pixel output registers from their
 * formats (0 for none) as powervr_shader_pixel_places does, refusing
 * (ENOTSUP) attachments that would take a tile buffer: each
 * attachment's first register (POWERVR_SHADER_NO_REG for none and for a
 * format the compiler does not pack) and the registers the pixel uses.
 */
int
powervr_shader_pixel_layout(
	const uint32_t formats[POWERVR_SHADER_OUTPUTS],
	uint32_t regs[POWERVR_SHADER_OUTPUTS],
	uint32_t *registers)
{
	struct powervr_shader_pixel_place places[POWERVR_SHADER_OUTPUTS];
	uint32_t tile_buffers;
	uint32_t location;
	int error;

	/* Refuses missing tables. */
	if (formats == NULL || regs == NULL || registers == NULL)
		return EINVAL;

	/* The places, the registers alone. */
	error = powervr_shader_pixel_places(formats, places, registers, &tile_buffers);
	if (error != 0)
		return error;

	if (tile_buffers != 0)
		return ENOTSUP;

	/* Each attachment's first register. */
	for (location = 0; location < POWERVR_SHADER_OUTPUTS; location++)
		regs[location] = places[location].reg;

	/* Succeeded: the layout. */
	return 0;
}

/*
 * Places the colour attachments from their formats (0 for none), in
 * attachment order: each takes the first free run of its registers, a
 * run of two or four starting at an even register, among the four a
 * pixel has, else the first such run among the same words of the tile
 * buffers taken so far, else the first words of a new tile buffer.
 * Gives each attachment its place (reg POWERVR_SHADER_NO_REG for none
 * and for a format the compiler does not pack), the registers the pixel
 * uses (up to the furthest word placed, in a register or a tile buffer:
 * the end of the tile loads a buffer's words into the registers for
 * the pixel back end) and the tile buffers taken.  ENOTSUP when even the
 * tile buffers have no room.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_mrt.c (pvr_alloc_mrt and
 * pvr_mrt_alloc_from_buffer: the render targets in order, each in the
 * first free run of the output registers, a 64-bit or 128-bit one at an
 * even register on a core with pbe2_in_xe, else in the first tile buffer
 * with such a run, else at word 0 of a new one, VK_ERROR_TOO_MANY_OBJECTS
 * past PVR_MAX_TILE_BUFFER_COUNT; the output registers used the furthest
 * end of either; pvr_arch_init_usc_mrt_setup: the tile buffers' count);
 * src/imagination/include/hwdef/rogue_hw_utils.h
 * (rogue_get_max_output_regs_per_pixel: 4 without eight_output_registers).
 * Values referenced:
 * Mesa 26.2.6 src/imagination/common/device_info/bxe-4-32.h
 * (has_pbe2_in_xe, no eight_output_registers).
 */
int
powervr_shader_pixel_places(
	const uint32_t formats[POWERVR_SHADER_OUTPUTS],
	struct powervr_shader_pixel_place places[POWERVR_SHADER_OUTPUTS],
	uint32_t *registers,
	uint32_t *tile_buffers)
{
	const struct pixel_format *layout;
	uint32_t used[1U + POWERVR_SHADER_TILE_BUFFERS];
	uint32_t location;
	uint32_t buffer;
	uint32_t buffers;
	uint32_t start;
	uint32_t step;
	uint32_t run;

	/* Refuses missing tables. */
	if (formats == NULL || places == NULL || registers == NULL || tile_buffers == NULL)
		return EINVAL;

	/* Nothing taken yet: the registers are buffer 0 of the table, the tile buffers 1 up. */
	for (buffer = 0; buffer < 1U + POWERVR_SHADER_TILE_BUFFERS; buffer++)
		used[buffer] = 0;
	buffers = 0;
	*registers = 0;

	/* Each attachment in order, in the first free run that takes it. */
	for (location = 0; location < POWERVR_SHADER_OUTPUTS; location++) {
		places[location].reg = POWERVR_SHADER_NO_REG;
		places[location].tile_buffer = POWERVR_SHADER_NO_REG;
		layout = pixel_format_find(formats[location]);
		if (formats[location] == 0 || layout == NULL)
			continue;

		/* The runs it may start at: every word, or every other for two or four. */
		run = (1U << layout->registers) - 1U;
		step = layout->registers == 1U ? 1U : 2U;

		/* The registers, then the tile buffers taken, then a new one: the first with such a run free. */
		start = PIXEL_OUTPUTS_MAX;
		for (buffer = 0; buffer <= buffers + 1U && buffer < 1U + POWERVR_SHADER_TILE_BUFFERS; buffer++) {
			for (start = 0; start + layout->registers <= PIXEL_OUTPUTS_MAX; start += step) {
				if ((used[buffer] & (run << start)) == 0)
					break;
			}
			if (start + layout->registers <= PIXEL_OUTPUTS_MAX)
				break;
		}

		/* Refuses an attachment past the last tile buffer. */
		if (buffer == 1U + POWERVR_SHADER_TILE_BUFFERS || start + layout->registers > PIXEL_OUTPUTS_MAX)
			return ENOTSUP;

		/* Takes the run, in a new tile buffer when it is the one after those taken. */
		used[buffer] |= run << start;
		if (buffer > buffers)
			buffers = buffer;
		places[location].reg = start;
		if (buffer != 0)
			places[location].tile_buffer = buffer - 1U;
		if (start + layout->registers > *registers)
			*registers = start + layout->registers;
	}

	/* Succeeded: the places and the tile buffers taken. */
	*tile_buffers = buffers;
	return 0;
}

/*
 * Writes one colour output into its pixel output register: clamped to
 * the attachment's range, blended with the colour already there, its
 * unwritten channels kept, packed (as 8-bit unsigned normalised in one
 * register, or as half floats in two for a 16-bit float attachment, not
 * clamped).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_nir_pvfio.c (lower_pfo_store,
 * to_pbe_format, pack_to_format: a B8G8R8A8 attachment is packed as
 * R8G8B8A8, the channels' float values packed to 8-bit unsigned
 * normalised; an sRGB one as 16-bit floats, saturated) and the Vulkan
 * 1.3 specification's blending equations
 * (the source and destination factors, the five operations, unsigned
 * normalised values clamped to [0, 1]).
 */
static int
reader_pixel_output(
	struct shader_spirv_reader *reader,
	const struct shader_spirv_id *variable,
	uint32_t words)
{
	const struct powervr_shader_blend *blend;
	const struct pixel_format *layout;
	struct powervr_ir_operand source[4];
	struct powervr_ir_operand destination[4];
	struct powervr_ir_operand result[4];
	struct powervr_ir_instruction *store;
	uint32_t channel;
	uint32_t loaded;
	uint32_t needs_destination;
	uint32_t sum_kind;
	uint32_t half;
	uint32_t srgb;
	uint32_t format;
	int held;
	int error;

	/*
	 * A format other than the 8-bit unorm, sRGB and 16-bit float ones,
	 * or any format in a tile buffer (whose words are only read and
	 * written whole): packed by its own channels.
	 */
	format = reader->key->output_formats[variable->location];
	layout = pixel_format_find(format);
	if (layout == NULL)
		return ENOTSUP;
	if (layout->classic == 0 || reader->pixel_tile_request[variable->location] != POWERVR_SHADER_NO_REG) {
		error = reader_pixel_output_packed(reader, variable, words, layout);
		if (error != 0)
			return error;

		/* Succeeded: the colour packed. */
		return 0;
	}

	/*
	 * Half floats (a 16-bit float attachment) are neither clamped nor
	 * normalised; an sRGB attachment takes them too, clamped to [0, 1]
	 * (the pixel back end converts them to sRGB).
	 */
	half = 0;
	srgb = 0;
	if (format == FORMAT_R16G16B16A16_SFLOAT || format == FORMAT_R8G8B8A8_SRGB || format == FORMAT_B8G8R8A8_SRGB)
		half = 1;
	if (format == FORMAT_R8G8B8A8_SRGB || format == FORMAT_B8G8R8A8_SRGB)
		srgb = 1;

	/*
	 * The shader's colour, clamped when the attachment is normalised and
	 * blended; a missing channel is 0, a missing alpha 1.  Without
	 * blending the pack clamps (its scaled 8-bit unsigned normalised
	 * form saturates, as Mesa's pco relies on: pck.scale.u8888 with no
	 * clamp before it), so the clamps are left out (they were eight of a
	 * flat quad's 24 operations a pixel).
	 */
	blend = &reader->key->blend[variable->location];
	for (channel = 0; channel < 4; channel++) {
		if (channel < words && !srgb && (half || blend->enable == 0)) {
			source[channel] = shader_spirv_value_operand(variable->first + channel, 0);
		} else if (channel < words) {
			source[channel] = saturate(reader, shader_spirv_value_operand(variable->first + channel, 0));
		} else {
			source[channel] = shader_spirv_constant_operand(channel == 3 ? FLOAT_ONE : 0);
		}
	}

	/*
	 * The sum-of-products form: a blend one operation does, with
	 * every channel a value or a constant the hardware holds (zero or
	 * one), is one instruction of the clamped colour; with the raw form,
	 * of the colour as the shader left it, for the operation to clamp
	 * (the clamps above then go as dead writes).
	 */
	if ((reader->key->forms & POWERVR_SHADER_FORM_SUM_OF_PRODUCTS) != 0 && half == 0 && blend->enable != 0) {
		error = blend_sum_kind(blend, &sum_kind);
		for (channel = 0; channel < 4 && error == 0; channel++) {
			/* A constant channel must be one the hardware holds. */
			if (source[channel].kind != POWERVR_IR_OPERAND_CONSTANT)
				continue;
			held = is_constant(&source[channel], 0);
			if (!held)
				held = is_constant(&source[channel], FLOAT_ONE);
			if (!held)
				error = ENOTSUP;
		}
		if (error == 0) {
			if ((reader->key->forms & POWERVR_SHADER_FORM_SUM_RAW) != 0) {
				/* The raw form: the colour as the shader left it. */
				for (channel = 0; channel < words && channel < 4; channel++)
					source[channel] = shader_spirv_value_operand(variable->first + channel, 0);
			}

			/* The blend, reading the fourth channel beside the three the emit takes. */
			shader_spirv_emit(reader, POWERVR_IR_PIXEL_BLEND, 0, &source[0], &source[1], &source[2], variable->interface, sum_kind);
			if (reader->error != 0)
				return reader->error;
			store = &reader->shader->instructions[reader->shader->instruction_count - 1U];
			store->source[3] = source[3];

			/* Reports what the colour came to. */
			return reader->error;
		}
	}

	/* The colour already there, when blending or a write mask needs it. */
	needs_destination = 0;
	if (blend->enable != 0 || (blend->write_mask & 0xfU) != 0xfU)
		needs_destination = 1;
	loaded = 0;
	if (needs_destination != 0) {
		loaded = shader_spirv_new_value(reader, 4);
		shader_spirv_emit(reader, POWERVR_IR_PIXEL_LOAD, loaded, NULL, NULL, NULL, variable->interface, half);
	}
	for (channel = 0; channel < 4; channel++)
		destination[channel] = shader_spirv_value_operand(loaded, channel);

	/* The blend of each channel. */
	error = blend_colour(reader, blend, source, destination, result);
	if (error != 0)
		return error;

	/* The write mask: an unwritten channel keeps the colour already there. */
	for (channel = 0; channel < 4; channel++) {
		if ((blend->write_mask & (1U << channel)) == 0)
			result[channel] = destination[channel];
	}

	/* The packed colour into its register. */
	shader_spirv_emit(reader, POWERVR_IR_PIXEL_STORE, 0, &result[0], &result[1], &result[2], variable->interface, half);
	if (reader->error != 0)
		return reader->error;
	store = &reader->shader->instructions[reader->shader->instruction_count - 1U];
	store->source[3] = result[3];

	/* Reports what the colour came to. */
	return reader->error;
}

/*
 * Writes one colour output into its attachment's pixel output
 * registers for a format the classic pack does not take: the shader's
 * channels (a missing one 0, a missing alpha 1), clamped to a normalised
 * format's range where the pack does not clamp or the blend needs it,
 * blended with the colour already there (float and normalised formats
 * only), packed into the format's words, the bits of the channels the
 * write mask leaves out kept from the words already there, and each word
 * written whole.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_nir_pvfio.c (lower_pfo_store,
 * to_pbe_format and pack_to_format: the format's missing channels 0 and
 * 1, a 32-bit channel a word, the integer channels inserted as bit
 * fields, the rest packed with the pack format of their width and type,
 * the normalised ones scaled, as many to a word as fit; lower_pfo_load
 * and unpack_from_format for the colour already there); the Khronos
 * Vulkan 1.3 specification (the blending equations: the source,
 * destination and result of a normalised attachment clamped to its
 * range; blending only for float and normalised formats; a format
 * without alpha blends with a destination alpha of 1; the write mask).
 * Measured: on the VF2 the 16-bit normalised packs of packUnorm2x16 and
 * packSnorm2x16 clamped values past the range as they scaled.
 */
static int
reader_pixel_output_packed(
	struct shader_spirv_reader *reader,
	const struct shader_spirv_id *variable,
	uint32_t words,
	const struct pixel_format *layout)
{
	const struct powervr_shader_blend *blend;
	struct powervr_ir_operand source[4];
	struct powervr_ir_operand destination[4];
	struct powervr_ir_operand result[4];
	struct powervr_ir_operand raw[4];
	struct powervr_ir_operand packed;
	struct powervr_ir_operand mask;
	struct powervr_ir_operand kept;
	uint32_t channel;
	uint32_t word;
	uint32_t one;
	uint32_t all;
	uint32_t written;
	uint32_t bits;
	uint32_t value;
	uint32_t blending;
	uint32_t normalised;
	uint32_t halves;
	uint32_t tile_request;
	uint32_t place;
	uint32_t form;
	int error;

	/* The shader's channels; a missing one 0, a missing alpha 1 (an integer 1 for an integer format). */
	one = FLOAT_ONE;
	if (layout->range == PIXEL_UINT || layout->range == PIXEL_SINT)
		one = 1;
	for (channel = 0; channel < 4; channel++) {
		if (channel < words) {
			source[channel] = shader_spirv_value_operand(variable->first + channel, 0);
		} else {
			source[channel] = shader_spirv_constant_operand(channel == 3 ? one : 0);
		}
	}

	/* The channels written: those of the format the write mask keeps; none, nothing to write. */
	blend = &reader->key->blend[variable->location];
	all = (1U << layout->channels) - 1U;
	written = blend->write_mask & all;
	if (written == 0)
		return 0;

	/*
	 * A float or normalised format blends; a normalised one goes through
	 * the clamp when it blends (the source first) or is carried as half
	 * floats (their pack does not clamp).
	 */
	blending = blend->enable != 0 && one == FLOAT_ONE;
	normalised = layout->range == PIXEL_UNORM || layout->range == PIXEL_SNORM;
	halves = layout->kind == PIXEL_PACKED && layout->code == POWERVR_IR_PACK_F16F16;
	if (normalised && (blending || halves)) {
		for (channel = 0; channel < 4; channel++)
			source[channel] = pixel_clamp(reader, source[channel], layout->range);
	}

	/*
	 * Where the words are: the attachment's pixel output registers, or
	 * its words of a tile buffer, named with the request of the buffer's
	 * address.
	 */
	form = POWERVR_IR_PIXEL_WORD;
	tile_request = reader->pixel_tile_request[variable->location];
	if (tile_request != POWERVR_SHADER_NO_REG)
		form = POWERVR_IR_PIXEL_TILE;

	/* The words already there, when the blend or the write mask needs them. */
	for (word = 0; word < layout->registers; word++) {
		raw[word] = shader_spirv_constant_operand(0);
		if (blending || written != all) {
			value = shader_spirv_new_value(reader, 1);
			place = pixel_word_place(variable->interface + word, tile_request);
			shader_spirv_emit(reader, POWERVR_IR_PIXEL_LOAD, value, NULL, NULL, NULL, place, form);
			raw[word] = shader_spirv_value_operand(value, 0);
		}
	}
	if (reader->error != 0)
		return reader->error;

	/* The blend with the colour already there, the sum clamped again where the pack does not. */
	for (channel = 0; channel < 4; channel++)
		result[channel] = source[channel];
	if (blending) {
		pixel_unpack(reader, layout, raw, destination);
		error = blend_colour(reader, blend, source, destination, result);
		if (error != 0)
			return error;
		if (normalised && halves) {
			for (channel = 0; channel < 4; channel++)
				result[channel] = pixel_clamp(reader, result[channel], layout->range);
		}
	}

	/* Each word: packed, the unwritten channels' bits kept, written. */
	for (word = 0; word < layout->registers; word++) {
		/* A word with no channel written stays as it is. */
		bits = pixel_word_bits(layout, word, written);
		if (bits == 0)
			continue;

		/* The packed word; with channels left out, its written bits over the word already there. */
		packed = pixel_pack_word(reader, layout, word, result);
		if (bits != pixel_word_bits(layout, word, all)) {
			mask = shader_spirv_constant_operand(bits);
			packed = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &packed, &mask, NULL);
			mask = shader_spirv_constant_operand(~bits);
			kept = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &raw[word], &mask, NULL);
			packed = shader_spirv_emit_value(reader, POWERVR_IR_BIT_OR, &packed, &kept, NULL);
		}
		place = pixel_word_place(variable->interface + word, tile_request);
		if (form == POWERVR_IR_PIXEL_TILE && reader->key->samples > 1U)
			place |= POWERVR_IR_PIXEL_TILE_SAMPLES;
		shader_spirv_emit(reader, POWERVR_IR_PIXEL_STORE, 0, &packed, NULL, NULL, place, form);
	}

	/* Reports what the colour came to. */
	return reader->error;
}

/*
 * The immediate of a pixel load or store of one word: the pixel output
 * register, or, with a tile buffer's request, the request and the word
 * among the pixel's words of the buffer (POWERVR_IR_PIXEL_TILE_PLACE).
 */
static uint32_t
pixel_word_place(
	uint32_t word,
	uint32_t tile_request)
{
	/* A register is its own number. */
	if (tile_request == POWERVR_SHADER_NO_REG)
		return word;

	/* Succeeded: the request and the word. */
	return POWERVR_IR_PIXEL_TILE_PLACE(tile_request, word);
}

/*
 * Clamps a float to a normalised format's range: [0, 1] by the
 * saturate, [-1, 1] by a maximum and a minimum.  A constant (a missing
 * channel's 0 or 1) is already in range.
 */
static struct powervr_ir_operand
pixel_clamp(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand x,
	uint32_t range)
{
	struct powervr_ir_operand bound;
	struct powervr_ir_operand result;

	/* A constant, or a range without bounds, stays. */
	if (x.kind == POWERVR_IR_OPERAND_CONSTANT && x.modifiers == 0)
		return x;
	if (range != PIXEL_UNORM && range != PIXEL_SNORM)
		return x;

	/* Unsigned: the saturate. */
	if (range == PIXEL_UNORM) {
		result = saturate(reader, x);
		return result;
	}

	/* Signed: no less than -1, no more than 1. */
	bound = shader_spirv_constant_operand(FLOAT_MINUS_ONE);
	result = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MAXIMUM, &x, &bound, NULL);
	bound = shader_spirv_constant_operand(FLOAT_ONE);
	result = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &result, &bound, NULL);
	return result;
}

/*
 * Unpacks the colour already in an attachment's words into four
 * floats: a packed format's channels by its pack format, a 32-bit one's
 * words as they are; a channel the format lacks 0, its alpha 1.
 */
static void
pixel_unpack(
	struct shader_spirv_reader *reader,
	const struct pixel_format *layout,
	const struct powervr_ir_operand *raw,
	struct powervr_ir_operand *destination)
{
	struct powervr_ir_operand word;
	uint32_t channel;
	uint32_t first;
	uint32_t count;
	uint32_t index;
	uint32_t value;
	uint32_t each;

	/* The channels the format lacks. */
	for (channel = 0; channel < 4; channel++)
		destination[channel] = shader_spirv_constant_operand(channel == 3 ? FLOAT_ONE : 0);

	/* A 32-bit format: each channel its word. */
	if (layout->kind == PIXEL_WORDS) {
		for (channel = 0; channel < layout->channels; channel++)
			destination[channel] = raw[channel];
		return;
	}

	/* A packed format: each word's channels by its pack format. */
	each = 32U / layout->bits;
	for (index = 0; index < layout->registers; index++) {
		first = index * each;
		count = layout->channels - first;
		if (count > each)
			count = each;
		word = shader_spirv_plain(reader, raw[index]);
		value = shader_spirv_new_value(reader, count);
		shader_spirv_emit(reader, POWERVR_IR_UNPACK_CHANNELS, value, &word, NULL, NULL, layout->code, count);
		for (channel = 0; channel < count; channel++)
			destination[first + channel] = shader_spirv_value_operand(value, channel);
	}
}

/*
 * Packs one of an attachment's words from the colour's four channels:
 * a 32-bit format's channel as it is, a packed format's channels of
 * the word by its pack format, an integer format's channels of the word
 * as bit fields (each cut to its width, at its offset).
 */
static struct powervr_ir_operand
pixel_pack_word(
	struct shader_spirv_reader *reader,
	const struct pixel_format *layout,
	uint32_t word,
	const struct powervr_ir_operand *colour)
{
	struct powervr_ir_instruction *pack;
	struct powervr_ir_operand result;
	struct powervr_ir_operand field;
	struct powervr_ir_operand mask;
	struct powervr_ir_operand amount;
	uint32_t channel;
	uint32_t first;
	uint32_t count;
	uint32_t value;
	uint32_t constant;
	uint32_t place;
	uint32_t shift;
	uint32_t width;
	uint32_t cut;
	int any;

	/* A 32-bit format: the channel itself, without float modifiers. */
	if (layout->kind == PIXEL_WORDS) {
		result = shader_spirv_plain(reader, colour[word]);
		return result;
	}

	/* A packed format: the word's channels by the pack format. */
	if (layout->kind == PIXEL_PACKED) {
		first = word * (32U / layout->bits);
		count = layout->channels - first;
		if (count > 32U / layout->bits)
			count = 32U / layout->bits;
		value = shader_spirv_new_value(reader, 1);
		shader_spirv_emit(reader, POWERVR_IR_PACK_CHANNELS, value, &colour[first],
				  count > 1U ? &colour[first + 1U] : NULL, count > 2U ? &colour[first + 2U] : NULL, layout->code, count);
		if (count > 3U && reader->error == 0) {
			pack = &reader->shader->instructions[reader->shader->instruction_count - 1U];
			pack->source[3] = colour[first + 3U];
		}
		result = shader_spirv_value_operand(value, 0);
		return result;
	}

	/* An integer format: the word's fields, the constant ones put together here. */
	constant = 0;
	any = 0;
	result = shader_spirv_constant_operand(0);
	for (channel = 0; channel < layout->channels; channel++) {
		pixel_channel_place(layout, channel, &place, &shift, &width);
		if (place != word)
			continue;

		/* The channel cut to its width at its offset. */
		cut = width >= 32U ? 0xffffffffU : (1U << width) - 1U;
		field = shader_spirv_plain(reader, colour[channel]);
		if (field.kind == POWERVR_IR_OPERAND_CONSTANT) {
			constant |= (field.constant & cut) << shift;
			continue;
		}
		if (cut != 0xffffffffU) {
			mask = shader_spirv_constant_operand(cut);
			field = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &field, &mask, NULL);
		}
		if (shift != 0) {
			amount = shader_spirv_constant_operand(shift);
			field = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_LEFT, &field, &amount, NULL);
		}

		/* Joined to the fields before it. */
		if (any) {
			result = shader_spirv_emit_value(reader, POWERVR_IR_BIT_OR, &result, &field, NULL);
		} else {
			result = field;
		}
		any = 1;
	}

	/* The constant fields: the word alone, or joined to the rest. */
	if (!any) {
		result = shader_spirv_constant_operand(constant);
		return result;
	}
	if (constant != 0) {
		mask = shader_spirv_constant_operand(constant);
		result = shader_spirv_emit_value(reader, POWERVR_IR_BIT_OR, &result, &mask, NULL);
	}
	return result;
}

/* Reports the bits of an attachment's word that the channels of a mask fill. */
static uint32_t
pixel_word_bits(
	const struct pixel_format *layout,
	uint32_t word,
	uint32_t channels)
{
	uint32_t channel;
	uint32_t bits;
	uint32_t place;
	uint32_t shift;
	uint32_t width;

	/* Each channel of the mask that lies in the word. */
	bits = 0;
	for (channel = 0; channel < layout->channels; channel++) {
		if ((channels & (1U << channel)) == 0)
			continue;
		pixel_channel_place(layout, channel, &place, &shift, &width);
		if (place != word)
			continue;
		if (width >= 32U) {
			bits = 0xffffffffU;
		} else {
			bits |= ((1U << width) - 1U) << shift;
		}
	}

	/* The bits. */
	return bits;
}

/*
 * Places a channel of an attachment's pixel: its word, the bit it
 * starts at and its width (channel c from bit c * bits on; the alpha of
 * a 10-bit format is 2 bits).
 */
static void
pixel_channel_place(
	const struct pixel_format *layout,
	uint32_t channel,
	uint32_t *word,
	uint32_t *shift,
	uint32_t *width)
{
	uint32_t offset;

	/* The offset across the words, then the word and the bit in it. */
	offset = channel * layout->bits;
	*word = offset / 32U;
	*shift = offset % 32U;

	/* The width: the format's, but 2 for a 10-bit format's alpha. */
	*width = layout->bits;
	if (layout->bits == 10U && channel == 3U)
		*width = 2;
}

/*
 * Blends each of the four channels of a colour with the destination's
 * by the attachment's blend (the source unchanged without one), asking
 * for the blend constants when a factor names them.
 *
 * Procedure referenced:
 * The Khronos Vulkan 1.3 specification's blending equations (the
 * source and destination factors, the five operations).
 */
static int
blend_colour(
	struct shader_spirv_reader *reader,
	const struct powervr_shader_blend *blend,
	const struct powervr_ir_operand *source,
	const struct powervr_ir_operand *destination,
	struct powervr_ir_operand *result)
{
	struct powervr_ir_operand a;
	struct powervr_ir_operand b;
	struct blend_factors kept;
	uint32_t channel;
	uint32_t factor;
	uint32_t operation;
	uint32_t constants;
	int error;

	/* The blend constants, asked for only when a factor names them. */
	constants = POWERVR_SHADER_REQUESTS;
	if (blend->enable != 0) {
		if ((blend->source_color >= FACTOR_CONSTANT_COLOR && blend->source_color <= FACTOR_ONE_MINUS_CONSTANT_ALPHA) ||
		    (blend->destination_color >= FACTOR_CONSTANT_COLOR && blend->destination_color <= FACTOR_ONE_MINUS_CONSTANT_ALPHA) ||
		    (blend->source_alpha >= FACTOR_CONSTANT_COLOR && blend->source_alpha <= FACTOR_ONE_MINUS_CONSTANT_ALPHA) ||
		    (blend->destination_alpha >= FACTOR_CONSTANT_COLOR && blend->destination_alpha <= FACTOR_ONE_MINUS_CONSTANT_ALPHA)) {
			error = shader_spirv_request_add(reader, POWERVR_SHADER_REQUEST_BLEND_CONSTANTS, 0, 0, 0, 4, &constants);
			if (error != 0)
				return error;
		}
	}

	/* Each channel's blend. */
	for (channel = 0; channel < BLEND_FACTORS; channel++)
		kept.known[channel] = 0;
	for (channel = 0; channel < 4; channel++) {
		result[channel] = source[channel];
		if (blend->enable != 0) {
			/* The operation of the colour or the alpha. */
			operation = channel < 3 ? blend->color_operation : blend->alpha_operation;
			if (operation == OPERATION_MINIMUM) {
				result[channel] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &source[channel], &destination[channel], NULL);
			} else if (operation == OPERATION_MAXIMUM) {
				result[channel] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MAXIMUM, &source[channel], &destination[channel], NULL);
			} else if (operation <= OPERATION_REVERSE_SUBTRACT) {
				/* The source and destination terms, each its value times its factor (the caller clamps the sum where the pack does not). */
				error = 0;
				factor = channel < 3 ? blend->source_color : blend->source_alpha;
				a = blend_factor_kept(reader, &kept, factor, channel, source, destination, constants, &error);
				factor = channel < 3 ? blend->destination_color : blend->destination_alpha;
				b = blend_factor_kept(reader, &kept, factor, channel, source, destination, constants, &error);
				if (error != 0)
					return error;
				result[channel] = blend_sum(reader, operation, &source[channel], &a, &destination[channel], &b);
			} else {
				return ENOTSUP;
			}
		}
	}

	/* Succeeded: the blended colour. */
	return 0;
}

/*
 * Works out one channel's blend factor, once a blend for the factors
 * that are the same for every colour channel (otherwise the one-minus
 * source alpha of premultiplied blending is worked out once a channel).
 */
static struct powervr_ir_operand
blend_factor_kept(
	struct shader_spirv_reader *reader,
	struct blend_factors *kept,
	uint32_t factor,
	uint32_t channel,
	const struct powervr_ir_operand *source,
	const struct powervr_ir_operand *destination,
	uint32_t constants,
	int *error)
{
	struct powervr_ir_operand value;
	int shared;

	/* The alpha and constant alpha factors, and the saturate one for a colour channel, are the same a channel. */
	shared = factor == FACTOR_SOURCE_ALPHA || factor == FACTOR_ONE_MINUS_SOURCE_ALPHA ||
		 factor == FACTOR_DESTINATION_ALPHA || factor == FACTOR_ONE_MINUS_DESTINATION_ALPHA ||
		 factor == FACTOR_CONSTANT_ALPHA || factor == FACTOR_ONE_MINUS_CONSTANT_ALPHA ||
		 (factor == FACTOR_SOURCE_ALPHA_SATURATE && channel < 3);
	if (shared && kept->known[factor] != 0)
		return kept->value[factor];

	/* Works it out, and keeps a shared one. */
	value = blend_factor(reader, factor, channel, source, destination, constants, error);
	if (shared && *error == 0) {
		kept->value[factor] = value;
		kept->known[factor] = 1;
	}
	return value;
}

/*
 * Works out source * source factor (+, - or reversed -) destination *
 * destination factor: a factor of one or zero takes no multiply, and a
 * destination term with a factor is one multiply-add with the source
 * term.
 */
static struct powervr_ir_operand
blend_sum(
	struct shader_spirv_reader *reader,
	uint32_t operation,
	const struct powervr_ir_operand *source,
	const struct powervr_ir_operand *source_factor,
	const struct powervr_ir_operand *destination,
	const struct powervr_ir_operand *destination_factor)
{
	struct powervr_ir_operand a;
	struct powervr_ir_operand b;
	struct powervr_ir_operand result;
	int has_a;
	int has_b;

	/* The source term: none for a zero factor, the source for one, else the product. */
	has_a = !is_constant(source_factor, 0);
	a = *source;
	if (has_a && !is_constant(source_factor, FLOAT_ONE))
		a = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, source, source_factor, NULL);
	if (operation == OPERATION_REVERSE_SUBTRACT)
		a = shader_spirv_float_negate(a);

	/* The destination term: none for a zero factor; negated when subtracted. */
	has_b = !is_constant(destination_factor, 0);
	b = *destination;
	if (operation == OPERATION_SUBTRACT)
		b = shader_spirv_float_negate(b);

	/* No destination term: the source term, or zero. */
	if (!has_b) {
		if (has_a)
			return a;
		result = shader_spirv_constant_operand(0);
		return result;
	}

	/* A destination term of factor one: added, or alone. */
	if (is_constant(destination_factor, FLOAT_ONE)) {
		if (!has_a)
			return b;
		result = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &a, &b, NULL);
		return result;
	}

	/* Alone: the product; with a source term: one multiply-add. */
	if (!has_a) {
		result = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &b, destination_factor, NULL);
		return result;
	}
	result = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &b, destination_factor, &a);
	return result;
}

/*
 * Names the blend of POWERVR_IR_PIXEL_BLEND a pipeline's blend is, when
 * it is one: every channel written, and the colour and the
 * alpha the same equation; ENOTSUP otherwise.
 */
static int
blend_sum_kind(
	const struct powervr_shader_blend *blend,
	uint32_t *kind)
{
	/* Every channel written. */
	if ((blend->write_mask & 0xfU) != 0xfU)
		return ENOTSUP;

	/* One and one minus the source alpha, added or subtracted: over. */
	if (blend->source_color == FACTOR_ONE &&
	    blend->destination_color == FACTOR_ONE_MINUS_SOURCE_ALPHA &&
	    blend->source_alpha == FACTOR_ONE &&
	    blend->destination_alpha == FACTOR_ONE_MINUS_SOURCE_ALPHA &&
	    blend->color_operation == blend->alpha_operation) {
		if (blend->color_operation == OPERATION_ADD) {
			*kind = POWERVR_IR_BLEND_OVER;
			return 0;
		}
		if (blend->color_operation == OPERATION_SUBTRACT) {
			*kind = POWERVR_IR_BLEND_SUBTRACT;
			return 0;
		}
		return ENOTSUP;
	}

	/* One and one, added. */
	if (blend->source_color == FACTOR_ONE &&
	    blend->destination_color == FACTOR_ONE &&
	    blend->source_alpha == FACTOR_ONE &&
	    blend->destination_alpha == FACTOR_ONE &&
	    blend->color_operation == OPERATION_ADD &&
	    blend->alpha_operation == OPERATION_ADD) {
		*kind = POWERVR_IR_BLEND_ADD;
		return 0;
	}

	/* The destination's colour (its alpha for the alpha) and zero, added: the product. */
	if (blend->source_color == FACTOR_DESTINATION_COLOR &&
	    blend->destination_color == FACTOR_ZERO &&
	    (blend->source_alpha == FACTOR_DESTINATION_ALPHA || blend->source_alpha == FACTOR_DESTINATION_COLOR) &&
	    blend->destination_alpha == FACTOR_ZERO &&
	    blend->color_operation == OPERATION_ADD &&
	    blend->alpha_operation == OPERATION_ADD) {
		*kind = POWERVR_IR_BLEND_MULTIPLY;
		return 0;
	}

	/* Any other blend takes the arithmetic. */
	return ENOTSUP;
}

/* Reports whether an operand is the constant of these bits. */
static int
is_constant(
	const struct powervr_ir_operand *operand,
	uint32_t bits)
{
	/* A constant operand without modifiers. */
	return operand->kind == POWERVR_IR_OPERAND_CONSTANT && operand->modifiers == 0 && operand->constant == bits;
}

/* Works out one channel's blend factor. */
static struct powervr_ir_operand
blend_factor(
	struct shader_spirv_reader *reader,
	uint32_t factor,
	uint32_t channel,
	const struct powervr_ir_operand *source,
	const struct powervr_ir_operand *destination,
	uint32_t constants,
	int *error)
{
	struct powervr_ir_operand one;
	struct powervr_ir_operand value;
	struct powervr_ir_operand other;

	/* The factor's value before any one-minus. */
	one = shader_spirv_constant_operand(FLOAT_ONE);
	switch (factor) {
	case FACTOR_ZERO:
		value = shader_spirv_constant_operand(0);

		/* Succeeded: the zero factor. */
		return value;
	case FACTOR_ONE:
		return one;
	case FACTOR_SOURCE_COLOR:
	case FACTOR_ONE_MINUS_SOURCE_COLOR:
		value = source[channel];
		break;
	case FACTOR_DESTINATION_COLOR:
	case FACTOR_ONE_MINUS_DESTINATION_COLOR:
		value = destination[channel];
		break;
	case FACTOR_SOURCE_ALPHA:
	case FACTOR_ONE_MINUS_SOURCE_ALPHA:
		value = source[3];
		break;
	case FACTOR_DESTINATION_ALPHA:
	case FACTOR_ONE_MINUS_DESTINATION_ALPHA:
		value = destination[3];
		break;
	case FACTOR_CONSTANT_COLOR:
	case FACTOR_ONE_MINUS_CONSTANT_COLOR:
		value = shader_spirv_shared_word(reader, constants, channel);
		break;
	case FACTOR_CONSTANT_ALPHA:
	case FACTOR_ONE_MINUS_CONSTANT_ALPHA:
		value = shader_spirv_shared_word(reader, constants, 3);
		break;
	case FACTOR_SOURCE_ALPHA_SATURATE:
		/* min(As, 1 - Ad) for the colour, 1 for the alpha. */
		if (channel == 3)
			return one;
		other = shader_spirv_float_negate(destination[3]);
		other = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &one, &other, NULL);
		value = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &source[3], &other, NULL);

		/* Succeeded: the saturated alpha factor. */
		return value;
	default:
		*error = ENOTSUP;
		return one;
	}

	/* The one-minus factors are the odd ones from 3 to 13. */
	if ((factor & 1U) != 0) {
		value = shader_spirv_float_negate(value);
		value = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &one, &value, NULL);
	}
	return value;
}

/*
 * Clamps a float to [0, 1]: one add of zero with the saturate (the
 * core's saturate in place of a maximum and a minimum, as Mesa's
 * pco lowers fsat).
 */
static struct powervr_ir_operand
saturate(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand x)
{
	struct powervr_ir_operand zero;
	struct powervr_ir_operand result;

	/* x + 0, saturated. */
	zero = shader_spirv_constant_operand(0);
	result = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &x, &zero, NULL);
	if (reader->error == 0 && reader->shader->instruction_count != 0)
		reader->shader->instructions[reader->shader->instruction_count - 1U].flags |= POWERVR_IR_SATURATE;
	return result;
}

/*
 * Works out how many words of a vertex input its attribute's format
 * gives: all the shader reads without a key or a format, else the
 * format's words, one a channel of the 32-bit float, unsigned and signed
 * integer formats (the rest are filled in by the shader).
 */
static int
attribute_words(
	struct shader_spirv_reader *reader,
	uint32_t location,
	uint32_t words,
	uint32_t *format_words)
{
	uint32_t format;

	/* No key, no format: the words as the shader reads them. */
	*format_words = words;
	if (reader->key == NULL || location >= POWERVR_SHADER_INPUTS)
		return 0;
	format = reader->key->attribute_formats[location];
	if (format == 0)
		return 0;

	/*
	 * The 32-bit formats, R32 to R32G32B32A32 each as UINT, SINT and
	 * SFLOAT in turn: one word a channel, copied as it is.  Other formats
	 * need the shader to convert them; outside the subset for now.
	 */
	if (format < FORMAT_R32_UINT || format > FORMAT_R32G32B32A32_SFLOAT)
		return ENOTSUP;
	*format_words = (format - FORMAT_R32_UINT) / 3U + 1U;

	/* The shader never reads past its own words. */
	if (*format_words > words)
		*format_words = words;
	return 0;
}

/*
 * Returns the bits of 1 in an attribute's format: an integer 1 for the
 * 32-bit unsigned and signed integer formats, else a float 1.
 *
 * Procedure referenced: Khronos Vulkan 1.3 specification, "Vertex Input
 * Extraction" (missing G, B and A components are filled with 0, 0 and
 * 1, the 1 of the format's type: an integer for an integer format).
 */
static uint32_t
attribute_one(
	const struct shader_spirv_reader *reader,
	uint32_t location)
{
	uint32_t format;

	/* No key or no format: the float 1. */
	if (reader->key == NULL || location >= POWERVR_SHADER_INPUTS)
		return FLOAT_ONE;
	format = reader->key->attribute_formats[location];

	/* A 32-bit UINT or SINT format (two of each three from R32_UINT on). */
	if (format >= FORMAT_R32_UINT && format <= FORMAT_R32G32B32A32_SFLOAT && (format - FORMAT_R32_UINT) % 3U != 2U)
		return 1;
	return FLOAT_ONE;
}

/* Finds a colour attachment format in the pixel output's table (NULL when the compiler does not pack it). */
static const struct pixel_format *
pixel_format_find(
	uint32_t format)
{
	uint32_t index;

	/* Each entry in turn. */
	for (index = 0; index < sizeof(pixel_formats) / sizeof(pixel_formats[0]); index++) {
		if (pixel_formats[index].format == format)
			return &pixel_formats[index];
	}

	/* Not a format the pixel output packs. */
	return NULL;
}

/* Reports whether the module has a kill (OpKill or OpTerminateInvocation) anywhere past its header. */
static int
module_kills(
	const struct shader_spirv_reader *reader)
{
	size_t at;
	uint32_t length;
	uint32_t opcode;

	/* Each instruction in turn. */
	at = SPIRV_HEADER_WORDS;
	while (at < reader->count) {
		length = reader->words[at] >> 16;
		opcode = reader->words[at] & 0xffffU;
		if (opcode == OP_KILL || opcode == OP_TERMINATE_INVOCATION)
			return 1;
		if (length == 0)
			return 0;
		at += length;
	}

	/* No kill. */
	return 0;
}

/*
 * Works out a compute shader's local index and local id once: the index
 * from vertex input register 0, the id from the index and the
 * workgroup's size (x fastest, then y, then z).
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_pipeline.c
 * (pvr_alloc_cs_sysvals: the local invocation index in a vertex input
 * register; pvr_pds_compute_program_setup: the data sequencer writes it
 * there, the workgroups flattened) and src/compiler/nir/
 * nir_lower_system_values.c (the local id from the index).
 */
static int
compute_local(
	struct shader_spirv_reader *reader)
{
	struct powervr_ir_operand width;
	struct powervr_ir_operand height;
	struct powervr_ir_operand plane;
	struct powervr_ir_operand row;
	uint32_t *size;

	/* Worked out already. */
	if (reader->local_ready != 0)
		return 0;
	size = reader->program->workgroup;
	if (size[0] == 0 || size[1] == 0 || size[2] == 0)
		return ENOTSUP;

	/* The index the data sequencer writes, in the one vertex input register the program declares. */
	reader->local[3] = shader_spirv_emit_value(reader, POWERVR_IR_INPUT, NULL, NULL, NULL);
	if (reader->program->vertex_inputs < 1U)
		reader->program->vertex_inputs = 1U;

	/* The id along each axis. */
	width = shader_spirv_constant_operand(size[0]);
	height = shader_spirv_constant_operand(size[1]);
	plane = shader_spirv_constant_operand(size[0] * size[1]);
	reader->local[0] = shader_spirv_divide(reader, reader->local[3], width, DIVIDE_UNSIGNED_REMAINDER);
	row = shader_spirv_divide(reader, reader->local[3], width, DIVIDE_UNSIGNED);
	reader->local[1] = shader_spirv_divide(reader, row, height, DIVIDE_UNSIGNED_REMAINDER);
	reader->local[2] = shader_spirv_divide(reader, reader->local[3], plane, DIVIDE_UNSIGNED);
	reader->local_ready = 1;
	return reader->error;
}

/*
 * Places a compute shader's workgroup memory: the words a barrier
 * between tasks keeps after the variables, and the workgroup's slice of
 * the dispatch's shared memory at its flat id (x fastest) times the
 * slice's size.
 */
static int
compute_shared(
	struct shader_spirv_reader *reader,
	uint32_t invocations)
{
	struct powervr_ir_operand group[3];
	struct powervr_ir_operand count[2];
	struct powervr_ir_operand flat;
	struct powervr_ir_operand size;
	uint32_t identity;
	uint32_t counts;
	uint32_t component;
	int error;

	/* The barrier's counter and sequence, when the workgroup is more than one task and meets at a barrier. */
	if (invocations > POWERVR_SHADER_TASK_INSTANCES && module_has(reader, OP_CONTROL_BARRIER)) {
		if (reader->program->shared_bytes == 0)
			reader->program->shared_bytes = 4U;
		if (reader->program->shared_bytes > SHADER_SHARED_BYTES_MAX - 8U)
			return ENOTSUP;
		reader->barrier_offset = reader->program->shared_bytes;
		reader->program->shared_bytes += 8U;
	}

	/* No workgroup memory, no slice. */
	if (reader->program->shared_bytes == 0)
		return 0;

	/* The workgroup's flat id from its id and the dispatch's counts. */
	error = shader_spirv_request_add(reader, POWERVR_SHADER_REQUEST_WORKGROUP_ID, 0, 0, 0, 3, &identity);
	if (error != 0)
		return error;
	error = shader_spirv_request_add(reader, POWERVR_SHADER_REQUEST_NUM_WORKGROUPS, 0, 0, 0, 3, &counts);
	if (error != 0)
		return error;
	for (component = 0; component < 3U; component++)
		group[component] = shader_spirv_shared_word(reader, identity, component);
	count[0] = shader_spirv_shared_word(reader, counts, 0);
	count[1] = shader_spirv_shared_word(reader, counts, 1);
	flat = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &group[2], &count[1], NULL);
	flat = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &flat, &group[1], NULL);
	flat = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &flat, &count[0], NULL);
	flat = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &flat, &group[0], NULL);

	/* Its slice. */
	size = shader_spirv_constant_operand(reader->program->shared_bytes);
	reader->shared_slice = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &flat, &size, NULL);
	return reader->error;
}

/* Reports whether the module has an instruction of an opcode anywhere. */
static int
module_has(
	const struct shader_spirv_reader *reader,
	uint32_t wanted)
{
	size_t at;
	uint32_t length;
	uint32_t opcode;

	/* Each instruction in turn. */
	at = SPIRV_HEADER_WORDS;
	while (at < reader->count) {
		length = reader->words[at] >> 16;
		opcode = reader->words[at] & 0xffffU;
		if (opcode == wanted)
			return 1;
		if (length == 0)
			return 0;
		at += length;
	}

	/* None. */
	return 0;
}

/*
 * Finds a vertex output variable's point size word: the variable's own
 * when it is the built-in, the member's in the per-vertex block, or
 * UINT32_MAX for none.
 */
static uint32_t
point_size_word(
	struct shader_spirv_reader *reader,
	const struct shader_spirv_id *variable)
{
	const struct shader_spirv_member *member;
	const struct shader_spirv_id *type;
	uint32_t member_index;

	/* Only a vertex shader's built-in outputs. */
	if (reader->stage != POWERVR_SHADER_VERTEX || variable->storage != STORAGE_OUTPUT || (variable->flags & FLAG_LOCATION) != 0)
		return UINT32_MAX;

	/* The built-in alone. */
	if ((variable->flags & FLAG_BUILT_IN) != 0)
		return variable->builtin == BUILT_IN_POINT_SIZE ? 0U : UINT32_MAX;

	/* A member of the per-vertex block. */
	type = &reader->ids[variable->type];
	if (type->type_kind != TYPE_STRUCT)
		return UINT32_MAX;
	for (member_index = 0; member_index < type->count; member_index++) {
		member = &reader->members[type->first + member_index];
		if (member->has_builtin != 0 && member->builtin == BUILT_IN_POINT_SIZE)
			return member->scalar_offset;
	}

	/* None. */
	return UINT32_MAX;
}

/*
 * Reads a fragment shader's built-in input into its values: the
 * position's x and y from the core's special registers (z and w are not
 * given yet: a shader that reads them is refused), the facing from the
 * primitive's orientation and the key's front face, the point's
 * coordinates iterated linearly.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_trans_nir.c (trans_load_input_fs:
 * the position's x and y from the pixel's special registers) and
 * src/imagination/pco/pco_nir_pvfio.c (lower_front_face: front where
 * the orientation is zero, swapped or always true by the front face).
 */
static int
fragment_built_in(
	struct shader_spirv_reader *reader,
	const struct shader_spirv_id *id,
	uint32_t words)
{
	struct powervr_ir_operand orientation;
	struct powervr_ir_operand zero;
	struct powervr_ir_operand word;
	uint32_t front;
	uint32_t component;
	uint32_t special;
	uint32_t multisampled;

	zero = shader_spirv_constant_operand(0);
	switch (id->builtin) {
	case BUILT_IN_FRAG_COORD:
		/* x and y (the sample's when four samples run at the sample rate); z and w a word nothing may read. */
		multisampled = reader->key != NULL && reader->key->samples == 4U;
		for (component = 0; component < words && component < 4U; component++) {
			special = POWERVR_IR_SPECIAL_NONE;
			if (component == 0)
				special = multisampled ? POWERVR_IR_SPECIAL_SAMPLE_X : POWERVR_IR_SPECIAL_X;
			else if (component == 1)
				special = multisampled ? POWERVR_IR_SPECIAL_SAMPLE_Y : POWERVR_IR_SPECIAL_Y;
			shader_spirv_emit(reader, POWERVR_IR_SPECIAL, id->first + component, NULL, NULL, NULL, special, 0);
		}
		break;
	case BUILT_IN_FRONT_FACING:
		/* Front where the orientation is zero, the other way, or always. */
		front = POWERVR_SHADER_FRONT_CLOCKWISE;
		if (reader->key != NULL)
			front = reader->key->front_face;
		if (front == POWERVR_SHADER_FRONT_ALWAYS) {
			word = shader_spirv_constant_operand(TRUE_WORD);
			shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first, &word, NULL, NULL, 0, 0);
			break;
		}
		orientation = shader_spirv_value_operand(shader_spirv_new_value(reader, 1), 0);
		shader_spirv_emit(reader, POWERVR_IR_SPECIAL, orientation.value, NULL, NULL, NULL, POWERVR_IR_SPECIAL_FACING, 0);
		shader_spirv_emit(reader, front == POWERVR_SHADER_FRONT_CLOCKWISE ? POWERVR_IR_INTEGER_EQUAL : POWERVR_IR_INTEGER_NOT_EQUAL,
				  id->first, &orientation, &zero, NULL, 0, 0);
		break;
	case BUILT_IN_SAMPLE_ID:
		/* The sample's number with four samples (each runs on its own), else sample 0. */
		if (reader->key != NULL && reader->key->samples == 4U) {
			shader_spirv_emit(reader, POWERVR_IR_SPECIAL, id->first, NULL, NULL, NULL, POWERVR_IR_SPECIAL_SAMPLE, 0);
		} else {
			shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first, &zero, NULL, NULL, 0, 0);
		}
		break;
	case BUILT_IN_POINT_COORD:
		/* The two coordinates from their sets. */
		if (reader->program->point_coord_coefficient == POWERVR_SHADER_NO_REG || words > 2U)
			return ENOTSUP;
		for (component = 0; component < words; component++) {
			shader_spirv_emit(reader, POWERVR_IR_ITERATE, id->first + component, NULL, NULL, NULL, reader->program->point_coord_coefficient + component * 4U, 0);
			reader->shader->instructions[reader->shader->instruction_count - 1U].flags = POWERVR_IR_LINEAR;
		}
		break;
	default:
		return ENOTSUP;
	}
	return reader->error;
}

/* Reports whether the shader declares an input variable of a built-in. */
static int
reads_built_in(
	const struct shader_spirv_reader *reader,
	uint32_t builtin)
{
	const struct shader_spirv_id *id;
	uint32_t index;

	/* Each input variable. */
	for (index = 1; index < reader->bound; index++) {
		id = &reader->ids[index];
		if (id->kind != ID_POINTER || id->variable != index || id->storage != STORAGE_INPUT)
			continue;
		if ((id->flags & FLAG_BUILT_IN) != 0 && id->builtin == builtin)
			return 1;
	}

	/* None. */
	return 0;
}
