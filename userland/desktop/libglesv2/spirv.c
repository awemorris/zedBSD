/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPIR-V side of zedBSD's OpenGL ES (WS068 p008): what a shader's
 * interface is (its inputs, outputs, uniform block and samplers, by name
 * and location), and the rewrite that turns a vertex shader's GL clip
 * coordinates into Vulkan's.
 *
 * Only what the module declares is read; nothing checks that the module
 * is valid SPIR-V beyond the instruction lengths (the Vulkan driver does,
 * when the shader module is made).
 */

#include "gles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SPIR-V's magic number and the length of its header in words. */
#define SPIRV_MAGIC		0x07230203U
#define SPIRV_HEADER		5U

/* The largest id bound a module may have. */
#define SPIRV_BOUND_MAX		(1U << 22)

/* The opcodes read or written. */
#define OP_NAME			5U
#define OP_MEMBER_NAME		6U
#define OP_ENTRY_POINT		15U
#define OP_TYPE_BOOL		20U
#define OP_TYPE_INT		21U
#define OP_TYPE_FLOAT		22U
#define OP_TYPE_VECTOR		23U
#define OP_TYPE_MATRIX		24U
#define OP_TYPE_IMAGE		25U

/* OpTypeImage's Dim of a cube map. */
#define SPIRV_DIM_3D		2U
#define SPIRV_DIM_CUBE		3U
#define SPIRV_DIM_BUFFER	5U
#define OP_TYPE_SAMPLED_IMAGE	27U
#define OP_TYPE_ARRAY		28U
#define OP_TYPE_STRUCT		30U
#define OP_TYPE_POINTER		32U
#define OP_CONSTANT		43U
#define OP_FUNCTION		54U
#define OP_FUNCTION_END		56U
#define OP_VARIABLE		59U
#define OP_LOAD			61U
#define OP_STORE		62U
#define OP_ACCESS_CHAIN		65U
#define OP_DECORATE		71U
#define OP_MEMBER_DECORATE	72U
#define OP_COMPOSITE_CONSTRUCT	80U
#define OP_COMPOSITE_EXTRACT	81U
#define OP_FNEGATE		127U
#define OP_FADD			129U
#define OP_FMUL			133U
#define OP_RETURN		253U
#define OP_EMIT_VERTEX		218U

/* The decorations read. */
#define DECORATION_BLOCK	2U
#define DECORATION_ARRAY_STRIDE	6U
#define DECORATION_MATRIX_STRIDE 7U
#define DECORATION_BUILT_IN	11U
#define DECORATION_LOCATION	30U
#define DECORATION_INDEX	32U
#define DECORATION_BINDING	33U
#define DECORATION_SET		34U
#define DECORATION_OFFSET	35U

/* The storage classes read, and the built-in rewritten. */
#define STORAGE_UNIFORM_CONSTANT 0U
#define STORAGE_INPUT		1U
#define STORAGE_UNIFORM		2U
#define STORAGE_OUTPUT		3U
#define STORAGE_WORKGROUP	4U
#define BUILT_IN_POSITION	0U

/* The execution model of a compute shader (GLCompute). */
#define SPIRV_MODEL_COMPUTE	5U

/* The value of a decoration an id does not have. */
#define SPIRV_NONE		0xffffffffU

/*
 * A module being read: where each id is defined, and the decorations and
 * names of each id.
 */
struct spirv_module {
	/* The words and the id bound. */
	const uint32_t *code;
	size_t words;
	uint32_t bound;

	/* The word each id's defining instruction starts at (0: none). */
	size_t *defs;

	/* Each id's OpName (NULL: none). */
	const char **names;

	/* Each id's Location (and the word it is in), Binding, DescriptorSet, BuiltIn and ArrayStride; SPIRV_NONE when absent. */
	uint32_t *locations;
	size_t *location_words;

	/* The word of each id's Index (a fragment output's dual-source blend input; 0 when absent). */
	size_t *index_words;
	uint32_t *bindings;
	uint32_t *sets;
	uint32_t *builtins;
	uint32_t *array_strides;

	/* Nonzero for ids decorated Block. */
	unsigned char *blocks;

	/* The entry point's function and execution model. */
	uint32_t entry;
	uint32_t model;
};

static int spirv_open(struct spirv_module *module, const uint32_t *code, size_t words);
static void spirv_close(struct spirv_module *module);
static void spirv_decorate(struct spirv_module *module, size_t at, uint32_t length);
static const char *spirv_string(const uint32_t *code, size_t at, uint32_t length, uint32_t first);
static uint32_t spirv_member_decoration(struct spirv_module *module, uint32_t structure, uint32_t member, uint32_t decoration);
static const char *spirv_member_name(struct spirv_module *module, uint32_t structure, uint32_t member);
static uint32_t spirv_pointee(struct spirv_module *module, uint32_t pointer, uint32_t *storage);
static uint32_t spirv_constant(struct spirv_module *module, uint32_t id);
static int spirv_leaf(struct spirv_module *module, uint32_t type, struct gles_uniform *uniform);
static int spirv_flatten(struct spirv_module *module, struct gles_spirv *out, uint32_t type, const char *prefix, uint32_t offset, uint32_t matrix_stride);
static struct gles_uniform *spirv_add_uniform(struct gles_spirv *out);
static int spirv_variable(struct spirv_module *module, struct gles_spirv *out, size_t at);
static uint32_t spirv_find_type(struct spirv_module *module, uint32_t opcode, uint32_t first, uint32_t second, uint32_t operands);
static GLenum spirv_sampler_type(struct spirv_module *module, size_t image);
static void spirv_emit(uint32_t *out, size_t *count, uint32_t opcode, uint32_t words, const uint32_t *operands);

/*
 * Returns GL's name of the type of a scalar, a vector or a matrix of a
 * scalar kind (0 float, 1 int, 2 unsigned, 3 bool, 4 double), components
 * per column and columns.
 */
GLenum
gles_gl_type(
	unsigned base,
	unsigned components,
	unsigned columns)
{
	static const GLenum floats[4] = { GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4 };
	static const GLenum ints[4] = { GL_INT, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4 };
	static const GLenum unsigneds[4] = { GL_UNSIGNED_INT, GL_UNSIGNED_INT_VEC2, GL_UNSIGNED_INT_VEC3, GL_UNSIGNED_INT_VEC4 };
	static const GLenum bools[4] = { GL_BOOL, GL_BOOL_VEC2, GL_BOOL_VEC3, GL_BOOL_VEC4 };
	static const GLenum doubles[4] = { GL_DOUBLE, GL_DOUBLE_VEC2, GL_DOUBLE_VEC3, GL_DOUBLE_VEC4 };
	static const GLenum matrices[3][3] = {
		{ GL_FLOAT_MAT2, GL_FLOAT_MAT2x3, GL_FLOAT_MAT2x4 },
		{ GL_FLOAT_MAT3x2, GL_FLOAT_MAT3, GL_FLOAT_MAT3x4 },
		{ GL_FLOAT_MAT4x2, GL_FLOAT_MAT4x3, GL_FLOAT_MAT4 }
	};
	static const GLenum double_matrices[3][3] = {
		{ GL_DOUBLE_MAT2, GL_DOUBLE_MAT2x3, GL_DOUBLE_MAT2x4 },
		{ GL_DOUBLE_MAT3x2, GL_DOUBLE_MAT3, GL_DOUBLE_MAT3x4 },
		{ GL_DOUBLE_MAT4x2, GL_DOUBLE_MAT4x3, GL_DOUBLE_MAT4 }
	};

	/* Outside the shapes GL names: a float. */
	if (components < 1U ||
	    components > 4U ||
	    columns < 1U ||
	    columns > 4U)
		return GL_FLOAT;

	/* A matrix of floats or of doubles: by its columns and rows. */
	if (columns > 1U) {
		if (components < 2U)
			return GL_FLOAT;
		if (base == 4U)
			return double_matrices[columns - 2U][components - 2U];
		return matrices[columns - 2U][components - 2U];
	}

	/* A scalar or a vector of its kind. */
	switch (base) {
	case 1U:
		return ints[components - 1U];
	case 2U:
		return unsigneds[components - 1U];
	case 3U:
		return bools[components - 1U];
	case 4U:
		return doubles[components - 1U];
	default:
		break;
	}

	/* Floats. */
	return floats[components - 1U];
}

/*
 * Reads a shader's interface out of its SPIR-V.  Returns 0, or -1 with a
 * line in the log when the module is not one this library can use.
 */
int
gles_spirv_reflect(
	const uint32_t *code,
	size_t words,
	struct gles_spirv *out,
	char *log,
	size_t log_size)
{
	struct spirv_module module;
	size_t at;
	uint32_t opcode;
	uint32_t length;
	int status;

	/* The module's ids, names and decorations. */
	memset(out, 0, sizeof(*out));
	status = spirv_open(&module, code, words);
	if (status != 0) {
		(void)snprintf(log, log_size, "not a SPIR-V module this library reads\n");
		return -1;
	}

	/* The stage. */
	out->model = module.model;

	/* Each global variable: an input, an output, the uniform block or a sampler. */
	for (at = SPIRV_HEADER; at < words; at += length) {
		opcode = code[at] & 0xffffU;
		length = code[at] >> 16;
		if (opcode == OP_FUNCTION)
			break;
		if (opcode != OP_VARIABLE)
			continue;

		/* A variable this library does not understand refuses the module. */
		status = spirv_variable(&module, out, at);
		if (status != 0) {
			(void)snprintf(log, log_size, "the shader's interface has a variable of a kind not supported (id %u)\n", code[at + 2U]);
			spirv_close(&module);
			gles_spirv_free(out);
			return -1;
		}
	}

	/* Succeeded: the interface. */
	spirv_close(&module);
	return 0;
}

/*
 * Releases what reflecting a shader allocated.
 */
void
gles_spirv_free(
	struct gles_spirv *spirv)
{
	/* The uniforms. */
	free(spirv->uniforms);
	spirv->uniforms = NULL;
	spirv->uniform_count = 0U;
	spirv->uniform_capacity = 0U;
}

/*
 * Returns a copy of a vertex shader that, before each return of its entry
 * point, turns gl_Position from GL's clip coordinates into Vulkan's: z
 * moved from [-w, w] to [0, w] when half_depth is nonzero (desktop GL's
 * glClipControl(GL_ZERO_TO_ONE) keeps it), and y turned over when flip is nonzero
 * (a window's rows go down from its top; a framebuffer object's image
 * keeps GL's rows from the bottom up, as textures do).  A geometry shader
 * (a module that emits vertices) is rewritten before each OpEmitVertex
 * instead.  Returns NULL when the module cannot be rewritten; a module
 * that never names gl_Position comes back unchanged.
 */
uint32_t *
gles_spirv_position(
	const uint32_t *code,
	size_t words,
	int flip,
	int half_depth,
	size_t *out_words)
{
	struct spirv_module module;
	uint32_t operands[8];
	uint32_t *out;
	size_t count;
	size_t at;
	size_t returns;
	size_t emits;
	size_t types_end;
	uint32_t trigger;
	uint32_t opcode;
	uint32_t length;
	uint32_t position;
	uint32_t member;
	uint32_t storage;
	uint32_t pointee;
	uint32_t float_type;
	uint32_t vector_type;
	uint32_t pointer_type;
	uint32_t int_type;
	uint32_t index_constant;
	uint32_t half;
	uint32_t next;
	uint32_t ids[10];
	uint32_t in_entry;
	uint32_t builtin;
	int status;

	/* The module's ids and decorations. */
	status = spirv_open(&module, code, words);
	if (status != 0)
		return NULL;

	/* The variable holding gl_Position: itself decorated, or a block with a member decorated. */
	position = 0U;
	member = SPIRV_NONE;
	for (at = SPIRV_HEADER; at < words && position == 0U; at += length) {
		opcode = code[at] & 0xffffU;
		length = code[at] >> 16;
		if (opcode == OP_FUNCTION)
			break;
		if (opcode != OP_VARIABLE || code[at + 3U] != STORAGE_OUTPUT)
			continue;

		/* A vec4 variable decorated Position. */
		if (module.builtins[code[at + 2U]] == BUILT_IN_POSITION) {
			position = code[at + 2U];
			continue;
		}

		/* A block whose member is Position (gl_PerVertex). */
		pointee = spirv_pointee(&module, code[at + 1U], &storage);
		if (pointee == 0U || module.defs[pointee] == 0U || (code[module.defs[pointee]] & 0xffffU) != OP_TYPE_STRUCT)
			continue;
		for (member = 0U; member + 2U < (code[module.defs[pointee]] >> 16); member++) {
			builtin = spirv_member_decoration(&module, pointee, member, DECORATION_BUILT_IN);
			if (builtin == BUILT_IN_POSITION)
				break;
		}

		/* The block has it. */
		if (member + 2U < (code[module.defs[pointee]] >> 16)) {
			position = code[at + 2U];
		} else {
			member = SPIRV_NONE;
		}
	}

	/* The types the rewrite needs: float, vec4, a pointer to an output vec4. */
	float_type = spirv_find_type(&module, OP_TYPE_FLOAT, 32U, SPIRV_NONE, 1U);
	vector_type = 0U;
	if (float_type != 0U)
		vector_type = spirv_find_type(&module, OP_TYPE_VECTOR, float_type, 4U, 2U);
	pointer_type = 0U;
	if (vector_type != 0U)
		pointer_type = spirv_find_type(&module, OP_TYPE_POINTER, STORAGE_OUTPUT, vector_type, 2U);
	int_type = spirv_find_type(&module, OP_TYPE_INT, 32U, 1U, 2U);

	/* The copy: as long as the module, a few declarations, and eleven instructions at each return or emitted vertex. */
	returns = 0U;
	emits = 0U;
	for (at = SPIRV_HEADER; at < words; at += length) {
		length = code[at] >> 16;
		if ((code[at] & 0xffffU) == OP_RETURN)
			returns++;
		if ((code[at] & 0xffffU) == OP_EMIT_VERTEX)
			emits++;
	}

	/* A module that emits vertices is rewritten at each vertex emitted, any other at each return. */
	trigger = OP_RETURN;
	if (emits != 0U)
		trigger = OP_EMIT_VERTEX;

	/* Without memory there is no copy. */
	out = malloc((words + 32U + (returns + emits) * 64U) * sizeof(uint32_t));
	if (out == NULL) {
		spirv_close(&module);
		return NULL;
	}

	/* Without gl_Position, or without its types, the module stays as it is. */
	if (position == 0U || vector_type == 0U) {
		memcpy(out, code, words * sizeof(uint32_t));
		*out_words = words;
		spirv_close(&module);
		return out;
	}

	/* The header, then everything before the first function. */
	next = code[3];
	memcpy(out, code, SPIRV_HEADER * sizeof(uint32_t));
	count = SPIRV_HEADER;
	types_end = words;
	for (at = SPIRV_HEADER; at < words; at += length) {
		length = code[at] >> 16;
		if ((code[at] & 0xffffU) == OP_FUNCTION) {
			types_end = at;
			break;
		}

		/* Copied as it is. */
		memcpy(out + count, code + at, length * sizeof(uint32_t));
		count += length;
	}

	/* The declarations the rewrite adds: the pointer type, the member index, 0.5. */
	if (pointer_type == 0U) {
		pointer_type = next++;
		operands[0] = pointer_type;
		operands[1] = STORAGE_OUTPUT;
		operands[2] = vector_type;
		spirv_emit(out, &count, OP_TYPE_POINTER, 3U, operands);
	}

	/* The member index, when gl_Position is in a block. */
	index_constant = 0U;
	if (member != SPIRV_NONE) {
		if (int_type == 0U) {
			int_type = next++;
			operands[0] = int_type;
			operands[1] = 32U;
			operands[2] = 1U;
			spirv_emit(out, &count, OP_TYPE_INT, 3U, operands);
		}

		/* The constant. */
		index_constant = next++;
		operands[0] = int_type;
		operands[1] = index_constant;
		operands[2] = member;
		spirv_emit(out, &count, OP_CONSTANT, 3U, operands);
	}

	/* The constant 0.5. */
	half = next++;
	operands[0] = float_type;
	operands[1] = half;
	operands[2] = 0x3f000000U;
	spirv_emit(out, &count, OP_CONSTANT, 3U, operands);

	/* The functions, with the rewrite before each return of the entry point. */
	in_entry = 0U;
	for (at = types_end; at < words; at += length) {
		opcode = code[at] & 0xffffU;
		length = code[at] >> 16;

		/* Which function the instruction is in. */
		if (opcode == OP_FUNCTION && code[at + 2U] == module.entry)
			in_entry = 1U;
		else if (opcode == OP_FUNCTION)
			in_entry = 0U;

		/* A return (a vertex emitted) of the entry point: gl_Position is read, changed and written back first. */
		if (opcode == trigger && in_entry) {
			/* Fresh ids: pointer, value, x, y, z, w, -y, z+w, (z+w)/2, the new vector. */
			for (member = 0U; member < 10U; member++)
				ids[member] = next++;

			/* The pointer to gl_Position. */
			if (index_constant != 0U) {
				operands[0] = pointer_type;
				operands[1] = ids[0];
				operands[2] = position;
				operands[3] = index_constant;
				spirv_emit(out, &count, OP_ACCESS_CHAIN, 4U, operands);
			} else {
				ids[0] = position;
			}

			/* Its value and its four components. */
			operands[0] = vector_type;
			operands[1] = ids[1];
			operands[2] = ids[0];
			spirv_emit(out, &count, OP_LOAD, 3U, operands);
			for (member = 0U; member < 4U; member++) {
				operands[0] = float_type;
				operands[1] = ids[2U + member];
				operands[2] = ids[1];
				operands[3] = member;
				spirv_emit(out, &count, OP_COMPOSITE_EXTRACT, 4U, operands);
			}

			/* -y. */
			operands[0] = float_type;
			operands[1] = ids[6];
			operands[2] = ids[3];
			spirv_emit(out, &count, OP_FNEGATE, 3U, operands);

			/* (z + w) * 0.5, when the depth moves. */
			if (half_depth) {
				operands[0] = float_type;
				operands[1] = ids[7];
				operands[2] = ids[4];
				operands[3] = ids[5];
				spirv_emit(out, &count, OP_FADD, 4U, operands);
				operands[0] = float_type;
				operands[1] = ids[8];
				operands[2] = ids[7];
				operands[3] = half;
				spirv_emit(out, &count, OP_FMUL, 4U, operands);
			}

			/* The vector made again (x, -y or y, (z + w) / 2 or z, w; no insert, which some compilers lack), and stored. */
			operands[0] = vector_type;
			operands[1] = ids[9];
			operands[2] = ids[2];
			operands[3] = ids[3];
			if (flip)
				operands[3] = ids[6];
			operands[4] = ids[4];
			if (half_depth)
				operands[4] = ids[8];
			operands[5] = ids[5];
			spirv_emit(out, &count, OP_COMPOSITE_CONSTRUCT, 6U, operands);
			operands[0] = ids[0];
			operands[1] = ids[9];
			spirv_emit(out, &count, OP_STORE, 2U, operands);
		}

		/* The instruction itself. */
		memcpy(out + count, code + at, length * sizeof(uint32_t));
		count += length;
	}

	/* Succeeded: the new id bound and the rewritten module. */
	out[3] = next;
	*out_words = count;
	spirv_close(&module);
	return out;
}

/* Indexes a module's ids, names and decorations; nonzero when it is not SPIR-V this reader can walk. */
static int
spirv_open(
	struct spirv_module *module,
	const uint32_t *code,
	size_t words)
{
	size_t at;
	uint32_t opcode;
	uint32_t length;
	uint32_t result;

	/* A header with the magic and a sane bound. */
	memset(module, 0, sizeof(*module));
	if (code == NULL || words < SPIRV_HEADER || code[0] != SPIRV_MAGIC)
		return -1;
	if (code[3] == 0U || code[3] > SPIRV_BOUND_MAX)
		return -1;
	module->code = code;
	module->words = words;
	module->bound = code[3];

	/* One table per property, indexed by id. */
	module->defs = calloc(module->bound, sizeof(*module->defs));
	module->names = calloc(module->bound, sizeof(*module->names));
	module->locations = malloc(module->bound * sizeof(*module->locations));
	module->location_words = calloc(module->bound, sizeof(*module->location_words));
	module->index_words = calloc(module->bound, sizeof(*module->index_words));
	module->bindings = malloc(module->bound * sizeof(*module->bindings));
	module->sets = malloc(module->bound * sizeof(*module->sets));
	module->builtins = malloc(module->bound * sizeof(*module->builtins));
	module->array_strides = malloc(module->bound * sizeof(*module->array_strides));
	module->blocks = calloc(module->bound, 1U);
	if (module->defs == NULL || module->names == NULL || module->locations == NULL || module->location_words == NULL ||
	    module->index_words == NULL || module->bindings == NULL || module->sets == NULL || module->builtins == NULL || module->array_strides == NULL ||
	    module->blocks == NULL) {
		spirv_close(module);
		return -1;
	}

	/* No id has a decoration yet. */
	memset(module->locations, 0xff, module->bound * sizeof(*module->locations));
	memset(module->bindings, 0xff, module->bound * sizeof(*module->bindings));
	memset(module->sets, 0xff, module->bound * sizeof(*module->sets));
	memset(module->builtins, 0xff, module->bound * sizeof(*module->builtins));
	memset(module->array_strides, 0xff, module->bound * sizeof(*module->array_strides));

	/* Each instruction: its length checked, its result id, and names, decorations and the entry point. */
	for (at = SPIRV_HEADER; at < words; at += length) {
		opcode = code[at] & 0xffffU;
		length = code[at] >> 16;
		if (length == 0U || at + length > words) {
			spirv_close(module);
			return -1;
		}

		/* The instructions whose result id is their first operand. */
		result = 0U;
		switch (opcode) {
		case OP_TYPE_BOOL:
		case OP_TYPE_INT:
		case OP_TYPE_FLOAT:
		case OP_TYPE_VECTOR:
		case OP_TYPE_MATRIX:
		case OP_TYPE_IMAGE:
		case OP_TYPE_SAMPLED_IMAGE:
		case OP_TYPE_ARRAY:
		case OP_TYPE_STRUCT:
		case OP_TYPE_POINTER:
			if (length > 1U)
				result = code[at + 1U];
			break;
		case OP_CONSTANT:
		case OP_VARIABLE:
		case OP_FUNCTION:
			if (length > 2U)
				result = code[at + 2U];
			break;
		default:
			break;
		}

		/* Where the id is defined. */
		if (result != 0U && result < module->bound)
			module->defs[result] = at;

		/* The first entry point names the function and the stage. */
		if (opcode == OP_ENTRY_POINT && module->entry == 0U && length > 2U) {
			module->model = code[at + 1U];
			module->entry = code[at + 2U];
		}

		/* A name. */
		if (opcode == OP_NAME && length > 2U && code[at + 1U] < module->bound)
			module->names[code[at + 1U]] = spirv_string(code, at, length, 2U);

		/* A decoration. */
		if (opcode == OP_DECORATE && length > 2U && code[at + 1U] < module->bound)
			spirv_decorate(module, at, length);
	}

	/* Succeeded: the module is indexed. */
	return 0;
}

/* Releases a module's tables. */
static void
spirv_close(
	struct spirv_module *module)
{
	/* Every table. */
	free(module->defs);
	free(module->names);
	free(module->locations);
	free(module->location_words);
	free(module->index_words);
	free(module->bindings);
	free(module->sets);
	free(module->builtins);
	free(module->array_strides);
	free(module->blocks);
	memset(module, 0, sizeof(*module));
}

/* Records one OpDecorate of an id. */
static void
spirv_decorate(
	struct spirv_module *module,
	size_t at,
	uint32_t length)
{
	const uint32_t *code;
	uint32_t target;
	uint32_t value;

	/* The target, and the decoration's first operand when it has one. */
	code = module->code;
	target = code[at + 1U];
	value = SPIRV_NONE;
	if (length > 3U)
		value = code[at + 3U];

	/* The decorations this reader keeps. */
	switch (code[at + 2U]) {
	case DECORATION_LOCATION:
		module->locations[target] = value;
		module->location_words[target] = at + 3U;
		break;
	case DECORATION_INDEX:
		if (length > 3U)
			module->index_words[target] = at + 3U;
		break;
	case DECORATION_BINDING:
		module->bindings[target] = value;
		break;
	case DECORATION_SET:
		module->sets[target] = value;
		break;
	case DECORATION_BUILT_IN:
		module->builtins[target] = value;
		break;
	case DECORATION_ARRAY_STRIDE:
		module->array_strides[target] = value;
		break;
	case DECORATION_BLOCK:
		module->blocks[target] = 1U;
		break;
	default:
		break;
	}
}

/* Returns the string literal starting at an instruction's operand, or NULL when it is not terminated inside the instruction. */
static const char *
spirv_string(
	const uint32_t *code,
	size_t at,
	uint32_t length,
	uint32_t first)
{
	const char *text;
	size_t bytes;
	size_t index;

	/* The bytes from the operand to the instruction's end. */
	if (first >= length)
		return NULL;
	text = (const char *)(code + at + first);
	bytes = (size_t)(length - first) * sizeof(uint32_t);

	/* A terminator must be among them. */
	for (index = 0U; index < bytes; index++) {
		if (text[index] == '\0')
			return text;
	}

	/* None: not a string. */
	return NULL;
}

/* Returns a decoration's operand for a member of a struct, or SPIRV_NONE. */
static uint32_t
spirv_member_decoration(
	struct spirv_module *module,
	uint32_t structure,
	uint32_t member,
	uint32_t decoration)
{
	const uint32_t *code;
	size_t at;
	uint32_t length;

	/* The OpMemberDecorate for it, if any. */
	code = module->code;
	for (at = SPIRV_HEADER; at < module->words; at += length) {
		length = code[at] >> 16;
		if ((code[at] & 0xffffU) == OP_FUNCTION)
			break;
		if ((code[at] & 0xffffU) != OP_MEMBER_DECORATE || length < 4U)
			continue;
		if (code[at + 1U] != structure || code[at + 2U] != member || code[at + 3U] != decoration)
			continue;

		/* A decoration without an operand still says it is there. */
		if (length < 5U)
			return 0U;
		return code[at + 4U];
	}

	/* The member does not have it. */
	return SPIRV_NONE;
}

/* Returns the name of a member of a struct, or NULL. */
static const char *
spirv_member_name(
	struct spirv_module *module,
	uint32_t structure,
	uint32_t member)
{
	const uint32_t *code;
	const char *name;
	size_t at;
	uint32_t length;

	/* The OpMemberName for it, if any. */
	code = module->code;
	for (at = SPIRV_HEADER; at < module->words; at += length) {
		length = code[at] >> 16;
		if ((code[at] & 0xffffU) == OP_FUNCTION)
			break;
		if ((code[at] & 0xffffU) != OP_MEMBER_NAME || length < 4U)
			continue;
		if (code[at + 1U] != structure || code[at + 2U] != member)
			continue;

		/* The name after the two ids. */
		name = spirv_string(code, at, length, 3U);
		return name;
	}

	/* The member has no name. */
	return NULL;
}

/* Returns the type a pointer type points to, and its storage class; 0 when the id is not a pointer type. */
static uint32_t
spirv_pointee(
	struct spirv_module *module,
	uint32_t pointer,
	uint32_t *storage)
{
	const uint32_t *code;
	size_t at;

	/* An OpTypePointer. */
	code = module->code;
	if (pointer >= module->bound || module->defs[pointer] == 0U)
		return 0U;
	at = module->defs[pointer];
	if ((code[at] & 0xffffU) != OP_TYPE_POINTER || (code[at] >> 16) < 4U)
		return 0U;

	/* Its storage class and type. */
	*storage = code[at + 2U];
	if (code[at + 3U] >= module->bound)
		return 0U;
	return code[at + 3U];
}

/* Returns the value of a 32-bit integer constant, or 0 when the id is not one. */
static uint32_t
spirv_constant(
	struct spirv_module *module,
	uint32_t id)
{
	const uint32_t *code;
	size_t at;

	/* An OpConstant with one word of value. */
	code = module->code;
	if (id >= module->bound || module->defs[id] == 0U)
		return 0U;
	at = module->defs[id];
	if ((code[at] & 0xffffU) != OP_CONSTANT || (code[at] >> 16) < 4U)
		return 0U;

	/* Its value. */
	return code[at + 3U];
}

/* Describes a scalar, vector, matrix or sampled-image type as a uniform's type; nonzero for any other type. */
static int
spirv_leaf(
	struct spirv_module *module,
	uint32_t type,
	struct gles_uniform *uniform)
{
	const uint32_t *code;
	size_t at;
	size_t image;
	uint32_t opcode;
	int status;

	/* The type's instruction. */
	code = module->code;
	if (type >= module->bound || module->defs[type] == 0U)
		return -1;
	at = module->defs[type];
	opcode = code[at] & 0xffffU;

	/* What kind of type it is. */
	switch (opcode) {
	case OP_TYPE_FLOAT:
		/* A float, or a double (64 bits wide). */
		uniform->base = 0U;
		if ((code[at] >> 16) >= 3U && code[at + 2U] == 64U)
			uniform->base = 4U;
		uniform->components = 1U;
		uniform->columns = 1U;
		uniform->type = gles_gl_type(uniform->base, 1U, 1U);
		return 0;
	case OP_TYPE_INT:
		uniform->base = 1U;
		if (code[at + 3U] == 0U)
			uniform->base = 2U;
		uniform->components = 1U;
		uniform->columns = 1U;
		uniform->type = gles_gl_type(uniform->base, 1U, 1U);
		return 0;
	case OP_TYPE_BOOL:
		uniform->base = 3U;
		uniform->components = 1U;
		uniform->columns = 1U;
		uniform->type = GL_BOOL;
		return 0;
	case OP_TYPE_VECTOR:
		status = spirv_leaf(module, code[at + 2U], uniform);
		if (status != 0 || code[at + 3U] < 2U || code[at + 3U] > 4U)
			return -1;
		uniform->components = code[at + 3U];
		uniform->type = gles_gl_type(uniform->base, uniform->components, 1U);
		return 0;
	case OP_TYPE_MATRIX:
		status = spirv_leaf(module, code[at + 2U], uniform);
		if (status != 0 || code[at + 3U] < 2U || code[at + 3U] > 4U)
			return -1;
		uniform->columns = code[at + 3U];
		uniform->type = gles_gl_type(uniform->base, uniform->components, uniform->columns);
		return 0;
	case OP_TYPE_SAMPLED_IMAGE:
		uniform->sampler = 1;
		uniform->components = 1U;
		uniform->columns = 1U;
		uniform->type = GL_SAMPLER_2D;

		/* The image's dimension, comparison, layers and texel kind name the sampler's type. */
		image = 0U;
		if (code[at + 2U] < module->bound)
			image = module->defs[code[at + 2U]];
		if (image != 0U && (code[image] & 0xffffU) == OP_TYPE_IMAGE)
			uniform->type = spirv_sampler_type(module, image);
		return 0;
	case OP_TYPE_IMAGE:
		/* An image read without a sampler: a buffer texture's texels (desktop GL's samplerBuffer). */
		if (code[at + 3U] != SPIRV_DIM_BUFFER)
			return -1;
		uniform->sampler = 1;
		uniform->components = 1U;
		uniform->columns = 1U;
		uniform->type = spirv_sampler_type(module, at);
		return 0;
	default:
		break;
	}

	/* Anything else is not a leaf. */
	return -1;
}

/*
 * Adds the leaves of a type in the uniform block to the uniforms: a
 * struct's members by name, an array of structs element by element, and a
 * leaf or an array of leaves as one uniform.
 */
static int
spirv_flatten(
	struct spirv_module *module,
	struct gles_spirv *out,
	uint32_t type,
	const char *prefix,
	uint32_t offset,
	uint32_t matrix_stride)
{
	struct gles_uniform *uniform;
	struct gles_uniform leaf;
	char name[GLES_NAME];
	const uint32_t *code;
	const char *member_name;
	size_t at;
	uint32_t opcode;
	uint32_t members;
	uint32_t member;
	uint32_t member_offset;
	uint32_t element_type;
	uint32_t length;
	uint32_t stride;
	uint32_t element;
	int status;

	/* The type's instruction. */
	code = module->code;
	if (type >= module->bound || module->defs[type] == 0U)
		return -1;
	at = module->defs[type];
	opcode = code[at] & 0xffffU;

	/* A struct: each member under its name. */
	if (opcode == OP_TYPE_STRUCT) {
		members = (code[at] >> 16) - 2U;
		for (member = 0U; member < members; member++) {
			/* The member's name, offset and matrix stride. */
			member_name = spirv_member_name(module, type, member);
			if (member_name == NULL)
				member_name = "";
			member_offset = spirv_member_decoration(module, type, member, DECORATION_OFFSET);
			if (member_offset == SPIRV_NONE)
				return -1;
			if (prefix[0] == '\0') {
				(void)snprintf(name, sizeof(name), "%s", member_name);
			} else {
				(void)snprintf(name, sizeof(name), "%s.%s", prefix, member_name);
			}

			/* Its leaves. */
			stride = spirv_member_decoration(module, type, member, DECORATION_MATRIX_STRIDE);
			status = spirv_flatten(module, out, code[at + 2U + member], name, offset + member_offset, stride);
			if (status != 0)
				return status;
		}

		/* Every member is in. */
		return 0;
	}

	/* An array: of structs element by element, of leaves as one uniform. */
	memset(&leaf, 0, sizeof(leaf));
	leaf.size = 1;
	if (opcode == OP_TYPE_ARRAY) {
		element_type = code[at + 2U];
		length = spirv_constant(module, code[at + 3U]);
		stride = module->array_strides[type];
		if (length == 0U || stride == SPIRV_NONE)
			return -1;

		/* Structs: each element's members under "name[i]". */
		if (module->defs[element_type] != 0U && (code[module->defs[element_type]] & 0xffffU) == OP_TYPE_STRUCT) {
			for (element = 0U; element < length; element++) {
				(void)snprintf(name, sizeof(name), "%s[%u]", prefix, element);
				status = spirv_flatten(module, out, element_type, name, offset + element * stride, matrix_stride);
				if (status != 0)
					return status;
			}

			/* Every element is in. */
			return 0;
		}

		/* Leaves: one uniform with the length and stride. */
		leaf.size = (GLint)length;
		leaf.array_stride = stride;
		type = element_type;
	}

	/* The leaf. */
	status = spirv_leaf(module, type, &leaf);
	if (status != 0)
		return -1;
	uniform = spirv_add_uniform(out);
	if (uniform == NULL)
		return -1;

	/* Succeeded: the uniform, where it lives in the block. */
	*uniform = leaf;
	(void)snprintf(uniform->name, sizeof(uniform->name), "%s", prefix);
	uniform->offset = offset;
	uniform->matrix_stride = matrix_stride;
	if (matrix_stride == SPIRV_NONE)
		uniform->matrix_stride = 16U;
	if (uniform->array_stride == 0U)
		uniform->array_stride = uniform->columns * uniform->matrix_stride;
	return 0;
}

/* Returns a new, cleared uniform at the end of the list, or NULL when there is no memory. */
static struct gles_uniform *
spirv_add_uniform(
	struct gles_spirv *out)
{
	struct gles_uniform *grown;
	unsigned capacity;

	/* The list grows by doubling. */
	if (out->uniform_count == out->uniform_capacity) {
		capacity = out->uniform_capacity * 2U;
		if (capacity == 0U)
			capacity = 16U;
		grown = realloc(out->uniforms, capacity * sizeof(*grown));
		if (grown == NULL)
			return NULL;
		out->uniforms = grown;
		out->uniform_capacity = capacity;
	}

	/* Succeeded: the new entry, cleared. */
	memset(&out->uniforms[out->uniform_count], 0, sizeof(out->uniforms[0]));
	out->uniform_count++;
	return &out->uniforms[out->uniform_count - 1U];
}

/* Adds one global variable to the interface; nonzero when it is of a kind this library cannot use. */
static int
spirv_variable(
	struct spirv_module *module,
	struct gles_spirv *out,
	size_t at)
{
	struct gles_spirv_variable *variable;
	struct gles_uniform *uniform;
	struct gles_uniform leaf;
	const uint32_t *code;
	const char *name;
	uint32_t id;
	uint32_t storage;
	uint32_t pointee;
	uint32_t element;
	uint32_t length;
	int status;

	/* The variable, its storage class and type. */
	code = module->code;
	id = code[at + 2U];
	pointee = spirv_pointee(module, code[at + 1U], &storage);
	if (pointee == 0U || module->defs[pointee] == 0U)
		return -1;
	name = module->names[id];
	if (name == NULL)
		name = "";

	/* Inputs and outputs, built-ins and blocks of built-ins left out. */
	if (storage == STORAGE_INPUT || storage == STORAGE_OUTPUT) {
		if (module->builtins[id] != SPIRV_NONE)
			return 0;
		if ((code[module->defs[pointee]] & 0xffffU) == OP_TYPE_STRUCT)
			return 0;

		/* A geometry shader's array of blocks (gl_in, an input block's vertices) is left out too. */
		element = pointee;
		if ((code[module->defs[pointee]] & 0xffffU) == OP_TYPE_ARRAY)
			element = code[module->defs[pointee] + 2U];
		if (element < module->bound && module->defs[element] != 0U && (code[module->defs[element]] & 0xffffU) == OP_TYPE_STRUCT)
			return 0;
		if (module->locations[id] == SPIRV_NONE)
			return -1;

		/* The next entry of its list. */
		variable = NULL;
		if (storage == STORAGE_INPUT && out->input_count < GLES_ATTRIBS * 2U)
			variable = &out->inputs[out->input_count++];
		if (storage == STORAGE_OUTPUT && out->output_count < GLES_ATTRIBS * 2U)
			variable = &out->outputs[out->output_count++];
		if (variable == NULL)
			return -1;

		/* Its name, location and type. */
		memset(&leaf, 0, sizeof(leaf));
		length = 1U;
		if ((code[module->defs[pointee]] & 0xffffU) == OP_TYPE_ARRAY) {
			length = spirv_constant(module, code[module->defs[pointee] + 3U]);
			pointee = code[module->defs[pointee] + 2U];
		}

		/* The type of one element. */
		status = spirv_leaf(module, pointee, &leaf);
		if (status != 0)
			return -1;
		(void)snprintf(variable->name, sizeof(variable->name), "%s", name);
		variable->location = module->locations[id];
		variable->location_word = module->location_words[id];
		variable->id = id;
		variable->index = 0U;
		variable->index_word = module->index_words[id];
		if (variable->index_word != 0U)
			variable->index = code[variable->index_word];
		variable->type = leaf.type;
		variable->size = (GLint)length;
		variable->components = leaf.components;
		return 0;
	}

	/* The storage buffer a vertex shader captures outputs into (transform feedback) is the link's, not the interface's. */
	if (storage == STORAGE_UNIFORM && module->bindings[id] == GLES_CAPTURE_BINDING)
		return 0;

	/*
	 * A compute shader's shader storage blocks (ws101-p009: the GLSL
	 * compiler's link lists them) and its shared variables are not the
	 * uniforms' interface either.
	 */
	if (storage == STORAGE_UNIFORM &&
	    module->model == SPIRV_MODEL_COMPUTE &&
	    module->bindings[id] != SPIRV_NONE &&
	    module->bindings[id] >= GLES_STORAGE_FIRST_BINDING &&
	    module->bindings[id] < GLES_STORAGE_FIRST_BINDING + GLES_STORAGE_BINDINGS)
		return 0;
	if (storage == STORAGE_WORKGROUP && module->model == SPIRV_MODEL_COMPUTE)
		return 0;

	/* Nor are its image uniforms (the compiler's link lists them, by unit). */
	if (storage == STORAGE_UNIFORM_CONSTANT &&
	    module->model == SPIRV_MODEL_COMPUTE &&
	    module->bindings[id] != SPIRV_NONE &&
	    module->bindings[id] >= GLES_IMAGE_FIRST_BINDING &&
	    module->bindings[id] < GLES_IMAGE_FIRST_BINDING + GLES_IMAGE_UNITS)
		return 0;

	/*
	 * A uniform block other than the default one (binding 0), or an array
	 * of them (a descriptor an element): its binding, how many
	 * descriptors it has, and its type's name are recorded.
	 */
	if (storage == STORAGE_UNIFORM && module->bindings[id] != SPIRV_NONE && module->bindings[id] != 0U) {
		length = 1U;
		if ((code[module->defs[pointee]] & 0xffffU) == OP_TYPE_ARRAY) {
			length = spirv_constant(module, code[module->defs[pointee] + 3U]);
			pointee = code[module->defs[pointee] + 2U];
		}
		if (pointee >= module->bound ||
		    !module->blocks[pointee] ||
		    length == 0U ||
		    length > GLES_NAMED_BLOCKS)
			return -1;
		if (out->named_count == GLES_NAMED_BLOCKS)
			return -1;
		out->named_bindings[out->named_count] = module->bindings[id];
		out->named_counts[out->named_count] = length;
		out->named_names[out->named_count][0] = '\0';
		if (module->names[pointee] != NULL)
			(void)snprintf(out->named_names[out->named_count], GLES_NAME, "%s", module->names[pointee]);
		out->named_count++;
		return 0;
	}

	/* The default uniform block: set 0, its leaves by name. */
	if (storage == STORAGE_UNIFORM) {
		if (!module->blocks[pointee] || out->has_block)
			return -1;
		if (module->sets[id] != SPIRV_NONE && module->sets[id] != 0U)
			return -1;
		out->has_block = 1;
		out->block_binding = module->bindings[id];
		if (out->block_binding == SPIRV_NONE)
			out->block_binding = 0U;
		status = spirv_flatten(module, out, pointee, "", 0U, SPIRV_NONE);
		return status;
	}

	/* A sampler, or an array of them (ws068-p004: one binding, a descriptor an element): set 0, its binding. */
	if (storage == STORAGE_UNIFORM_CONSTANT) {
		length = 1U;
		if ((code[module->defs[pointee]] & 0xffffU) == OP_TYPE_ARRAY) {
			length = spirv_constant(module, code[module->defs[pointee] + 3U]);
			pointee = code[module->defs[pointee] + 2U];
		}

		/* One to sixteen samplers of a kind GL names. */
		if (length == 0U || length > GLES_UNITS || pointee >= module->bound || module->defs[pointee] == 0U)
			return -1;
		memset(&leaf, 0, sizeof(leaf));
		status = spirv_leaf(module, pointee, &leaf);
		if (status != 0 || !leaf.sampler)
			return -1;
		if (module->sets[id] != SPIRV_NONE && module->sets[id] != 0U)
			return -1;
		uniform = spirv_add_uniform(out);
		if (uniform == NULL)
			return -1;
		*uniform = leaf;
		(void)snprintf(uniform->name, sizeof(uniform->name), "%s", name);
		uniform->size = (GLint)length;
		uniform->binding = module->bindings[id];
		if (uniform->binding == SPIRV_NONE)
			uniform->binding = 1U;
		return 0;
	}

	/* Anything else (private variables are not global in shaders of this kind). */
	return -1;
}

/*
 * Returns GL's type of a sampler of an OpTypeImage (at a word of the
 * module): by its dimension (2D, 3D, cube), whether it is a depth image
 * compared (a shadow sampler), arrayed, and whether its texels are signed
 * or unsigned integers.
 */
static GLenum
spirv_sampler_type(
	struct spirv_module *module,
	size_t image)
{
	const uint32_t *code;
	size_t sampled;
	uint32_t dimension;
	unsigned base;

	/* The image's operands: sampled type, Dim, Depth, Arrayed. */
	code = module->code;
	dimension = code[image + 3U];

	/* A depth image compared. */
	if (code[image + 4U] == 1U) {
		if (dimension == SPIRV_DIM_CUBE)
			return GL_SAMPLER_CUBE_SHADOW;
		if (code[image + 5U] != 0U)
			return GL_SAMPLER_2D_ARRAY_SHADOW;
		return GL_SAMPLER_2D_SHADOW;
	}

	/* The texel kind: 0 floats, 1 signed, 2 unsigned integers. */
	base = 0U;
	sampled = 0U;
	if (code[image + 2U] < module->bound)
		sampled = module->defs[code[image + 2U]];
	if (sampled != 0U && (code[sampled] & 0xffffU) == OP_TYPE_INT) {
		base = 2U;
		if (code[sampled + 3U] != 0U)
			base = 1U;
	}

	/* A multisample image. */
	if (code[image + 6U] != 0U) {
		if (base == 1U)
			return GL_INT_SAMPLER_2D_MULTISAMPLE;
		if (base == 2U)
			return GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE;
		return GL_SAMPLER_2D_MULTISAMPLE;
	}

	/* A buffer texture's texels. */
	if (dimension == SPIRV_DIM_BUFFER) {
		if (base == 1U)
			return GL_INT_SAMPLER_BUFFER;
		if (base == 2U)
			return GL_UNSIGNED_INT_SAMPLER_BUFFER;
		return GL_SAMPLER_BUFFER;
	}

	/* A cube map. */
	if (dimension == SPIRV_DIM_CUBE) {
		if (base == 1U)
			return GL_INT_SAMPLER_CUBE;
		if (base == 2U)
			return GL_UNSIGNED_INT_SAMPLER_CUBE;
		return GL_SAMPLER_CUBE;
	}

	/* A 3D texture. */
	if (dimension == SPIRV_DIM_3D) {
		if (base == 1U)
			return GL_INT_SAMPLER_3D;
		if (base == 2U)
			return GL_UNSIGNED_INT_SAMPLER_3D;
		return GL_SAMPLER_3D;
	}

	/* An array of 2D layers. */
	if (code[image + 5U] != 0U) {
		if (base == 1U)
			return GL_INT_SAMPLER_2D_ARRAY;
		if (base == 2U)
			return GL_UNSIGNED_INT_SAMPLER_2D_ARRAY;
		return GL_SAMPLER_2D_ARRAY;
	}

	/* A 2D texture. */
	if (base == 1U)
		return GL_INT_SAMPLER_2D;
	if (base == 2U)
		return GL_UNSIGNED_INT_SAMPLER_2D;
	return GL_SAMPLER_2D;
}

/* Returns the id of a type declared with the given operands after its result, or 0 when the module has none. */
static uint32_t
spirv_find_type(
	struct spirv_module *module,
	uint32_t opcode,
	uint32_t first,
	uint32_t second,
	uint32_t operands)
{
	const uint32_t *code;
	size_t at;
	uint32_t length;

	/* The first declaration that matches. */
	code = module->code;
	for (at = SPIRV_HEADER; at < module->words; at += length) {
		length = code[at] >> 16;
		if ((code[at] & 0xffffU) == OP_FUNCTION)
			break;
		if ((code[at] & 0xffffU) != opcode || length < 2U + operands)
			continue;
		if (code[at + 2U] != first)
			continue;
		if (operands > 1U && code[at + 3U] != second)
			continue;

		/* This one. */
		return code[at + 1U];
	}

	/* The module declares none. */
	return 0U;
}

/* Appends one instruction. */
static void
spirv_emit(
	uint32_t *out,
	size_t *count,
	uint32_t opcode,
	uint32_t words,
	const uint32_t *operands)
{
	/* The first word holds the length and the opcode. */
	out[*count] = ((words + 1U) << 16) | opcode;
	memcpy(out + *count + 1U, operands, words * sizeof(uint32_t));
	*count += words + 1U;
}
