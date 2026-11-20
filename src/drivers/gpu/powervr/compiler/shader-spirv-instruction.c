/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPIR-V reader's instructions: memory access through pointers,
 * composites, arithmetic, the GLSL.std.450 functions and texture
 * lookups, each split into the intermediate form's scalar words.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/compiler/shader-ir.h"
#include "drivers/gpu/powervr/compiler/shader-private.h"
#include "drivers/gpu/powervr/compiler/shader-spirv.h"
#include "drivers/gpu/compiler/spirv.h"

/*
 * The sine polynomial: the odd powers' coefficients of sin(y) for y in
 * [-pi, pi], highest first, and the turn the argument is reduced by.
 * The Taylor terms to y^15 err by less than 1e-6 over the range.
 */
static const uint32_t sine_coefficients[8] = {
	0xab573f9fU,	/* -1/15! */
	0x2f309231U,	/* 1/13! */
	0xb2d7322bU,	/* -1/11! */
	0x3638ef1dU,	/* 1/9! */
	0xb9500d01U,	/* -1/7! */
	0x3c088889U,	/* 1/5! */
	0xbe2aaaabU,	/* -1/3! */
	0x3f800000U	/* 1 */
};

static int reader_access_chain(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
static int reader_load(struct shader_spirv_reader *reader, uint32_t type, uint32_t result, uint32_t pointer);
static int reader_store(struct shader_spirv_reader *reader, uint32_t pointer, uint32_t object);
static int reader_memory_address(struct shader_spirv_reader *reader, const struct shader_spirv_id *pointer, struct powervr_ir_operand address[3]);
static int reader_shape_load(struct shader_spirv_reader *reader, const struct shader_spirv_id *pointer, uint32_t count, struct powervr_ir_operand *operands);
static int reader_arithmetic(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length, uint32_t *handled);
static int reader_composite(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length, uint32_t *handled);
static int reader_extended(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
static struct powervr_ir_operand word_operation(struct shader_spirv_reader *reader, uint32_t opcode, struct powervr_ir_operand x, struct powervr_ir_operand y);
static struct powervr_ir_operand derivative(struct shader_spirv_reader *reader, struct powervr_ir_operand x, uint32_t which);
static struct powervr_ir_operand float_truncate(struct shader_spirv_reader *reader, struct powervr_ir_operand x);
static struct powervr_ir_operand round_even(struct shader_spirv_reader *reader, struct powervr_ir_operand x);
static struct powervr_ir_operand integer_sign(struct shader_spirv_reader *reader, uint32_t instruction, struct powervr_ir_operand x);
static struct powervr_ir_operand transcendental(struct shader_spirv_reader *reader, uint32_t instruction, struct powervr_ir_operand x, struct powervr_ir_operand y);
static struct powervr_ir_operand natural_exp(struct shader_spirv_reader *reader, struct powervr_ir_operand x);
static struct powervr_ir_operand natural_log(struct shader_spirv_reader *reader, struct powervr_ir_operand x);
static struct powervr_ir_operand square_root(struct shader_spirv_reader *reader, struct powervr_ir_operand x);
static struct powervr_ir_operand select_float(struct shader_spirv_reader *reader, struct powervr_ir_operand condition, struct powervr_ir_operand yes, struct powervr_ir_operand no);
static struct powervr_ir_operand arctangent(struct shader_spirv_reader *reader, struct powervr_ir_operand y, struct powervr_ir_operand x);
static int reader_geometry(struct shader_spirv_reader *reader, uint32_t instruction, struct powervr_ir_operand *result, uint32_t count, const struct powervr_ir_operand *a, uint32_t a_count, const struct powervr_ir_operand *b, uint32_t b_count, const struct powervr_ir_operand *c, uint32_t c_count);
static struct powervr_ir_operand dot_product(struct shader_spirv_reader *reader, const struct powervr_ir_operand *a, const struct powervr_ir_operand *b, uint32_t count);
static struct powervr_ir_operand minor_determinant(struct shader_spirv_reader *reader, const struct powervr_ir_operand *matrix, uint32_t size, uint32_t rows, uint32_t columns);
static int reader_packing(struct shader_spirv_reader *reader, uint32_t instruction, struct powervr_ir_operand *result, uint32_t count, const struct powervr_ir_operand *a, uint32_t a_count);
static int reader_modf(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length, const struct powervr_ir_operand *a, uint32_t a_count, uint32_t count);
static int reader_matrix(struct shader_spirv_reader *reader, const uint32_t *words, const struct powervr_ir_operand *a, uint32_t a_count, const struct powervr_ir_operand *b, uint32_t b_count, uint32_t count);
static int reader_dynamic_component(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length, const struct powervr_ir_operand *a, uint32_t a_count, const struct powervr_ir_operand *b, uint32_t b_count, uint32_t count);
static int store_words(struct shader_spirv_reader *reader, uint32_t pointer, const struct powervr_ir_operand *operands, uint32_t count);
static struct powervr_ir_operand register_index_test(struct shader_spirv_reader *reader, const struct shader_spirv_id *pointer, uint32_t element);
/*
 * The ids of a lookup's image operands (texture_operands) by what they
 * are, and the most data words a lookup is put together from before the
 * count the core takes is checked.
 */
#define TEXTURE_BIAS			0U
#define TEXTURE_LOD			1U
#define TEXTURE_GRADIENT_X		2U
#define TEXTURE_GRADIENT_Y		3U
#define TEXTURE_CONSTANT_OFFSET		4U
#define TEXTURE_OFFSET			5U
#define TEXTURE_SAMPLE			6U
#define TEXTURE_OPERAND_IDS		7U
#define TEXTURE_DATA_MAX		8U

/*
 * The image state's kind of a strided (linear) image.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/csbgen/rogue/texstate.xml (TEXTYPE
 * STRIDE 4).
 */
#define TEXTURE_TYPE_STRIDE		4U

/* The shape of a lookup's image: its coordinates, whether it is a cube, arrayed or multisampled, and whether its texels are integers. */
struct texture_shape {
	uint32_t dimensions;
	uint32_t cube;
	uint32_t arrayed;
	uint32_t multisampled;
	uint32_t integer;
};

static int reader_sample(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
static int reader_texture(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
static int texture_shape_of(struct shader_spirv_reader *reader, const struct shader_spirv_id *image, struct texture_shape *shape);
static int texture_operands(const uint32_t *words, uint32_t length, uint32_t at, uint32_t mask, uint32_t ids[TEXTURE_OPERAND_IDS]);
static int texture_fetch_last(struct shader_spirv_reader *reader, const struct shader_spirv_id *image, uint32_t lod_id, uint32_t dimensions, struct powervr_ir_operand last[3]);
static int texture_layer_address(struct shader_spirv_reader *reader, const struct shader_spirv_id *image, uint32_t image_request, struct powervr_ir_operand layer, uint32_t fetch, struct powervr_ir_operand address[2]);
static int texture_offsets(struct shader_spirv_reader *reader, uint32_t id, uint32_t dimensions, struct powervr_ir_operand *packed);
static int point_sampler_request(struct shader_spirv_reader *reader, uint32_t *index);
static int compare_depth(struct shader_spirv_reader *reader, const struct shader_spirv_id *image, struct powervr_ir_operand depth, struct powervr_ir_operand reference, struct powervr_ir_operand *result);
static int reader_texture_size(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
static struct powervr_ir_operand sine(struct shader_spirv_reader *reader, struct powervr_ir_operand x, uint32_t quarter);
static int same_operand(const struct powervr_ir_operand *a, const struct powervr_ir_operand *b);
static int reader_elect(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
static int reader_barrier(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
static int reader_atomic(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
static struct powervr_ir_operand memory_load(struct shader_spirv_reader *reader, const struct powervr_ir_operand *address, const struct powervr_ir_operand *offset, uint32_t flags);
static void memory_store(struct shader_spirv_reader *reader, const struct powervr_ir_operand *address, const struct powervr_ir_operand *offset, struct powervr_ir_operand word);
static struct powervr_ir_operand compare_exchange(struct shader_spirv_reader *reader, const struct powervr_ir_operand *address, struct powervr_ir_operand value, struct powervr_ir_operand comparator);

/* Reads one instruction inside a block. */
int
shader_spirv_reader_instruction(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length)
{
	struct powervr_ir_operand *operands;
	struct shader_spirv_id *function;
	struct shader_spirv_id *id;
	uint32_t opcode;
	uint32_t handled;
	uint32_t at;
	uint32_t parameter;
	uint32_t argument;
	uint32_t result;
	uint32_t words_count;
	int error;

	/* Dispatches on the opcode. */
	opcode = words[0] & 0xffffU;
	switch (opcode) {
	case 5U:	/* OpName */
	case 6U:	/* OpMemberName */
	case 8U:	/* OpLine */
	case 317U:	/* OpNoLine */
	case OP_PHI:
	case OP_SELECTION_MERGE:
	case OP_LOOP_MERGE:
		/* Phis are moved on their edges; merges were noted in the first pass. */
		return 0;
	case OP_UNDEF:
		error = shader_spirv_reader_constant(reader, words, length);
		if (error != 0)
			return error;

		/* Succeeded: the undefined value is a constant. */
		return 0;
	case OP_VARIABLE:
		/* A function variable: values for its words, set to its initializer if it has one. */
		if (length < 4 || words[2] >= reader->bound)
			return EINVAL;
		if (words[3] != STORAGE_FUNCTION || words[1] >= reader->bound)
			return ENOTSUP;
		id = &reader->ids[words[2]];
		id->kind = ID_POINTER;
		id->storage = words[3];
		id->type = reader->ids[words[1]].type;
		id->variable = words[2];
		words_count = shader_spirv_scalar_count(reader, id->type);
		if (words_count == 0 || words_count > COMPOSITE_MAX * 2U)
			return ENOTSUP;
		id->first = shader_spirv_new_value(reader, 1);
		for (at = 1; at < words_count; at++)
			(void)shader_spirv_new_value(reader, 1);
		if (length > 4) {
			error = shader_spirv_id_operands(reader, words[4], &operands, &result);
			if (error != 0)
				return error;
			for (at = 0; at < result && at < words_count; at++)
				shader_spirv_emit(reader, POWERVR_IR_MOVE, id->first + at, &operands[at], NULL, NULL, 0, 0);
		}
		return reader->error;
	case OP_LOAD:
		if (length < 4)
			return EINVAL;
		error = reader_load(reader, words[1], words[2], words[3]);
		if (error != 0)
			return error;

		/* Succeeded: the load is read. */
		return 0;
	case OP_STORE:
		if (length < 3)
			return EINVAL;
		error = reader_store(reader, words[1], words[2]);
		if (error != 0)
			return error;

		/* Succeeded: the store is read. */
		return 0;
	case OP_ACCESS_CHAIN:
	case OP_IN_BOUNDS_ACCESS_CHAIN:
		error = reader_access_chain(reader, words, length);
		if (error != 0)
			return error;

		/* Succeeded: the access chain is read. */
		return 0;
	case OP_FUNCTION_CALL:
		/* Binds the parameters to the arguments, then inlines the body. */
		if (length < 4 || words[3] >= reader->bound)
			return EINVAL;
		function = &reader->ids[words[3]];
		if (function->kind != ID_FUNCTION)
			return EINVAL;
		at = function->word + (reader->words[function->word] >> 16);
		for (argument = 4; argument < length; argument++) {
			if ((reader->words[at] & 0xffffU) != OP_FUNCTION_PARAMETER)
				return EINVAL;
			parameter = reader->words[at + 2];
			if (parameter >= reader->bound || words[argument] >= reader->bound)
				return EINVAL;
			reader->ids[parameter] = reader->ids[words[argument]];
			at += reader->words[at] >> 16;
		}
		error = shader_spirv_reader_function(reader, words[3], &result);
		if (error != 0)
			return error;
		if (words[2] >= reader->bound)
			return EINVAL;
		if (reader->result_value != 0) {
			/* The values every return of the function left its result in. */
			error = shader_spirv_result_begin(reader, words[2], words[1], reader->result_words, &operands);
			if (error != 0)
				return error;
			for (at = 0; at < reader->result_words; at++)
				operands[at] = shader_spirv_value_operand(reader->result_value + at, 0);
		} else if (result != 0) {
			reader->ids[words[2]] = reader->ids[result];
		}
		return reader->error;
	case OP_EXT_INST:
		error = reader_extended(reader, words, length);
		if (error != 0)
			return error;

		/* Succeeded: the extended instruction is read. */
		return 0;
	case OP_SAMPLED_IMAGE:
	case OP_IMAGE:
		/* The image's binding carries over, with the result's type. */
		if (length < 4 || words[1] >= reader->bound || words[2] >= reader->bound || words[3] >= reader->bound)
			return EINVAL;
		if (reader->ids[words[3]].kind != ID_SAMPLED_IMAGE)
			return ENOTSUP;
		reader->ids[words[2]] = reader->ids[words[3]];
		reader->ids[words[2]].type = words[1];
		return 0;
	case OP_IMAGE_SAMPLE_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_EXPLICIT_LOD:
	case OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_DREF_IMPLICIT_LOD:
	case OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD:
	case OP_IMAGE_FETCH:
		error = reader_texture(reader, words, length);
		if (error != 0)
			return error;

		/* Succeeded: the lookup is read. */
		return 0;
	case OP_IMAGE_QUERY_SIZE_LOD:
	case OP_IMAGE_QUERY_SIZE:
		error = reader_texture_size(reader, words, length);
		if (error != 0)
			return error;

		/* Succeeded: the size is read. */
		return 0;
	case OP_CONTROL_BARRIER:
		error = reader_barrier(reader, words, length);
		if (error != 0)
			return error;

		/* Succeeded: the barrier is read. */
		return 0;
	case OP_MEMORY_BARRIER:
		/*
		 * Nothing to order: a store waits until it is done before the
		 * next instruction, and a load of workgroup memory goes around
		 * the caches.
		 */
		return 0;
	case OP_GROUP_NON_UNIFORM_ELECT:
		error = reader_elect(reader, words, length);
		if (error != 0)
			return error;

		/* Succeeded: the election is read. */
		return 0;
	case OP_ATOMIC_LOAD:
	case OP_ATOMIC_STORE:
	case OP_ATOMIC_EXCHANGE:
	case OP_ATOMIC_COMPARE_EXCHANGE:
	case OP_ATOMIC_I_INCREMENT:
	case OP_ATOMIC_I_DECREMENT:
	case OP_ATOMIC_I_ADD:
	case OP_ATOMIC_I_SUB:
	case OP_ATOMIC_S_MIN:
	case OP_ATOMIC_U_MIN:
	case OP_ATOMIC_S_MAX:
	case OP_ATOMIC_U_MAX:
	case OP_ATOMIC_AND:
	case OP_ATOMIC_OR:
	case OP_ATOMIC_XOR:
		error = reader_atomic(reader, words, length);
		if (error != 0)
			return error;

		/* Succeeded: the atomic is read. */
		return 0;
	default:
		break;
	}

	/* Composite shuffling and arithmetic. */
	error = reader_composite(reader, words, length, &handled);
	if (error != 0 || handled != 0)
		return error;
	error = reader_arithmetic(reader, words, length, &handled);
	if (error != 0 || handled != 0)
		return error;

	/* Anything else is outside the subset. */
	return ENOTSUP;
}

/* Reads an access chain into a pointer with an offset and maybe an index. */
static int
reader_access_chain(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length)
{
	struct powervr_ir_operand index;
	struct powervr_ir_operand scale;
	struct powervr_ir_operand scaled;
	struct shader_spirv_member *member;
	struct shader_spirv_id *base;
	struct shader_spirv_id *type;
	struct shader_spirv_id pointer;
	uint32_t memory;
	uint32_t at;
	uint32_t current;
	uint32_t literal;
	uint32_t stride;
	int error;

	/* Starts from the base pointer. */
	if (length < 4 || words[2] >= reader->bound || words[3] >= reader->bound)
		return EINVAL;
	base = &reader->ids[words[3]];
	if (base->kind != ID_POINTER)
		return EINVAL;
	pointer = *base;
	current = base->type;
	memory = 0;
	if (pointer.storage == STORAGE_UNIFORM || pointer.storage == STORAGE_STORAGE_BUFFER || pointer.storage == STORAGE_PUSH_CONSTANT ||
	    pointer.storage == STORAGE_WORKGROUP)
		memory = 1;

	/* Steps through each index. */
	for (at = 4; at < length; at++) {
		if (words[at] >= reader->bound || current >= reader->bound)
			return EINVAL;
		type = &reader->ids[current];
		error = shader_spirv_operand_word(reader, words[at], 0, &index);
		if (error != 0)
			return error;
		literal = index.constant;

		if (type->type_kind == TYPE_STRUCT) {
			/* A member: a constant index. */
			if (index.kind != POWERVR_IR_OPERAND_CONSTANT || literal >= type->count)
				return EINVAL;
			member = &reader->members[type->first + literal];
			if (pointer.storage == STORAGE_WORKGROUP) {
				/* Workgroup memory has no member offsets: the words packed. */
				pointer.offset += member->scalar_offset * 4U;
			} else if (memory != 0) {
				pointer.offset += member->offset;
			} else {
				pointer.offset += member->scalar_offset;
			}
			current = member->type;
			continue;
		}

		/*
		 * An element of an array of buffer descriptors (the first index
		 * into a uniform or storage buffer variable of an array of
		 * blocks): a constant element of the binding, not memory.
		 */
		if (at == 4 &&
		    memory != 0 &&
		    pointer.storage != STORAGE_WORKGROUP &&
		    pointer.storage != STORAGE_PUSH_CONSTANT &&
		    pointer.variable == words[3] &&
		    type->type_kind == TYPE_ARRAY) {
			if (index.kind != POWERVR_IR_OPERAND_CONSTANT || literal >= type->count)
				return ENOTSUP;
			pointer.element = literal;
			current = type->type;
			continue;
		}

		/* An element of an array of samplers: its number among the binding's descriptors. */
		if (pointer.storage == STORAGE_UNIFORM_CONSTANT) {
			if (type->type_kind != TYPE_ARRAY || index.kind != POWERVR_IR_OPERAND_CONSTANT || literal >= type->count)
				return ENOTSUP;
			pointer.offset = pointer.offset * type->count + literal;
			current = type->type;
			continue;
		}

		/* An element of an array, a vector or a matrix. */
		if (type->type_kind != TYPE_ARRAY &&
		    type->type_kind != TYPE_RUNTIME_ARRAY &&
		    type->type_kind != TYPE_VECTOR &&
		    type->type_kind != TYPE_MATRIX)
			return ENOTSUP;
		if (memory != 0) {
			stride = type->stride;
			if (type->type_kind == TYPE_VECTOR)
				stride = 4;
			if (stride == 0)
				stride = shader_spirv_byte_size(reader, type->type);
		} else {
			stride = shader_spirv_scalar_count(reader, type->type);
		}

		if (index.kind == POWERVR_IR_OPERAND_CONSTANT) {
			pointer.offset += literal * stride;
		} else if (memory != 0 && (pointer.flags & FLAG_HAS_INDEX) == 0 && pointer.storage != STORAGE_PUSH_CONSTANT) {
			/* One run-time index into memory. */
			pointer.index = shader_spirv_plain(reader, index);
			pointer.index_stride = stride;
			pointer.flags |= FLAG_HAS_INDEX;
		} else if (memory != 0 && pointer.storage != STORAGE_PUSH_CONSTANT) {
			/* A further one: both as one byte offset. */
			scale = shader_spirv_constant_operand(pointer.index_stride);
			scaled = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &pointer.index, &scale, NULL);
			index = shader_spirv_plain(reader, index);
			scale = shader_spirv_constant_operand(stride);
			pointer.index = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &index, &scale, NULL);
			pointer.index = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &pointer.index, &scaled, NULL);
			pointer.index_stride = 1;
		} else if (memory == 0 && (pointer.flags & FLAG_HAS_INDEX) == 0 && type->type_kind != TYPE_RUNTIME_ARRAY && type->count != 0 &&
			   type->count <= COMPOSITE_MAX * 2U) {
			/*
			 * One run-time index into a variable in registers: its
			 * element count (in words) and stride kept, each read
			 * and write a select over the elements.
			 */
			pointer.index = shader_spirv_plain(reader, index);
			pointer.index_stride = stride;
			pointer.words = type->count;
			pointer.flags |= FLAG_HAS_INDEX;
		} else {
			/* A second run-time index into registers is outside the subset. */
			return ENOTSUP;
		}
		current = type->type;
	}

	/* The result is the pointer to the last type reached. */
	pointer.type = current;
	reader->ids[words[2]] = pointer;
	reader->ids[words[2]].kind = ID_POINTER;
	return reader->error;
}

/* Reads the words a pointer names into a new result. */
static int
reader_load(
	struct shader_spirv_reader *reader,
	uint32_t type,
	uint32_t result,
	uint32_t pointer)
{
	struct powervr_ir_operand address[3];
	struct powervr_ir_operand offset;
	struct powervr_ir_operand word;
	struct powervr_ir_operand *operands;
	struct powervr_ir_operand test;
	struct shader_spirv_id *source;
	uint32_t count;
	uint32_t index;
	uint32_t element;
	uint32_t request;
	uint32_t flags;
	int error;

	/* The pointer and the words to read. */
	if (pointer >= reader->bound || result >= reader->bound || type >= reader->bound)
		return EINVAL;
	source = &reader->ids[pointer];
	if (source->kind != ID_POINTER)
		return EINVAL;

	/* A sampled image: its binding, element and type are all a load gives. */
	if (source->storage == STORAGE_UNIFORM_CONSTANT) {
		reader->ids[result].kind = ID_SAMPLED_IMAGE;
		reader->ids[result].set = reader->ids[source->variable].set;
		reader->ids[result].binding = reader->ids[source->variable].binding;
		reader->ids[result].offset = source->offset;
		reader->ids[result].type = type;
		return 0;
	}

	/* The result's words. */
	count = shader_spirv_scalar_count(reader, type);
	if (count == 0 || count > COMPOSITE_MAX)
		return ENOTSUP;
	error = shader_spirv_result_begin(reader, result, type, count, &operands);
	if (error != 0)
		return error;

	/* Fills them by where the pointer points. */
	switch (source->storage) {
	case STORAGE_INPUT:
	case STORAGE_OUTPUT:
	case STORAGE_PRIVATE:
	case STORAGE_FUNCTION:
		/* Copies of the variable's values, so later stores do not change the result. */
		for (index = 0; index < count; index++) {
			word = shader_spirv_value_operand(reader->ids[source->variable].first + source->offset + index, 0);
			operands[index] = shader_spirv_emit_value(reader, POWERVR_IR_MOVE, &word, NULL, NULL);
		}

		/* At a run-time index, the element's words where the index names it. */
		if ((source->flags & FLAG_HAS_INDEX) != 0) {
			for (element = 1; element < source->words; element++) {
				test = register_index_test(reader, source, element);
				for (index = 0; index < count; index++) {
					word = shader_spirv_value_operand(reader->ids[source->variable].first + source->offset + element * source->index_stride + index, 0);
					operands[index] = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &test, &word, &operands[index]);
				}
			}
		}
		break;
	case STORAGE_PUSH_CONSTANT:
		/* Words of the push constant request. */
		if ((source->offset & 3U) != 0)
			return ENOTSUP;
		if (reader->push_request >= POWERVR_SHADER_REQUESTS) {
			error = shader_spirv_request_add(reader, POWERVR_SHADER_REQUEST_PUSH_CONSTANTS, 0, 0, 0, 0, &request);
			if (error != 0)
				return error;
			reader->push_request = request;
		}
		for (index = 0; index < count; index++) {
			operands[index] = shader_spirv_shared_word(reader, reader->push_request, source->offset / 4U + index);
			if (source->offset / 4U + index + 1U > reader->push_words)
				reader->push_words = source->offset / 4U + index + 1U;
		}
		break;
	case STORAGE_UNIFORM:
	case STORAGE_STORAGE_BUFFER:
	case STORAGE_WORKGROUP:
		/* A storage image's shape is words of its descriptor, not memory. */
		if (source->storage == STORAGE_UNIFORM && (reader->ids[source->variable].binding & DRV_GPU_SPIRV_IMAGE_INFO_BINDING) != 0U) {
			error = reader_shape_load(reader, source, count, operands);
			if (error != 0)
				return error;
			break;
		}

		/* Loads from the buffer, word by word; workgroup memory, which other tasks write, around the caches. */
		error = reader_memory_address(reader, source, address);
		if (error != 0)
			return error;
		flags = 0;
		if (source->storage == STORAGE_WORKGROUP)
			flags = POWERVR_IR_BYPASS;
		for (index = 0; index < count; index++) {
			offset = address[2];
			if (index != 0) {
				word = shader_spirv_constant_operand(index * 4U);
				offset = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &address[2], &word, NULL);
			}
			operands[index] = memory_load(reader, address, &offset, flags);
		}
		break;
	default:
		return ENOTSUP;
	}

	/* Reports what the load came to. */
	return reader->error;
}

/* Writes an object's words where a pointer names. */
static int
reader_store(
	struct shader_spirv_reader *reader,
	uint32_t pointer,
	uint32_t object)
{
	struct powervr_ir_operand *operands;
	uint32_t count;
	int error;

	/* The object's words, written. */
	error = shader_spirv_id_operands(reader, object, &operands, &count);
	if (error != 0)
		return error;
	error = store_words(reader, pointer, operands, count);
	if (error != 0)
		return error;

	/* Succeeded: the store is read. */
	return 0;
}

/* Writes words where a pointer names. */
static int
store_words(
	struct shader_spirv_reader *reader,
	uint32_t pointer,
	const struct powervr_ir_operand *operands,
	uint32_t count)
{
	struct powervr_ir_operand address[3];
	struct powervr_ir_operand offset;
	struct powervr_ir_operand word;
	struct powervr_ir_operand old;
	struct powervr_ir_operand test;
	struct shader_spirv_id *target;
	uint32_t index;
	uint32_t element;
	uint32_t value;
	int error;

	/* The pointer. */
	if (pointer >= reader->bound)
		return EINVAL;
	target = &reader->ids[pointer];
	if (target->kind != ID_POINTER)
		return EINVAL;

	/* Writes them by where the pointer points. */
	switch (target->storage) {
	case STORAGE_OUTPUT:
	case STORAGE_PRIVATE:
	case STORAGE_FUNCTION:
		/* Moves into the variable's values. */
		if ((target->flags & FLAG_HAS_INDEX) == 0) {
			for (index = 0; index < count; index++)
				shader_spirv_emit(reader, POWERVR_IR_MOVE, reader->ids[target->variable].first + target->offset + index, &operands[index], NULL, NULL, 0, 0);
			break;
		}

		/* At a run-time index: each element's words, the new ones where the index names it. */
		for (element = 0; element < target->words; element++) {
			test = register_index_test(reader, target, element);
			for (index = 0; index < count; index++) {
				value = reader->ids[target->variable].first + target->offset + element * target->index_stride + index;
				old = shader_spirv_value_operand(value, 0);
				word = shader_spirv_plain(reader, operands[index]);
				shader_spirv_emit(reader, POWERVR_IR_SELECT, value, &test, &word, &old, 0, 0);
			}
		}
		break;
	case STORAGE_UNIFORM:
	case STORAGE_STORAGE_BUFFER:
	case STORAGE_WORKGROUP:
		/* Stores to the buffer or the workgroup memory, word by word. */
		error = reader_memory_address(reader, target, address);
		if (error != 0)
			return error;
		reader->program->stores = 1;
		for (index = 0; index < count; index++) {
			offset = address[2];
			if (index != 0) {
				word = shader_spirv_constant_operand(index * 4U);
				offset = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &address[2], &word, NULL);
			}
			memory_store(reader, address, &offset, operands[index]);
		}
		break;
	default:
		return ENOTSUP;
	}

	/* Reports what the store came to. */
	return reader->error;
}

/* Whether a register variable's run-time index names an element (a Boolean word). */
static struct powervr_ir_operand
register_index_test(
	struct shader_spirv_reader *reader,
	const struct shader_spirv_id *pointer,
	uint32_t element)
{
	struct powervr_ir_operand k;

	k = shader_spirv_constant_operand(element);
	return shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_EQUAL, &pointer->index, &k, NULL);
}

/*
 * Works out a buffer pointer's address: the binding's address words
 * from its descriptor request, and the byte offset as an operand.
 */
static int
reader_memory_address(
	struct shader_spirv_reader *reader,
	const struct shader_spirv_id *pointer,
	struct powervr_ir_operand address[3])
{
	struct powervr_ir_operand stride;
	struct powervr_ir_operand scaled;
	struct powervr_ir_operand offset;
	const struct shader_spirv_id *variable;
	uint32_t request;
	int error;

	/* The binding's address words, asked for once per binding; workgroup memory's the dispatch's. */
	variable = &reader->ids[pointer->variable];
	if (variable->storage == STORAGE_WORKGROUP) {
		error = shader_spirv_request_add(reader, POWERVR_SHADER_REQUEST_SHARED_MEMORY, 0, 0, 0, 2, &request);
	} else {
		error = shader_spirv_request_element(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_BUFFER_ADDRESS, variable->set, variable->binding, pointer->element, 2, &request);
	}
	if (error != 0)
		return error;
	address[0] = shader_spirv_shared_word(reader, request, 0);
	address[1] = shader_spirv_shared_word(reader, request, 1);

	/* The byte offset: the constant part (in the workgroup's slice), plus the index times its stride. */
	offset = shader_spirv_constant_operand(pointer->offset);
	if (variable->storage == STORAGE_WORKGROUP)
		offset = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &reader->shared_slice, &offset, NULL);
	if ((pointer->flags & FLAG_HAS_INDEX) != 0) {
		stride = shader_spirv_constant_operand(pointer->index_stride);
		scaled = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &pointer->index, &stride, NULL);
		offset = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &scaled, &offset, NULL);
	}
	address[2] = offset;

	/* Reports what the address came to. */
	return reader->error;
}

/*
 * Reads words of a storage image's shape (the uniform block its lowering
 * reads, at the binding with DRV_GPU_SPIRV_IMAGE_INFO_BINDING set): the
 * descriptor's shape words in shared registers, the last three zero.  A
 * run-time index into the shape is refused.
 */
static int
reader_shape_load(
	struct shader_spirv_reader *reader,
	const struct shader_spirv_id *pointer,
	uint32_t count,
	struct powervr_ir_operand *operands)
{
	const struct shader_spirv_id *variable;
	uint32_t request;
	uint32_t word;
	uint32_t index;
	int error;

	/* Refuses a word chosen at run time, and one past the shape. */
	if ((pointer->flags & FLAG_HAS_INDEX) != 0 || (pointer->offset & 3U) != 0)
		return ENOTSUP;

	/* The descriptor's shape words, asked for once for each element. */
	variable = &reader->ids[pointer->variable];
	error = shader_spirv_request_element(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_IMAGE_SHAPE, variable->set, variable->binding, pointer->element, POWERVR_SHADER_SHAPE_WORDS, &request);
	if (error != 0)
		return error;

	/* Each word read: a shared word, or zero past the five. */
	for (index = 0; index < count; index++) {
		word = pointer->offset / 4U + index;
		operands[index] = shader_spirv_constant_operand(0);
		if (word < POWERVR_SHADER_SHAPE_WORDS)
			operands[index] = shader_spirv_shared_word(reader, request, word);
	}

	/* Reports what the load came to. */
	return reader->error;
}

/*
 * Reads the composite-shaping instructions, which only rearrange words
 * and write no code.
 */
static int
reader_composite(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length,
	uint32_t *handled)
{
	struct powervr_ir_operand gathered[COMPOSITE_MAX * 2U];
	struct powervr_ir_operand *operands;
	struct powervr_ir_operand *part;
	struct powervr_ir_operand *second;
	uint32_t opcode;
	uint32_t count;
	uint32_t parts;
	uint32_t second_count;
	uint32_t at;
	uint32_t index;
	uint32_t offset;
	uint32_t member_type;
	uint32_t total;
	int error;

	/* Only the shaping opcodes are this function's. */
	opcode = words[0] & 0xffffU;
	*handled = 1;
	if (opcode != OP_COMPOSITE_CONSTRUCT &&
	    opcode != OP_COMPOSITE_EXTRACT &&
	    opcode != OP_COMPOSITE_INSERT &&
	    opcode != OP_VECTOR_SHUFFLE &&
	    opcode != OP_COPY_OBJECT &&
	    opcode != OP_BITCAST) {
		*handled = 0;
		return 0;
	}
	if (length < 4 || words[1] >= reader->bound || words[2] >= reader->bound)
		return EINVAL;

	/* Gathers the words the result is made from. */
	total = 0;
	if (opcode == OP_COMPOSITE_CONSTRUCT) {
		for (at = 3; at < length; at++) {
			error = shader_spirv_id_operands(reader, words[at], &part, &parts);
			if (error != 0)
				return error;
			for (index = 0; index < parts && total < COMPOSITE_MAX * 2U; index++)
				gathered[total++] = part[index];
		}
	} else if (opcode == OP_COMPOSITE_EXTRACT) {
		error = shader_spirv_id_operands(reader, words[3], &part, &parts);
		if (error != 0)
			return error;
		error = shader_spirv_composite_offset(reader, reader->ids[words[3]].type, &words[4], length - 4, &offset, &member_type);
		if (error != 0)
			return error;
		count = shader_spirv_scalar_count(reader, words[1]);
		if (offset + count > parts)
			return EINVAL;
		for (index = 0; index < count; index++)
			gathered[total++] = part[offset + index];
	} else if (opcode == OP_COMPOSITE_INSERT) {
		if (length < 5)
			return EINVAL;
		error = shader_spirv_id_operands(reader, words[4], &part, &parts);
		if (error != 0)
			return error;
		for (index = 0; index < parts && total < COMPOSITE_MAX * 2U; index++)
			gathered[total++] = part[index];
		error = shader_spirv_composite_offset(reader, reader->ids[words[4]].type, &words[5], length - 5, &offset, &member_type);
		if (error != 0)
			return error;
		error = shader_spirv_id_operands(reader, words[3], &part, &parts);
		if (error != 0)
			return error;
		if (offset + parts > total)
			return EINVAL;
		for (index = 0; index < parts; index++)
			gathered[offset + index] = part[index];
	} else if (opcode == OP_VECTOR_SHUFFLE) {
		if (length < 5)
			return EINVAL;
		error = shader_spirv_id_operands(reader, words[3], &part, &parts);
		if (error != 0)
			return error;
		error = shader_spirv_id_operands(reader, words[4], &second, &second_count);
		if (error != 0)
			return error;
		for (at = 5; at < length && total < COMPOSITE_MAX * 2U; at++) {
			if (words[at] == 0xffffffffU) {
				gathered[total++] = shader_spirv_constant_operand(0);
			} else if (words[at] < parts) {
				gathered[total++] = part[words[at]];
			} else if (words[at] - parts < second_count) {
				gathered[total++] = second[words[at] - parts];
			} else {
				return EINVAL;
			}
		}
	} else {
		/* A copy or a bitcast: the same words, a bitcast without float modifiers. */
		error = shader_spirv_id_operands(reader, words[3], &part, &parts);
		if (error != 0)
			return error;
		for (index = 0; index < parts && total < COMPOSITE_MAX * 2U; index++) {
			gathered[total] = part[index];
			if (opcode == OP_BITCAST)
				gathered[total] = shader_spirv_plain(reader, part[index]);
			total++;
		}
	}

	/* The result holds exactly its type's words. */
	count = shader_spirv_scalar_count(reader, words[1]);
	if (count != total || count == 0 || count > COMPOSITE_MAX)
		return ENOTSUP;
	error = shader_spirv_result_begin(reader, words[2], words[1], count, &operands);
	if (error != 0)
		return error;
	for (index = 0; index < count; index++)
		operands[index] = gathered[index];

	/* Reports what the shaping came to. */
	return reader->error;
}

/* Reads one arithmetic, comparison, logic or conversion instruction, word by word. */
static int
reader_arithmetic(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length,
	uint32_t *handled)
{
	struct powervr_ir_operand *result;
	struct powervr_ir_operand *a;
	struct powervr_ir_operand *b;
	struct powervr_ir_operand *c;
	struct powervr_ir_operand x;
	struct powervr_ir_operand y;
	struct powervr_ir_operand z;
	struct powervr_ir_operand sum;
	uint32_t opcode;
	uint32_t count;
	uint32_t a_count;
	uint32_t b_count;
	uint32_t c_count;
	uint32_t index;
	uint32_t row;
	uint32_t column;
	uint32_t columns;
	uint32_t ir;
	uint32_t swap;
	uint32_t invert;
	uint32_t divide;
	int error;

	/* The result and the sources every form has. */
	opcode = words[0] & 0xffffU;
	*handled = 1;
	if (length < 4 || words[1] >= reader->bound || words[2] >= reader->bound) {
		*handled = 0;
		return 0;
	}
	count = shader_spirv_scalar_count(reader, words[1]);
	if (count == 0 || count > COMPOSITE_MAX) {
		*handled = 0;
		return 0;
	}
	error = shader_spirv_id_operands(reader, words[3], &a, &a_count);
	if (error != 0) {
		*handled = 0;
		return 0;
	}
	b = NULL;
	b_count = 0;
	if (length > 4) {
		error = shader_spirv_id_operands(reader, words[4], &b, &b_count);
		if (error != 0)
			b = NULL;
	}
	c = NULL;
	c_count = 0;
	if (length > 5) {
		error = shader_spirv_id_operands(reader, words[5], &c, &c_count);
		if (error != 0)
			c = NULL;
	}

	/* The forms that are not word by word. */
	if (opcode == OP_DOT) {
		if (b == NULL || a_count != b_count || count != 1)
			return EINVAL;
		error = shader_spirv_result_begin(reader, words[2], words[1], 1, &result);
		if (error != 0)
			return error;
		sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &a[0], &b[0], NULL);
		for (index = 1; index < a_count; index++)
			sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &a[index], &b[index], &sum);
		result[0] = sum;
		return reader->error;
	}
	if (opcode == OP_MATRIX_TIMES_VECTOR) {
		/* Each row: the sum of the columns' words in it times the vector's. */
		if (b == NULL || b_count == 0 || a_count != b_count * count)
			return ENOTSUP;
		error = shader_spirv_result_begin(reader, words[2], words[1], count, &result);
		if (error != 0)
			return error;
		columns = b_count;
		for (row = 0; row < count; row++) {
			sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &a[row], &b[0], NULL);
			for (column = 1; column < columns; column++)
				sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &a[column * count + row], &b[column], &sum);
			result[row] = sum;
		}
		return reader->error;
	}

	/* The other products of matrices and vectors, and the transpose. */
	if (opcode == OP_VECTOR_TIMES_MATRIX || opcode == OP_MATRIX_TIMES_MATRIX || opcode == OP_OUTER_PRODUCT || opcode == OP_TRANSPOSE) {
		error = reader_matrix(reader, words, a, a_count, b, b_count, count);
		if (error != 0)
			return error;

		/* Succeeded: the product is read. */
		return 0;
	}

	/* Whether any or all of a vector's Booleans hold. */
	if (opcode == OP_ANY || opcode == OP_ALL) {
		if (count != 1)
			return EINVAL;
		error = shader_spirv_result_begin(reader, words[2], words[1], 1, &result);
		if (error != 0)
			return error;
		sum = shader_spirv_plain(reader, a[0]);
		for (index = 1; index < a_count; index++) {
			x = shader_spirv_plain(reader, a[index]);
			sum = shader_spirv_emit_value(reader, opcode == OP_ANY ? POWERVR_IR_BIT_OR : POWERVR_IR_BIT_AND, &sum, &x, NULL);
		}
		result[0] = sum;
		return reader->error;
	}

	/* A word of a vector, or a vector with one replaced, at a run-time index. */
	if (opcode == OP_VECTOR_EXTRACT_DYNAMIC || opcode == OP_VECTOR_INSERT_DYNAMIC) {
		error = reader_dynamic_component(reader, words, length, a, a_count, b, b_count, count);
		if (error != 0)
			return error;

		/* Succeeded: the access is read. */
		return 0;
	}

	/* The integer divisions, word by word through their lowering. */
	divide = DIVIDE_SIGNED_MODULO + 1U;
	if (opcode == OP_U_DIV)
		divide = DIVIDE_UNSIGNED;
	else if (opcode == OP_U_MOD)
		divide = DIVIDE_UNSIGNED_REMAINDER;
	else if (opcode == OP_S_DIV)
		divide = DIVIDE_SIGNED;
	else if (opcode == OP_S_REM)
		divide = DIVIDE_SIGNED_REMAINDER;
	else if (opcode == OP_S_MOD)
		divide = DIVIDE_SIGNED_MODULO;
	if (divide <= DIVIDE_SIGNED_MODULO) {
		if (b == NULL || b_count == 0)
			return EINVAL;
		error = shader_spirv_result_begin(reader, words[2], words[1], count, &result);
		if (error != 0)
			return error;
		for (index = 0; index < count; index++) {
			if ((index >= a_count && a_count != 1) || (index >= b_count && b_count != 1))
				return EINVAL;
			x = shader_spirv_plain(reader, a[a_count == 1 ? 0 : index]);
			y = shader_spirv_plain(reader, b[b_count == 1 ? 0 : index]);
			result[index] = shader_spirv_divide(reader, x, y, divide);
		}
		return reader->error;
	}

	/* The word-by-word forms of several operations: derivatives, NaN and infinity tests, the float remainders. */
	if ((opcode >= OP_DPDX && opcode <= OP_FWIDTH_COARSE) || opcode == OP_IS_NAN || opcode == OP_IS_INF || opcode == OP_F_MOD || opcode == OP_F_REM) {
		error = shader_spirv_result_begin(reader, words[2], words[1], count, &result);
		if (error != 0)
			return error;
		for (index = 0; index < count; index++) {
			if ((index >= a_count && a_count != 1) || ((opcode == OP_F_MOD || opcode == OP_F_REM) && (b == NULL || (index >= b_count && b_count != 1))))
				return EINVAL;
			x = a[a_count == 1 ? 0 : index];
			y = shader_spirv_constant_operand(0);
			if (b != NULL)
				y = b[b_count == 1 ? 0 : index];
			result[index] = word_operation(reader, opcode, x, y);
		}
		return reader->error;
	}

	/* Maps the opcode to the operation, with swapped or inverted forms. */
	swap = 0;
	invert = 0;
	ir = POWERVR_IR_OPCODE_COUNT;
	switch (opcode) {
	case OP_F_ADD:
		ir = POWERVR_IR_FLOAT_ADD;
		break;
	case OP_F_SUB:
		ir = POWERVR_IR_FLOAT_ADD;
		break;
	case OP_F_MUL:
		ir = POWERVR_IR_FLOAT_MULTIPLY;
		break;
	case OP_F_DIV:
		ir = POWERVR_IR_FLOAT_MULTIPLY;
		break;
	case OP_VECTOR_TIMES_SCALAR:
		ir = POWERVR_IR_FLOAT_MULTIPLY;
		break;
	case OP_MATRIX_TIMES_SCALAR:
		ir = POWERVR_IR_FLOAT_MULTIPLY;
		break;
	case OP_F_NEGATE:
		ir = POWERVR_IR_MOVE;
		break;
	case OP_I_ADD:
		ir = POWERVR_IR_INTEGER_ADD;
		break;
	case OP_I_SUB:
		ir = POWERVR_IR_INTEGER_ADD;
		break;
	case OP_I_MUL:
		ir = POWERVR_IR_INTEGER_MULTIPLY;
		break;
	case OP_S_NEGATE:
		ir = POWERVR_IR_INTEGER_MULTIPLY;
		break;
	case OP_BITWISE_AND:
		ir = POWERVR_IR_BIT_AND;
		break;
	case OP_BITWISE_OR:
		ir = POWERVR_IR_BIT_OR;
		break;
	case OP_BITWISE_XOR:
		ir = POWERVR_IR_BIT_XOR;
		break;
	case OP_NOT:
		ir = POWERVR_IR_BIT_XOR;
		break;
	case OP_LOGICAL_AND:
		ir = POWERVR_IR_BIT_AND;
		break;
	case OP_LOGICAL_OR:
		ir = POWERVR_IR_BIT_OR;
		break;
	case OP_LOGICAL_NOT:
		ir = POWERVR_IR_BIT_XOR;
		break;
	case OP_LOGICAL_NOT_EQUAL:
		ir = POWERVR_IR_BIT_XOR;
		break;
	case OP_LOGICAL_EQUAL:
		ir = POWERVR_IR_BIT_XOR;
		invert = 1;
		break;
	case OP_SHIFT_LEFT_LOGICAL:
		ir = POWERVR_IR_SHIFT_LEFT;
		break;
	case OP_SHIFT_RIGHT_LOGICAL:
		ir = POWERVR_IR_SHIFT_RIGHT;
		break;
	case OP_SHIFT_RIGHT_ARITHMETIC:
		ir = POWERVR_IR_SHIFT_RIGHT_SIGNED;
		break;
	case OP_I_EQUAL:
		ir = POWERVR_IR_INTEGER_EQUAL;
		break;
	case OP_I_NOT_EQUAL:
		ir = POWERVR_IR_INTEGER_NOT_EQUAL;
		break;
	case OP_U_LESS_THAN:
		ir = POWERVR_IR_UNSIGNED_LESS;
		break;
	case OP_U_GREATER_THAN_EQUAL:
		ir = POWERVR_IR_UNSIGNED_GREATER_EQUAL;
		break;
	case OP_U_GREATER_THAN:
		ir = POWERVR_IR_UNSIGNED_LESS;
		swap = 1;
		break;
	case OP_U_LESS_THAN_EQUAL:
		ir = POWERVR_IR_UNSIGNED_GREATER_EQUAL;
		swap = 1;
		break;
	case OP_S_LESS_THAN:
		ir = POWERVR_IR_SIGNED_LESS;
		break;
	case OP_S_GREATER_THAN_EQUAL:
		ir = POWERVR_IR_SIGNED_GREATER_EQUAL;
		break;
	case OP_S_GREATER_THAN:
		ir = POWERVR_IR_SIGNED_LESS;
		swap = 1;
		break;
	case OP_S_LESS_THAN_EQUAL:
		ir = POWERVR_IR_SIGNED_GREATER_EQUAL;
		swap = 1;
		break;
	case OP_SELECT:
		ir = POWERVR_IR_SELECT;
		break;
	case OP_CONVERT_F_TO_S:
		ir = POWERVR_IR_FLOAT_TO_SIGNED;
		break;
	case OP_CONVERT_F_TO_U:
		ir = POWERVR_IR_FLOAT_TO_UNSIGNED;
		break;
	case OP_CONVERT_S_TO_F:
		ir = POWERVR_IR_SIGNED_TO_FLOAT;
		break;
	case OP_CONVERT_U_TO_F:
		ir = POWERVR_IR_UNSIGNED_TO_FLOAT;
		break;
	default:
		/*
		 * The float comparisons, ordered and unordered alike (the
		 * subset does not tell NaNs apart): equal 180-181, not equal
		 * 182-183, less 184-185, greater 186-187, less or equal
		 * 188-189, greater or equal 190-191.
		 */
		if (opcode >= OP_F_ORD_EQUAL && opcode <= OP_F_UNORD_GREATER_THAN_EQUAL) {
			switch ((opcode - OP_F_ORD_EQUAL) / 2U) {
			case 0:
				ir = POWERVR_IR_FLOAT_EQUAL;
				break;
			case 1:
				ir = POWERVR_IR_FLOAT_NOT_EQUAL;
				break;
			case 2:
				ir = POWERVR_IR_FLOAT_LESS;
				break;
			case 3:
				ir = POWERVR_IR_FLOAT_LESS;
				swap = 1;
				break;
			case 4:
				ir = POWERVR_IR_FLOAT_GREATER_EQUAL;
				swap = 1;
				break;
			default:
				ir = POWERVR_IR_FLOAT_GREATER_EQUAL;
				break;
			}
		}
		break;
	}
	if (ir == POWERVR_IR_OPCODE_COUNT) {
		*handled = 0;
		return 0;
	}

	/* The result's words. */
	error = shader_spirv_result_begin(reader, words[2], words[1], count, &result);
	if (error != 0)
		return error;

	/* Word by word, a scalar source spread over a vector result. */
	for (index = 0; index < count; index++) {
		if (index >= a_count && a_count != 1)
			return EINVAL;
		x = a[a_count == 1 ? 0 : index];
		if (b != NULL && b_count != 0) {
			y = b[b_count == 1 ? 0 : index];
		} else {
			y = shader_spirv_constant_operand(0);
		}
		if (c != NULL && c_count != 0) {
			z = c[c_count == 1 ? 0 : index];
		} else {
			z = shader_spirv_constant_operand(0);
		}

		/* The forms that adjust their sources. */
		switch (opcode) {
		case OP_F_SUB:
			y = shader_spirv_float_negate(y);
			break;
		case OP_F_DIV:
			y = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &y, NULL, NULL);
			break;
		case OP_F_NEGATE:
			result[index] = shader_spirv_float_negate(x);
			continue;
		case OP_I_SUB:
			x = shader_spirv_plain(reader, x);
			y = shader_spirv_plain(reader, y);
			z = shader_spirv_constant_operand(TRUE_WORD);
			y = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &y, &z, NULL);
			break;
		case OP_S_NEGATE:
			y = shader_spirv_constant_operand(TRUE_WORD);
			break;
		case OP_NOT:
		case OP_LOGICAL_NOT:
			y = shader_spirv_constant_operand(TRUE_WORD);
			break;
		default:
			break;
		}

		/* Integer and bit forms read plain words. */
		if (ir != POWERVR_IR_FLOAT_ADD &&
		    ir != POWERVR_IR_FLOAT_MULTIPLY &&
		    ir != POWERVR_IR_FLOAT_LESS &&
		    ir != POWERVR_IR_FLOAT_GREATER_EQUAL &&
		    ir != POWERVR_IR_FLOAT_EQUAL &&
		    ir != POWERVR_IR_FLOAT_NOT_EQUAL &&
		    ir != POWERVR_IR_FLOAT_TO_SIGNED &&
		    ir != POWERVR_IR_FLOAT_TO_UNSIGNED) {
			x = shader_spirv_plain(reader, x);
			y = shader_spirv_plain(reader, y);
		}
		if (ir == POWERVR_IR_SELECT) {
			x = shader_spirv_plain(reader, x);
			y = b[b_count == 1 ? 0 : index];
			z = c[c_count == 1 ? 0 : index];
			y = shader_spirv_plain(reader, y);
			z = shader_spirv_plain(reader, z);
		}

		/* The operation itself. */
		if (swap != 0) {
			result[index] = shader_spirv_emit_value(reader, ir, &y, &x, NULL);
		} else {
			result[index] = shader_spirv_emit_value(reader, ir, &x, &y, &z);
		}
		if (invert != 0) {
			y = shader_spirv_constant_operand(TRUE_WORD);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_BIT_XOR, &result[index], &y, NULL);
		}
	}

	/* Reports what the arithmetic came to. */
	return reader->error;
}

/*
 * Works out one word of the operations reader_arithmetic takes word by
 * word through several instructions: a derivative (across the 2x2 block,
 * or the sum of both magnitudes for a width), a NaN or infinity test on
 * the bits, and the float remainder of a division rounded down (OpFMod)
 * or toward zero (OpFRem).
 */
static struct powervr_ir_operand
word_operation(
	struct shader_spirv_reader *reader,
	uint32_t opcode,
	struct powervr_ir_operand x,
	struct powervr_ir_operand y)
{
	struct powervr_ir_operand t;
	struct powervr_ir_operand u;
	struct powervr_ir_operand k;
	uint32_t kind;
	uint32_t across;
	uint32_t down;

	/* The tests and the remainders; the derivatives after them (the back end refuses them outside a fragment shader). */
	switch (opcode) {
	case OP_IS_NAN:
	case OP_IS_INF:
		/* The magnitude's bits against the infinity's. */
		x = shader_spirv_plain(reader, x);
		k = shader_spirv_constant_operand(~FLOAT_SIGN);
		t = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &x, &k, NULL);
		k = shader_spirv_constant_operand(FLOAT_INFINITY);
		if (opcode == OP_IS_NAN)
			return shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_LESS, &k, &t, NULL);
		return shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_EQUAL, &t, &k, NULL);
	case OP_F_MOD:
	case OP_F_REM:
		/* x - y * floor(x / y), or with the quotient truncated. */
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &y, NULL, NULL);
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &x, &t, NULL);
		if (opcode == OP_F_MOD)
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_FLOOR, &t, NULL, NULL);
		else
			t = float_truncate(reader, t);
		t = shader_spirv_float_negate(t);
		return shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &t, &y, &x);
	default:
		break;
	}

	/*
	 * The derivatives: dPdx, dPdy, fwidth (207 to 209), the fine ones
	 * (210 to 212) and the coarse ones (213 to 215), the plain ones the
	 * core's coarse.
	 */
	kind = (opcode - OP_DPDX) % 3U;
	across = POWERVR_IR_DERIVATIVE_X;
	down = POWERVR_IR_DERIVATIVE_Y;
	if ((opcode - OP_DPDX) / 3U == 1U) {
		across = POWERVR_IR_DERIVATIVE_X_FINE;
		down = POWERVR_IR_DERIVATIVE_Y_FINE;
	}
	if (kind == 0)
		return derivative(reader, x, across);
	if (kind == 1U)
		return derivative(reader, x, down);

	/* A width: the sum of both derivatives' magnitudes. */
	t = derivative(reader, x, across);
	u = derivative(reader, x, down);
	t = shader_spirv_float_absolute(t);
	u = shader_spirv_float_absolute(u);
	return shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &t, &u, NULL);
}

/* A derivative of a word across the fragment's 2x2 block (POWERVR_IR_DERIVATIVE_*). */
static struct powervr_ir_operand
derivative(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand x,
	uint32_t which)
{
	uint32_t value;

	/* A new value the derivative writes. */
	value = shader_spirv_new_value(reader, 1);
	shader_spirv_emit(reader, POWERVR_IR_FLOAT_DERIVATIVE, value, &x, NULL, NULL, which, 0);

	/* Succeeded: the derivative as an operand. */
	return shader_spirv_value_operand(value, 0);
}

/* Reads one GLSL.std.450 instruction, lowered to the intermediate form's operations. */
static int
reader_extended(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length)
{
	struct powervr_ir_operand *result;
	struct powervr_ir_operand *a;
	struct powervr_ir_operand *b;
	struct powervr_ir_operand *c;
	struct powervr_ir_operand x;
	struct powervr_ir_operand y;
	struct powervr_ir_operand z;
	struct powervr_ir_operand t;
	struct powervr_ir_operand k;
	struct powervr_ir_operand sum;
	struct powervr_ir_operand mix_weight;
	struct powervr_ir_operand mix_rest;
	uint32_t instruction;
	uint32_t count;
	uint32_t a_count;
	uint32_t b_count;
	uint32_t c_count;
	uint32_t index;
	int kept_mix;
	int error;

	/* Only GLSL.std.450 is known. */
	if (length < 6 || words[3] != reader->glsl || reader->glsl == 0)
		return ENOTSUP;
	instruction = words[4];
	count = shader_spirv_scalar_count(reader, words[1]);
	if (count == 0 || count > COMPOSITE_MAX)
		return ENOTSUP;

	/* The sources; a modf's second is a pointer for the whole part. */
	error = shader_spirv_id_operands(reader, words[5], &a, &a_count);
	if (error != 0)
		return error;
	b = NULL;
	c = NULL;
	b_count = 0;
	c_count = 0;
	if (instruction == GLSL_MODF || instruction == GLSL_MODF_STRUCT) {
		error = reader_modf(reader, words, length, a, a_count, count);
		if (error != 0)
			return error;

		/* Succeeded: the modf is read. */
		return 0;
	}
	if (length > 6) {
		error = shader_spirv_id_operands(reader, words[6], &b, &b_count);
		if (error != 0)
			return error;
	}
	if (length > 7) {
		error = shader_spirv_id_operands(reader, words[7], &c, &c_count);
		if (error != 0)
			return error;
	}
	error = shader_spirv_result_begin(reader, words[2], words[1], count, &result);
	if (error != 0)
		return error;

	/* The reductions over a vector. */
	if (instruction == GLSL_LENGTH || instruction == GLSL_DISTANCE) {
		/* The root of the sum of squares (of the difference, for a distance). */
		sum = shader_spirv_constant_operand(0);
		for (index = 0; index < a_count; index++) {
			x = a[index];
			if (instruction == GLSL_DISTANCE) {
				if (b == NULL || b_count != a_count)
					return EINVAL;
				y = shader_spirv_float_negate(b[index]);
				x = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &x, &y, NULL);
			}
			sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &x, &x, &sum);
		}
		x = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL_ROOT, &sum, NULL, NULL);
		result[0] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &x, NULL, NULL);
		return reader->error;
	}
	if (instruction == GLSL_CROSS || instruction == GLSL_REFLECT || instruction == GLSL_REFRACT || instruction == GLSL_FACE_FORWARD ||
	    instruction == GLSL_DETERMINANT || instruction == GLSL_MATRIX_INVERSE) {
		error = reader_geometry(reader, instruction, result, count, a, a_count, b, b_count, c, c_count);
		if (error != 0)
			return error;

		/* Succeeded: the function is read. */
		return 0;
	}
	if (instruction >= GLSL_PACK_SNORM_4X8 && instruction <= GLSL_UNPACK_UNORM_4X8) {
		error = reader_packing(reader, instruction, result, count, a, a_count);
		if (error != 0)
			return error;

		/* Succeeded: the pack or unpack is read. */
		return 0;
	}
	if (instruction == GLSL_NORMALIZE) {
		sum = shader_spirv_constant_operand(0);
		for (index = 0; index < a_count; index++)
			sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &a[index], &a[index], &sum);
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL_ROOT, &sum, NULL, NULL);
		for (index = 0; index < count; index++)
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &a[index], &t, NULL);
		return reader->error;
	}

	/* Word by word; a mix's one minus its weight is kept for the next word with the same weight. */
	kept_mix = 0;
	for (index = 0; index < count; index++) {
		x = a[a_count == 1 ? 0 : index];
		y = shader_spirv_constant_operand(0);
		z = shader_spirv_constant_operand(0);
		if (b != NULL)
			y = b[b_count == 1 ? 0 : index];
		if (c != NULL)
			z = c[c_count == 1 ? 0 : index];

		switch (instruction) {
		case GLSL_F_ABS:
			result[index] = shader_spirv_float_absolute(x);
			break;
		case GLSL_FLOOR:
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_FLOOR, &x, NULL, NULL);
			break;
		case GLSL_CEIL:
			/* The negated floor of the negated value. */
			x = shader_spirv_float_negate(x);
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_FLOOR, &x, NULL, NULL);
			result[index] = shader_spirv_float_negate(t);
			break;
		case GLSL_FRACT:
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_FLOOR, &x, NULL, NULL);
			t = shader_spirv_float_negate(t);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &x, &t, NULL);
			break;
		case GLSL_ROUND:
			/* The floor of the value plus a half. */
			k = shader_spirv_constant_operand(FLOAT_HALF);
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &x, &k, NULL);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_FLOOR, &t, NULL, NULL);
			break;
		case GLSL_F_SIGN:
			/* One, minus one or zero by two comparisons. */
			k = shader_spirv_constant_operand(0);
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LESS, &k, &x, NULL);
			y = shader_spirv_constant_operand(FLOAT_ONE);
			z = shader_spirv_constant_operand(0);
			t = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &t, &y, &z);
			k = shader_spirv_constant_operand(0);
			sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LESS, &x, &k, NULL);
			y = shader_spirv_constant_operand(FLOAT_ONE | FLOAT_SIGN);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &sum, &y, &t);
			break;
		case GLSL_SIN:
			result[index] = sine(reader, x, 0);
			break;
		case GLSL_COS:
			result[index] = sine(reader, x, 1);
			break;
		case GLSL_EXP2:
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_EXP2, &x, NULL, NULL);
			break;
		case GLSL_LOG2:
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LOG2, &x, NULL, NULL);
			break;
		case GLSL_POW:
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LOG2, &x, NULL, NULL);
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &t, &y, NULL);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_EXP2, &t, NULL, NULL);
			break;
		case GLSL_SQRT:
			/* The reciprocal of the reciprocal root, which is right at zero. */
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL_ROOT, &x, NULL, NULL);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &t, NULL, NULL);
			break;
		case GLSL_INVERSE_SQRT:
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL_ROOT, &x, NULL, NULL);
			break;
		case GLSL_F_MIN:
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &x, &y, NULL);
			break;
		case GLSL_F_MAX:
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MAXIMUM, &x, &y, NULL);
			break;
		case GLSL_U_MIN:
			x = shader_spirv_plain(reader, x);
			y = shader_spirv_plain(reader, y);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_MINIMUM, &x, &y, NULL);
			break;
		case GLSL_U_MAX:
			x = shader_spirv_plain(reader, x);
			y = shader_spirv_plain(reader, y);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_MAXIMUM, &x, &y, NULL);
			break;
		case GLSL_S_MIN:
			x = shader_spirv_plain(reader, x);
			y = shader_spirv_plain(reader, y);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_MINIMUM, &x, &y, NULL);
			break;
		case GLSL_S_MAX:
			x = shader_spirv_plain(reader, x);
			y = shader_spirv_plain(reader, y);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_MAXIMUM, &x, &y, NULL);
			break;
		case GLSL_F_CLAMP:
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MAXIMUM, &x, &y, NULL);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &t, &z, NULL);
			break;
		case GLSL_U_CLAMP:
			x = shader_spirv_plain(reader, x);
			y = shader_spirv_plain(reader, y);
			z = shader_spirv_plain(reader, z);
			t = shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_MAXIMUM, &x, &y, NULL);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_MINIMUM, &t, &z, NULL);
			break;
		case GLSL_S_CLAMP:
			x = shader_spirv_plain(reader, x);
			y = shader_spirv_plain(reader, y);
			z = shader_spirv_plain(reader, z);
			t = shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_MAXIMUM, &x, &y, NULL);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_MINIMUM, &t, &z, NULL);
			break;
		case GLSL_F_MIX:
			/*
			 * Towards 0 or 1 (the desktop's glass whitens and
			 * darkens so): x * (1 - z), or x * (1 - z) + z, with
			 * 1 - z worked out once for the words of one weight.
			 */
			if (y.kind == POWERVR_IR_OPERAND_CONSTANT && y.modifiers == 0 && (y.constant == 0 || y.constant == FLOAT_ONE)) {
				if (kept_mix == 0 || !same_operand(&mix_weight, &z)) {
					k = shader_spirv_constant_operand(FLOAT_ONE);
					t = shader_spirv_float_negate(z);
					mix_rest = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &k, &t, NULL);
					mix_weight = z;
					kept_mix = 1;
				}
				if (y.constant == 0) {
					result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &x, &mix_rest, NULL);
				} else {
					result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &x, &mix_rest, &z);
				}
				break;
			}

			/* x + (y - x) * z. */
			t = shader_spirv_float_negate(x);
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &y, &t, NULL);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &t, &z, &x);
			break;
		case GLSL_FMA:
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &x, &y, &z);
			break;
		case GLSL_N_MIN:
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &x, &y, NULL);
			break;
		case GLSL_N_MAX:
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MAXIMUM, &x, &y, NULL);
			break;
		case GLSL_N_CLAMP:
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MAXIMUM, &x, &y, NULL);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &t, &z, NULL);
			break;
		case GLSL_TRUNC:
			result[index] = float_truncate(reader, x);
			break;
		case GLSL_ROUND_EVEN:
			result[index] = round_even(reader, x);
			break;
		case GLSL_S_ABS:
		case GLSL_S_SIGN:
			result[index] = integer_sign(reader, instruction, x);
			break;
		case GLSL_LDEXP:
			/* x times two to the exponent's power. */
			y = shader_spirv_plain(reader, y);
			t = shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_TO_FLOAT, &y, NULL, NULL);
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_EXP2, &t, NULL, NULL);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &x, &t, NULL);
			break;
		case GLSL_RADIANS:
		case GLSL_DEGREES:
		case GLSL_EXP:
		case GLSL_LOG:
		case GLSL_TAN:
		case GLSL_ASIN:
		case GLSL_ACOS:
		case GLSL_ATAN:
		case GLSL_ATAN2:
		case GLSL_SINH:
		case GLSL_COSH:
		case GLSL_TANH:
		case GLSL_ASINH:
		case GLSL_ACOSH:
		case GLSL_ATANH:
			result[index] = transcendental(reader, instruction, x, y);
			break;
		case GLSL_STEP:
			/* Zero below the edge (x), one from it on. */
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LESS, &y, &x, NULL);
			k = shader_spirv_constant_operand(0);
			sum = shader_spirv_constant_operand(FLOAT_ONE);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &t, &k, &sum);
			break;
		case GLSL_SMOOTH_STEP:
			/* t = clamp((z - x) / (y - x), 0, 1); t * t * (3 - 2 t). */
			t = shader_spirv_float_negate(x);
			sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &y, &t, NULL);
			sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &sum, NULL, NULL);
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &z, &t, NULL);
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &t, &sum, NULL);
			k = shader_spirv_constant_operand(0);
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MAXIMUM, &t, &k, NULL);
			k = shader_spirv_constant_operand(FLOAT_ONE);
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &t, &k, NULL);
			k = shader_spirv_constant_operand(FLOAT_TWO | FLOAT_SIGN);
			sum = shader_spirv_constant_operand(FLOAT_THREE);
			sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &t, &k, &sum);
			sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &sum, &t, NULL);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &sum, &t, NULL);
			break;
		default:
			return ENOTSUP;
		}
	}

	/* Reports what the lowering came to. */
	return reader->error;
}

/* A float rounded toward zero: the floor of its magnitude, its sign kept. */
static struct powervr_ir_operand
float_truncate(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand x)
{
	struct powervr_ir_operand whole;
	struct powervr_ir_operand negative;
	struct powervr_ir_operand zero;

	/* floor(|x|), negated back where x is below zero. */
	whole = shader_spirv_float_absolute(x);
	whole = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_FLOOR, &whole, NULL, NULL);
	zero = shader_spirv_constant_operand(FLOAT_ZERO);
	negative = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LESS, &x, &zero, NULL);
	return select_float(reader, negative, shader_spirv_float_negate(whole), whole);
}

/* A float rounded to the nearest whole number, a half to the even one. */
static struct powervr_ir_operand
round_even(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand x)
{
	struct powervr_ir_operand shifted;
	struct powervr_ir_operand rounded;
	struct powervr_ir_operand tie;
	struct powervr_ir_operand half;
	struct powervr_ir_operand odd;
	struct powervr_ir_operand k;

	/* floor(x + 1/2); a tie is when that sum is whole, and an odd result then goes down by one. */
	k = shader_spirv_constant_operand(FLOAT_HALF);
	shifted = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &x, &k, NULL);
	rounded = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_FLOOR, &shifted, NULL, NULL);
	tie = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_EQUAL, &rounded, &shifted, NULL);
	half = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &rounded, &k, NULL);
	odd = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_FLOOR, &half, NULL, NULL);
	odd = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_NOT_EQUAL, &odd, &half, NULL);
	tie = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &tie, &odd, NULL);
	k = shader_spirv_constant_operand(FLOAT_ONE | FLOAT_SIGN);
	odd = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &rounded, &k, NULL);
	return select_float(reader, tie, odd, rounded);
}

/* An integer's magnitude (GLSL_S_ABS) or sign (GLSL_S_SIGN). */
static struct powervr_ir_operand
integer_sign(
	struct shader_spirv_reader *reader,
	uint32_t instruction,
	struct powervr_ir_operand x)
{
	struct powervr_ir_operand negated;
	struct powervr_ir_operand k;
	struct powervr_ir_operand t;

	/* The larger of x and -x. */
	x = shader_spirv_plain(reader, x);
	if (instruction == GLSL_S_ABS) {
		k = shader_spirv_constant_operand(TRUE_WORD);
		negated = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &x, &k, NULL);
		return shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_MAXIMUM, &x, &negated, NULL);
	}

	/* x clamped to [-1, 1]. */
	k = shader_spirv_constant_operand(TRUE_WORD);
	t = shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_MAXIMUM, &x, &k, NULL);
	k = shader_spirv_constant_operand(1);
	return shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_MINIMUM, &t, &k, NULL);
}

/*
 * One word of the GLSL.std.450 functions built of others: radians and
 * degrees by their factors, exp and log by the base-2 ones, the tangent
 * as sine over cosine, the inverse sines and tangents through atan2,
 * and the hyperbolic functions and their inverses through exp, log and
 * the square root.  Y is atan2's x.
 */
static struct powervr_ir_operand
transcendental(
	struct shader_spirv_reader *reader,
	uint32_t instruction,
	struct powervr_ir_operand x,
	struct powervr_ir_operand y)
{
	struct powervr_ir_operand t;
	struct powervr_ir_operand u;
	struct powervr_ir_operand k;
	struct powervr_ir_operand one;

	one = shader_spirv_constant_operand(FLOAT_ONE);
	switch (instruction) {
	case GLSL_RADIANS:
		k = shader_spirv_constant_operand(FLOAT_PI_OVER_180);
		return shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &x, &k, NULL);
	case GLSL_DEGREES:
		k = shader_spirv_constant_operand(FLOAT_180_OVER_PI);
		return shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &x, &k, NULL);
	case GLSL_EXP:
		return natural_exp(reader, x);
	case GLSL_LOG:
		return natural_log(reader, x);
	case GLSL_TAN:
		t = sine(reader, x, 0);
		u = sine(reader, x, 1);
		u = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &u, NULL, NULL);
		return shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &t, &u, NULL);
	case GLSL_ASIN:
	case GLSL_ACOS:
		/* sqrt(1 - x * x), then atan2(x, it) or atan2(it, x). */
		t = shader_spirv_float_negate(x);
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &t, &x, &one);
		t = square_root(reader, t);
		if (instruction == GLSL_ASIN)
			return arctangent(reader, x, t);
		return arctangent(reader, t, x);
	case GLSL_ATAN:
		return arctangent(reader, x, one);
	case GLSL_ATAN2:
		return arctangent(reader, x, y);
	case GLSL_SINH:
	case GLSL_COSH:
		/* (e^x -+ e^-x) / 2. */
		t = natural_exp(reader, x);
		u = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &t, NULL, NULL);
		if (instruction == GLSL_SINH)
			u = shader_spirv_float_negate(u);
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &t, &u, NULL);
		k = shader_spirv_constant_operand(FLOAT_HALF);
		return shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &t, &k, NULL);
	case GLSL_TANH:
		/* (e^2x - 1) / (e^2x + 1), x clamped to [-10, 10] so neither overflows. */
		k = shader_spirv_constant_operand(FLOAT_TEN | FLOAT_SIGN);
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MAXIMUM, &x, &k, NULL);
		k = shader_spirv_constant_operand(FLOAT_TEN);
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &t, &k, NULL);
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &t, &t, NULL);
		t = natural_exp(reader, t);
		u = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &t, &one, NULL);
		u = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &u, NULL, NULL);
		k = shader_spirv_float_negate(one);
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &t, &k, NULL);
		return shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &t, &u, NULL);
	case GLSL_ASINH:
		/* The sign of x times log(|x| + sqrt(x * x + 1)). */
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &x, &x, &one);
		t = square_root(reader, t);
		u = shader_spirv_float_absolute(x);
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &u, &t, NULL);
		t = natural_log(reader, t);
		k = shader_spirv_constant_operand(FLOAT_ZERO);
		u = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LESS, &x, &k, NULL);
		return select_float(reader, u, shader_spirv_float_negate(t), t);
	case GLSL_ACOSH:
		/* log(x + sqrt(x * x - 1)). */
		k = shader_spirv_float_negate(one);
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &x, &x, &k);
		t = square_root(reader, t);
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &x, &t, NULL);
		return natural_log(reader, t);
	default:
		/* atanh: log((1 + x) / (1 - x)) / 2. */
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &one, &x, NULL);
		u = shader_spirv_float_negate(x);
		u = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &one, &u, NULL);
		u = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &u, NULL, NULL);
		t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &t, &u, NULL);
		t = natural_log(reader, t);
		k = shader_spirv_constant_operand(FLOAT_HALF);
		return shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &t, &k, NULL);
	}
}

/* e to the power x, as 2 to the power x log2(e). */
static struct powervr_ir_operand
natural_exp(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand x)
{
	struct powervr_ir_operand k;
	struct powervr_ir_operand t;

	k = shader_spirv_constant_operand(FLOAT_LOG2_E);
	t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &x, &k, NULL);
	return shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_EXP2, &t, NULL, NULL);
}

/* The natural logarithm, as log2(x) ln(2). */
static struct powervr_ir_operand
natural_log(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand x)
{
	struct powervr_ir_operand k;
	struct powervr_ir_operand t;

	t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LOG2, &x, NULL, NULL);
	k = shader_spirv_constant_operand(FLOAT_LN_2);
	return shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &t, &k, NULL);
}

/* The square root, as the reciprocal of the reciprocal root (right at zero). */
static struct powervr_ir_operand
square_root(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand x)
{
	struct powervr_ir_operand t;

	t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL_ROOT, &x, NULL, NULL);
	return shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &t, NULL, NULL);
}

/* A select of two floats by a Boolean word, the floats' modifiers applied first. */
static struct powervr_ir_operand
select_float(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand condition,
	struct powervr_ir_operand yes,
	struct powervr_ir_operand no)
{
	condition = shader_spirv_plain(reader, condition);
	yes = shader_spirv_plain(reader, yes);
	no = shader_spirv_plain(reader, no);
	return shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &condition, &yes, &no);
}

/*
 * atan2(y, x): the arctangent of the smaller magnitude over the larger
 * (zero when both are zero) by its polynomial on [0, 1], then moved to
 * its octant and quadrant by the magnitudes' order and the signs.
 *
 * Values referenced:
 * M. Abramowitz and I. A. Stegun, Handbook of Mathematical Functions
 * (1964), formula 4.4.49 (arctan x on [0, 1] to within 2e-8: the
 * coefficients of x^2 to x^16).
 */
static struct powervr_ir_operand
arctangent(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand y,
	struct powervr_ir_operand x)
{
	static const uint32_t coefficients[8] = {
		0x3b3bd74aU,	/* 0.0028662257 */
		0xbc846e02U,	/* -0.0161657367 */
		0x3d2fc1feU,	/* 0.0429096138 */
		0xbd9a3174U,	/* -0.0752896400 */
		0x3dda3d83U,	/* 0.1065626393 */
		0xbe117fc7U,	/* -0.1420889944 */
		0x3e4cbbe5U,	/* 0.1999355085 */
		0xbeaaaa6cU	/* -0.3333314528 */
	};
	struct powervr_ir_operand ax;
	struct powervr_ir_operand ay;
	struct powervr_ir_operand larger;
	struct powervr_ir_operand smaller;
	struct powervr_ir_operand ratio;
	struct powervr_ir_operand square;
	struct powervr_ir_operand sum;
	struct powervr_ir_operand test;
	struct powervr_ir_operand k;
	struct powervr_ir_operand t;
	uint32_t index;

	/* The smaller magnitude over the larger, 0 for 0 over 0. */
	ax = shader_spirv_float_absolute(x);
	ay = shader_spirv_float_absolute(y);
	larger = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MAXIMUM, &ax, &ay, NULL);
	smaller = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &ax, &ay, NULL);
	t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &larger, NULL, NULL);
	ratio = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &smaller, &t, NULL);
	k = shader_spirv_constant_operand(FLOAT_ZERO);
	test = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_EQUAL, &larger, &k, NULL);
	ratio = select_float(reader, test, k, ratio);

	/* Its arctangent: ratio (1 + a2 r^2 + ... + a16 r^16). */
	square = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &ratio, &ratio, NULL);
	sum = shader_spirv_constant_operand(coefficients[0]);
	for (index = 1; index < 8; index++) {
		k = shader_spirv_constant_operand(coefficients[index]);
		sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &sum, &square, &k);
	}
	k = shader_spirv_constant_operand(FLOAT_ONE);
	sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &sum, &square, &k);
	sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &sum, &ratio, NULL);

	/* pi/2 less it where |y| is the larger, pi less that where x is negative, negated where y is. */
	test = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LESS, &ax, &ay, NULL);
	k = shader_spirv_constant_operand(FLOAT_HALF_PI);
	t = shader_spirv_float_negate(sum);
	t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &k, &t, NULL);
	sum = select_float(reader, test, t, sum);
	k = shader_spirv_constant_operand(FLOAT_ZERO);
	test = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LESS, &x, &k, NULL);
	k = shader_spirv_constant_operand(FLOAT_PI);
	t = shader_spirv_float_negate(sum);
	t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &k, &t, NULL);
	sum = select_float(reader, test, t, sum);
	k = shader_spirv_constant_operand(FLOAT_ZERO);
	test = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LESS, &y, &k, NULL);
	return select_float(reader, test, shader_spirv_float_negate(sum), sum);
}

/*
 * The GLSL.std.450 functions of whole vectors and matrices: cross,
 * reflect, refract, faceforward, determinant and inverse.
 */
static int
reader_geometry(
	struct shader_spirv_reader *reader,
	uint32_t instruction,
	struct powervr_ir_operand *result,
	uint32_t count,
	const struct powervr_ir_operand *a,
	uint32_t a_count,
	const struct powervr_ir_operand *b,
	uint32_t b_count,
	const struct powervr_ir_operand *c,
	uint32_t c_count)
{
	struct powervr_ir_operand dot;
	struct powervr_ir_operand t;
	struct powervr_ir_operand u;
	struct powervr_ir_operand k;
	struct powervr_ir_operand scale;
	struct powervr_ir_operand test;
	struct powervr_ir_operand zero;
	uint32_t index;
	uint32_t row;
	uint32_t column;
	uint32_t size;
	uint32_t all;

	/* A matrix's determinant, or its inverse: the adjugate over the determinant. */
	if (instruction == GLSL_DETERMINANT || instruction == GLSL_MATRIX_INVERSE) {
		/* The size whose square the matrix's words are, 2 to 4; one result word, or the matrix's. */
		for (size = 2; size <= 4 && size * size != a_count; size++)
			;
		if (size > 4)
			return ENOTSUP;
		if (instruction == GLSL_DETERMINANT && count != 1)
			return EINVAL;
		if (instruction == GLSL_MATRIX_INVERSE && count != a_count)
			return EINVAL;
		all = (1U << size) - 1U;
		dot = minor_determinant(reader, a, size, all, all);
		if (instruction == GLSL_DETERMINANT) {
			result[0] = dot;
			return reader->error;
		}
		scale = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &dot, NULL, NULL);
		for (column = 0; column < size; column++) {
			for (row = 0; row < size; row++) {
				t = minor_determinant(reader, a, size, all & ~(1U << column), all & ~(1U << row));
				if (((row + column) & 1U) != 0)
					t = shader_spirv_float_negate(t);
				result[column * size + row] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &t, &scale, NULL);
			}
		}
		return reader->error;
	}

	/* The rest take two vectors of the result's size. */
	if (b == NULL || a_count != count || b_count != count)
		return EINVAL;
	if (instruction == GLSL_CROSS) {
		/* a.y b.z - a.z b.y and its rotations. */
		if (count != 3)
			return EINVAL;
		for (index = 0; index < 3; index++) {
			t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &a[(index + 1U) % 3U], &b[(index + 2U) % 3U], NULL);
			u = shader_spirv_float_negate(a[(index + 2U) % 3U]);
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &u, &b[(index + 1U) % 3U], &t);
		}
		return reader->error;
	}

	/* The dot product of the second and the first (of the third and the second for faceforward). */
	if (instruction == GLSL_FACE_FORWARD) {
		if (c == NULL || c_count != count)
			return EINVAL;
		dot = dot_product(reader, c, b, count);
		zero = shader_spirv_constant_operand(FLOAT_ZERO);
		test = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LESS, &dot, &zero, NULL);
		for (index = 0; index < count; index++)
			result[index] = select_float(reader, test, a[index], shader_spirv_float_negate(a[index]));
		return reader->error;
	}
	dot = dot_product(reader, b, a, count);
	if (instruction == GLSL_REFLECT) {
		/* I - 2 dot(N, I) N. */
		k = shader_spirv_constant_operand(FLOAT_TWO | FLOAT_SIGN);
		scale = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &dot, &k, NULL);
		for (index = 0; index < count; index++)
			result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &scale, &b[index], &a[index]);
		return reader->error;
	}

	/* refract: k = 1 - eta^2 (1 - dot^2); 0 where k < 0, else eta I - (eta dot + sqrt(k)) N. */
	if (c == NULL || c_count == 0)
		return EINVAL;
	k = shader_spirv_constant_operand(FLOAT_ONE);
	t = shader_spirv_float_negate(dot);
	t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &t, &dot, &k);
	u = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &c[0], &c[0], NULL);
	u = shader_spirv_float_negate(u);
	t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &u, &t, &k);
	zero = shader_spirv_constant_operand(FLOAT_ZERO);
	test = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LESS, &t, &zero, NULL);
	t = square_root(reader, t);
	scale = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &c[0], &dot, &t);
	scale = shader_spirv_float_negate(scale);
	for (index = 0; index < count; index++) {
		u = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &c[0], &a[index], NULL);
		u = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &scale, &b[index], &u);
		result[index] = select_float(reader, test, zero, u);
	}

	/* Reports what the function came to. */
	return reader->error;
}

/* The dot product of two vectors of a count of words. */
static struct powervr_ir_operand
dot_product(
	struct shader_spirv_reader *reader,
	const struct powervr_ir_operand *a,
	const struct powervr_ir_operand *b,
	uint32_t count)
{
	struct powervr_ir_operand sum;
	uint32_t index;

	sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &a[0], &b[0], NULL);
	for (index = 1; index < count; index++)
		sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &a[index], &b[index], &sum);
	return sum;
}

/*
 * The determinant of the square part of a matrix (columns of size words)
 * its row and column masks leave, by expansion along its first column.
 */
static struct powervr_ir_operand
minor_determinant(
	struct shader_spirv_reader *reader,
	const struct powervr_ir_operand *matrix,
	uint32_t size,
	uint32_t rows,
	uint32_t columns)
{
	struct powervr_ir_operand sum;
	struct powervr_ir_operand term;
	struct powervr_ir_operand entry;
	uint32_t column;
	uint32_t row;
	uint32_t sign;

	/* The first column left. */
	for (column = 0; column < size && (columns & (1U << column)) == 0; column++)
		;
	if (column >= size)
		return shader_spirv_constant_operand(FLOAT_ONE);
	columns &= ~(1U << column);

	/* One entry left: itself. */
	if (columns == 0) {
		for (row = 0; row < size && (rows & (1U << row)) == 0; row++)
			;
		return matrix[column * size + (row < size ? row : 0U)];
	}

	/* Each row's entry times its minor, the signs alternating down the rows left. */
	sum = shader_spirv_constant_operand(FLOAT_ZERO);
	sign = 0;
	for (row = 0; row < size; row++) {
		if ((rows & (1U << row)) == 0)
			continue;
		term = minor_determinant(reader, matrix, size, rows & ~(1U << row), columns);
		entry = matrix[column * size + row];
		if (sign != 0)
			entry = shader_spirv_float_negate(entry);
		sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &entry, &term, &sum);
		sign ^= 1U;
	}

	/* Succeeded: the determinant. */
	return sum;
}

/*
 * The GLSL.std.450 packs and unpacks: a vector of 2 or 4 floats into one
 * word by the core's pack format, or one word into such a vector.
 */
static int
reader_packing(
	struct shader_spirv_reader *reader,
	uint32_t instruction,
	struct powervr_ir_operand *result,
	uint32_t count,
	const struct powervr_ir_operand *a,
	uint32_t a_count)
{
	struct powervr_ir_instruction *pack;
	struct powervr_ir_operand word;
	uint32_t format;
	uint32_t channels;
	uint32_t value;
	uint32_t index;

	/* The format and the channels of each. */
	switch (instruction) {
	case GLSL_PACK_SNORM_4X8:
	case GLSL_UNPACK_SNORM_4X8:
		format = POWERVR_IR_PACK_S8888;
		channels = 4;
		break;
	case GLSL_PACK_UNORM_4X8:
	case GLSL_UNPACK_UNORM_4X8:
		format = POWERVR_IR_PACK_U8888;
		channels = 4;
		break;
	case GLSL_PACK_SNORM_2X16:
	case GLSL_UNPACK_SNORM_2X16:
		format = POWERVR_IR_PACK_S1616;
		channels = 2;
		break;
	case GLSL_PACK_UNORM_2X16:
	case GLSL_UNPACK_UNORM_2X16:
		format = POWERVR_IR_PACK_U1616;
		channels = 2;
		break;
	case GLSL_PACK_HALF_2X16:
	case GLSL_UNPACK_HALF_2X16:
		format = POWERVR_IR_PACK_F16F16;
		channels = 2;
		break;
	default:
		return ENOTSUP;
	}

	/* A pack: the channels into one word. */
	if (instruction <= GLSL_PACK_HALF_2X16) {
		if (a_count != channels || count != 1)
			return EINVAL;
		value = shader_spirv_new_value(reader, 1);
		shader_spirv_emit(reader, POWERVR_IR_PACK_CHANNELS, value, &a[0], &a[1], channels > 2 ? &a[2] : NULL, format, channels);
		if (channels > 3 && reader->error == 0) {
			pack = &reader->shader->instructions[reader->shader->instruction_count - 1U];
			pack->source[3] = a[3];
		}
		result[0] = shader_spirv_value_operand(value, 0);
		return reader->error;
	}

	/* An unpack: one word into the channels. */
	if (a_count != 1 || count != channels)
		return EINVAL;
	word = shader_spirv_plain(reader, a[0]);
	value = shader_spirv_new_value(reader, channels);
	shader_spirv_emit(reader, POWERVR_IR_UNPACK_CHANNELS, value, &word, NULL, NULL, format, channels);
	for (index = 0; index < channels; index++)
		result[index] = shader_spirv_value_operand(value, index);

	/* Reports what the packing came to. */
	return reader->error;
}

/*
 * A modf: the fraction (x less x truncated), with the whole part stored
 * through the pointer (GLSL_MODF) or the second member of the result
 * (GLSL_MODF_STRUCT).
 */
static int
reader_modf(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length,
	const struct powervr_ir_operand *a,
	uint32_t a_count,
	uint32_t count)
{
	struct powervr_ir_operand wholes[COMPOSITE_MAX];
	struct powervr_ir_operand *result;
	struct powervr_ir_operand t;
	uint32_t index;
	int error;

	/* The whole parts, then the fractions. */
	if (a_count == 0 || a_count > COMPOSITE_MAX / 2U)
		return ENOTSUP;
	if (words[4] == GLSL_MODF && (length < 7 || count != a_count))
		return EINVAL;
	if (words[4] == GLSL_MODF_STRUCT && count != a_count * 2U)
		return EINVAL;
	error = shader_spirv_result_begin(reader, words[2], words[1], count, &result);
	if (error != 0)
		return error;
	for (index = 0; index < a_count; index++) {
		wholes[index] = float_truncate(reader, a[index]);
		t = shader_spirv_float_negate(wholes[index]);
		result[index] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &a[index], &t, NULL);
		if (words[4] == GLSL_MODF_STRUCT)
			result[a_count + index] = wholes[index];
	}

	/* The whole parts through the pointer. */
	if (words[4] == GLSL_MODF) {
		error = store_words(reader, words[6], wholes, a_count);
		if (error != 0)
			return error;
	}

	/* Reports what the modf came to. */
	return reader->error;
}

/*
 * The products of matrices (columns of words) with vectors and with
 * each other, the outer product of two vectors, and the transpose.
 */
static int
reader_matrix(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	const struct powervr_ir_operand *a,
	uint32_t a_count,
	const struct powervr_ir_operand *b,
	uint32_t b_count,
	uint32_t count)
{
	struct powervr_ir_operand *result;
	struct powervr_ir_operand sum;
	const struct shader_spirv_id *type;
	uint32_t opcode;
	uint32_t rows;
	uint32_t columns;
	uint32_t inner;
	uint32_t row;
	uint32_t column;
	uint32_t index;
	int error;

	/* The result's words. */
	opcode = words[0] & 0xffffU;
	error = shader_spirv_result_begin(reader, words[2], words[1], count, &result);
	if (error != 0)
		return error;

	/* The transpose: the rows of the source (its column type's words) become columns. */
	if (opcode == OP_TRANSPOSE) {
		type = &reader->ids[reader->ids[words[3]].type];
		if (type->type_kind != TYPE_MATRIX || type->type >= reader->bound || a_count != count)
			return ENOTSUP;
		rows = reader->ids[type->type].count;
		columns = type->count;
		for (column = 0; column < columns; column++) {
			for (row = 0; row < rows; row++)
				result[row * columns + column] = a[column * rows + row];
		}
		return reader->error;
	}

	/* A vector times a matrix: each column's dot product with the vector. */
	if (b == NULL)
		return EINVAL;
	if (opcode == OP_VECTOR_TIMES_MATRIX) {
		if (b_count != a_count * count)
			return EINVAL;
		for (column = 0; column < count; column++)
			result[column] = dot_product(reader, a, &b[column * a_count], a_count);
		return reader->error;
	}

	/* The outer product: column j is the first vector times the second's word j. */
	if (opcode == OP_OUTER_PRODUCT) {
		if (a_count * b_count != count)
			return EINVAL;
		for (column = 0; column < b_count; column++) {
			for (row = 0; row < a_count; row++)
				result[column * a_count + row] = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &a[row], &b[column], NULL);
		}
		return reader->error;
	}

	/* A matrix times a matrix: entry (row, column) is the left's row times the right's column. */
	type = &reader->ids[words[1]];
	if (type->type_kind != TYPE_MATRIX || type->type >= reader->bound)
		return ENOTSUP;
	rows = reader->ids[type->type].count;
	columns = type->count;
	if (rows == 0 || a_count % rows != 0)
		return EINVAL;
	inner = a_count / rows;
	if (b_count != inner * columns)
		return EINVAL;
	for (column = 0; column < columns; column++) {
		for (row = 0; row < rows; row++) {
			sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &a[row], &b[column * inner], NULL);
			for (index = 1; index < inner; index++)
				sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &a[index * rows + row], &b[column * inner + index], &sum);
			result[column * rows + row] = sum;
		}
	}

	/* Reports what the product came to. */
	return reader->error;
}

/*
 * Reads a vector's word at a run-time index (OpVectorExtractDynamic), or
 * the vector with that word replaced (OpVectorInsertDynamic): a select
 * for each word the index may name.
 */
static int
reader_dynamic_component(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length,
	const struct powervr_ir_operand *a,
	uint32_t a_count,
	const struct powervr_ir_operand *b,
	uint32_t b_count,
	uint32_t count)
{
	struct powervr_ir_operand *result;
	struct powervr_ir_operand index;
	struct powervr_ir_operand chosen;
	struct powervr_ir_operand test;
	struct powervr_ir_operand k;
	uint32_t word;
	int error;

	/* The index: the second operand of an extract, the third of an insert. */
	if (b == NULL || b_count == 0)
		return EINVAL;
	if ((words[0] & 0xffffU) == OP_VECTOR_INSERT_DYNAMIC) {
		if (length < 6)
			return EINVAL;
		error = shader_spirv_operand_word(reader, words[5], 0, &index);
	} else {
		error = shader_spirv_operand_word(reader, words[4], 0, &index);
	}
	if (error != 0)
		return error;
	index = shader_spirv_plain(reader, index);
	error = shader_spirv_result_begin(reader, words[2], words[1], count, &result);
	if (error != 0)
		return error;

	/* An extract: the word whose number the index equals (the first when none). */
	if ((words[0] & 0xffffU) == OP_VECTOR_EXTRACT_DYNAMIC) {
		if (count != 1)
			return EINVAL;
		chosen = shader_spirv_plain(reader, a[0]);
		for (word = 1; word < a_count; word++) {
			k = shader_spirv_constant_operand(word);
			test = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_EQUAL, &index, &k, NULL);
			chosen = select_float(reader, test, a[word], chosen);
		}
		result[0] = chosen;
		return reader->error;
	}

	/* An insert: each word, or the new one where the index names it. */
	if (a_count != count)
		return EINVAL;
	for (word = 0; word < count; word++) {
		k = shader_spirv_constant_operand(word);
		test = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_EQUAL, &index, &k, NULL);
		result[word] = select_float(reader, test, b[0], a[word]);
	}

	/* Reports what the access came to. */
	return reader->error;
}

/*
 * Works out sin(x), or cos(x) with quarter set, by reducing the angle
 * to a turn in [-1/2, 1/2) and summing the odd Taylor terms of
 * sin(2 pi t).
 */
static struct powervr_ir_operand
sine(
	struct shader_spirv_reader *reader,
	struct powervr_ir_operand x,
	uint32_t quarter)
{
	struct powervr_ir_operand turn;
	struct powervr_ir_operand k;
	struct powervr_ir_operand t;
	struct powervr_ir_operand y;
	struct powervr_ir_operand y2;
	struct powervr_ir_operand sum;
	uint32_t index;

	/*
	 * The turn: x / 2 pi, plus a quarter for a cosine and a half to
	 * centre it, less its floor, less the half again.
	 */
	k = shader_spirv_constant_operand(0x3e22f983U);	/* 1 / 2 pi */
	turn = shader_spirv_constant_operand(quarter != 0 ? 0x3f400000U : FLOAT_HALF);	/* 3/4 or 1/2 */
	t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &x, &k, &turn);
	y = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_FLOOR, &t, NULL, NULL);
	y = shader_spirv_float_negate(y);
	t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &t, &y, NULL);
	k = shader_spirv_constant_operand(FLOAT_HALF | FLOAT_SIGN);
	t = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &t, &k, NULL);

	/* The angle y = 2 pi t in [-pi, pi) and its square. */
	k = shader_spirv_constant_operand(0x40c90fdbU);	/* 2 pi */
	y = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &t, &k, NULL);
	y2 = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &y, &y, NULL);

	/* Horner's rule over y^2, highest term first, then times y. */
	sum = shader_spirv_constant_operand(sine_coefficients[0]);
	for (index = 1; index < 8; index++) {
		k = shader_spirv_constant_operand(sine_coefficients[index]);
		sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY_ADD, &sum, &y2, &k);
	}
	sum = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &sum, &y, NULL);

	/* Succeeded: the sine's value. */
	return sum;
}

/* Reads a plain fragment sample of a two-dimensional combined image at two coordinates, compared with a reference or not. */
static int
reader_sample(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length)
{
	struct powervr_ir_operand *coordinates;
	struct powervr_ir_operand *references;
	struct powervr_ir_operand *result;
	struct powervr_ir_operand u;
	struct powervr_ir_operand v;
	struct shader_spirv_id *image;
	uint32_t coordinate_count;
	uint32_t reference_count;
	uint32_t image_request;
	uint32_t sampler_request;
	uint32_t sampled;
	uint32_t opcode;
	uint32_t operands;
	uint32_t index;
	int error;

	/* Only plain fragment samples of a two-dimensional image, compared with a reference or not. */
	opcode = words[0] & 0xffffU;
	operands = opcode == OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD ? 6U : 5U;
	if (length != operands || reader->stage != POWERVR_SHADER_FRAGMENT)
		return ENOTSUP;
	if (words[3] >= reader->bound || words[2] >= reader->bound)
		return EINVAL;
	image = &reader->ids[words[3]];
	if (image->kind != ID_SAMPLED_IMAGE)
		return ENOTSUP;
	error = shader_spirv_id_operands(reader, words[4], &coordinates, &coordinate_count);
	if (error != 0)
		return error;
	if (coordinate_count != 2)
		return ENOTSUP;

	/* The image and sampler state words of the binding's element. */
	error = shader_spirv_request_element(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_IMAGE_STATE, image->set, image->binding, image->offset,
					     POWERVR_SHADER_STATE_WORDS, &image_request);
	if (error != 0)
		return error;
	error = shader_spirv_request_element(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_SAMPLER_STATE, image->set, image->binding, image->offset,
					     POWERVR_SHADER_STATE_WORDS, &sampler_request);
	if (error != 0)
		return error;

	/* The sample into a four-word value. */
	u = coordinates[0];
	v = coordinates[1];
	sampled = shader_spirv_new_value(reader, 4);
	shader_spirv_emit(reader, POWERVR_IR_SAMPLE, sampled, &u, &v, NULL, image_request, sampler_request);

	/* A plain sample is its four words. */
	if (opcode == OP_IMAGE_SAMPLE_IMPLICIT_LOD) {
		error = shader_spirv_result_begin(reader, words[2], words[1], 4, &result);
		if (error != 0)
			return error;
		for (index = 0; index < 4; index++)
			result[index] = shader_spirv_value_operand(sampled, index);
		return reader->error;
	}

	/* A compared one is one or zero. */
	error = shader_spirv_id_operands(reader, words[5], &references, &reference_count);
	if (error != 0)
		return error;
	if (reference_count != 1)
		return ENOTSUP;
	error = shader_spirv_result_begin(reader, words[2], words[1], 1, &result);
	if (error != 0)
		return error;
	error = compare_depth(reader, image, shader_spirv_value_operand(sampled, 0), references[0], &result[0]);
	if (error != 0)
		return error;

	/* Reports what the sample came to. */
	return reader->error;
}

/*
 * Reads a lookup of a combined image: a sample with or without a level
 * of detail, gradients, a projection, a reference to compare with or
 * offsets, or a fetch of one texel.  A plain fragment sample of a
 * two-dimensional image of floats keeps its own instruction (the forms
 * the back end gives it); everything else is a texture lookup.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_nir_tex.c (lower_tex and
 * pco_emit_nir_smp: the data words in the order coordinates, level of
 * detail or gradients, address, offsets; an array layer rounded,
 * clamped to the view's layers and turned into the texels' address,
 * with a level of detail bias of zero when the lookup has none; a fetch
 * as texel coordinates with the level replaced, by a point sampler; a
 * projection's divisor and a comparison after the lookup).
 */
static int
reader_texture(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length)
{
	struct powervr_ir_operand data[TEXTURE_DATA_MAX];
	struct powervr_ir_operand *coordinates;
	struct powervr_ir_operand *operands;
	struct powervr_ir_operand *result;
	struct powervr_ir_operand reference;
	struct powervr_ir_operand divisor;
	struct powervr_ir_operand address[2];
	struct powervr_ir_operand last[3];
	struct powervr_ir_operand word;
	struct powervr_ir_operand one;
	struct powervr_ir_operand k;
	struct shader_spirv_id *image;
	struct texture_shape shape;
	uint32_t ids[TEXTURE_OPERAND_IDS];
	uint32_t coordinate_count;
	uint32_t operand_count;
	uint32_t image_request;
	uint32_t sampler_request;
	uint32_t opcode;
	uint32_t mask;
	uint32_t lod;
	uint32_t flags;
	uint32_t count;
	uint32_t index;
	uint32_t at;
	uint32_t sampled;
	uint32_t fetch;
	uint32_t compared;
	uint32_t projected;
	uint32_t explicit_lod;
	int error;

	/* The lookup's kind and its fixed operands. */
	opcode = words[0] & 0xffffU;
	if (length < 5 || words[1] >= reader->bound || words[2] >= reader->bound || words[3] >= reader->bound)
		return EINVAL;
	image = &reader->ids[words[3]];
	if (image->kind != ID_SAMPLED_IMAGE)
		return ENOTSUP;
	fetch = opcode == OP_IMAGE_FETCH;
	compared = opcode == OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD || opcode == OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD ||
		   opcode == OP_IMAGE_SAMPLE_PROJ_DREF_IMPLICIT_LOD || opcode == OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD;
	projected = opcode >= OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD && opcode <= OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD;
	explicit_lod = opcode == OP_IMAGE_SAMPLE_EXPLICIT_LOD || opcode == OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD ||
		       opcode == OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD || opcode == OP_IMAGE_SAMPLE_PROJ_DREF_EXPLICIT_LOD;
	at = 5;
	if (compared) {
		if (length < 6)
			return EINVAL;
		at = 6;
	}
	mask = 0;
	if (at < length)
		mask = words[at++];
	error = texture_shape_of(reader, image, &shape);
	if (error != 0)
		return error;

	/* A plain fragment sample of a two-dimensional image of floats, compared or not, keeps its instruction. */
	if ((opcode == OP_IMAGE_SAMPLE_IMPLICIT_LOD || opcode == OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD) && mask == 0 && length == (compared ? 6U : 5U) && shape.dimensions == 2 &&
	    shape.cube == 0 && shape.arrayed == 0 && shape.integer == 0 && reader->stage == POWERVR_SHADER_FRAGMENT) {
		error = reader_sample(reader, words, length);
		if (error != 0)
			return error;

		/* Succeeded: the plain sample is read. */
		return 0;
	}

	/* The level of detail of an implicit lookup comes from the fragments around it. */
	if (!fetch && !explicit_lod && reader->stage != POWERVR_SHADER_FRAGMENT)
		return ENOTSUP;

	/* The image operands' ids, in the order of their bits (those without ids ignored). */
	error = texture_operands(words, length, at, mask, ids);
	if (error != 0)
		return error;
	if (explicit_lod && ids[TEXTURE_LOD] == 0 && ids[TEXTURE_GRADIENT_X] == 0)
		return EINVAL;
	if (ids[TEXTURE_BIAS] != 0 && (fetch || explicit_lod))
		return EINVAL;

	/* A multisampled image only fetched, by a sample number, at its one level. */
	if (shape.multisampled != (ids[TEXTURE_SAMPLE] != 0))
		return EINVAL;
	if (shape.multisampled && (!fetch || ids[TEXTURE_LOD] != 0))
		return EINVAL;

	/* The coordinates, and a projection's divisor after them. */
	error = shader_spirv_id_operands(reader, words[4], &coordinates, &coordinate_count);
	if (error != 0)
		return error;
	if (coordinate_count < shape.dimensions + shape.arrayed + projected)
		return EINVAL;
	if (projected && shape.arrayed)
		return EINVAL;
	one = shader_spirv_constant_operand(FLOAT_ONE);
	divisor = one;
	if (projected)
		divisor = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_RECIPROCAL, &coordinates[shape.dimensions], NULL, NULL);

	/* A fetch's texels kept inside its level (texture_fetch_last). */
	if (fetch) {
		error = texture_fetch_last(reader, image, ids[TEXTURE_LOD], shape.dimensions, last);
		if (error != 0)
			return error;
	}

	/* The coordinates as floats: divided by the divisor, or a fetch's texels. */
	count = 0;
	flags = 0;
	for (index = 0; index < shape.dimensions; index++) {
		word = coordinates[index];
		if (fetch) {
			word = shader_spirv_plain(reader, word);
			k = shader_spirv_constant_operand(0);
			word = shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_MAXIMUM, &word, &k, NULL);
			word = shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_MINIMUM, &word, &last[index], NULL);
			word = shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_TO_FLOAT, &word, NULL, NULL);
		} else if (projected) {
			word = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &word, &divisor, NULL);
		}
		data[count++] = word;
	}
	if (fetch)
		flags |= POWERVR_IR_TEXTURE_TEXELS;

	/* The level of detail: a bias, a level (a fetch's level 0 unless named), or gradients. */
	lod = POWERVR_IR_TEXTURE_LOD_NORMAL;
	if (ids[TEXTURE_BIAS] != 0 || ids[TEXTURE_LOD] != 0 || fetch) {
		lod = ids[TEXTURE_BIAS] != 0 ? POWERVR_IR_TEXTURE_LOD_BIAS : POWERVR_IR_TEXTURE_LOD_REPLACE;
		word = shader_spirv_constant_operand(FLOAT_ZERO);
		if (ids[TEXTURE_BIAS] != 0 || ids[TEXTURE_LOD] != 0) {
			error = shader_spirv_operand_word(reader, ids[TEXTURE_BIAS] != 0 ? ids[TEXTURE_BIAS] : ids[TEXTURE_LOD], 0, &word);
			if (error != 0)
				return error;
		}
		if (fetch && ids[TEXTURE_LOD] != 0) {
			word = shader_spirv_plain(reader, word);
			word = shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_TO_FLOAT, &word, NULL, NULL);
		}
		data[count++] = word;
	} else if (ids[TEXTURE_GRADIENT_X] != 0) {
		lod = POWERVR_IR_TEXTURE_LOD_GRADIENTS;
		if (count + shape.dimensions * 2U > TEXTURE_DATA_MAX)
			return ENOTSUP;
		for (index = 0; index < shape.dimensions; index++) {
			error = shader_spirv_operand_word(reader, ids[TEXTURE_GRADIENT_X], index, &data[count++]);
			if (error != 0)
				return error;
			error = shader_spirv_operand_word(reader, ids[TEXTURE_GRADIENT_Y], index, &data[count++]);
			if (error != 0)
				return error;
		}
	}

	/* The image state words, and an array layer's texels by their address (with a bias of zero if there is no level of detail yet). */
	error = shader_spirv_request_element(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_IMAGE_STATE, image->set, image->binding, image->offset,
					     POWERVR_SHADER_STATE_WORDS, &image_request);
	if (error != 0)
		return error;
	if (shape.arrayed) {
		error = texture_layer_address(reader, image, image_request, coordinates[shape.dimensions], fetch, address);
		if (error != 0)
			return error;
		if (lod == POWERVR_IR_TEXTURE_LOD_NORMAL) {
			lod = POWERVR_IR_TEXTURE_LOD_BIAS;
			if (count >= TEXTURE_DATA_MAX)
				return ENOTSUP;
			data[count++] = shader_spirv_constant_operand(FLOAT_ZERO);
		}
		if (count + 2U > TEXTURE_DATA_MAX)
			return ENOTSUP;
		data[count++] = address[0];
		data[count++] = address[1];
		flags |= POWERVR_IR_TEXTURE_ADDRESS;
	}

	/* The texel offsets, packed into one word. */
	if (ids[TEXTURE_CONSTANT_OFFSET] != 0 || ids[TEXTURE_OFFSET] != 0) {
		if (count >= TEXTURE_DATA_MAX)
			return ENOTSUP;
		error = texture_offsets(reader, ids[TEXTURE_CONSTANT_OFFSET] != 0 ? ids[TEXTURE_CONSTANT_OFFSET] : ids[TEXTURE_OFFSET], shape.dimensions, &data[count++]);
		if (error != 0)
			return error;
		flags |= POWERVR_IR_TEXTURE_OFFSETS;
	}

	/*
	 * A multisampled image's sample number, in bits 16 to 18 of the
	 * lookup word the offsets are packed in (a word of its own without
	 * them).
	 *
	 * Values referenced: Mesa 26.2.6 src/imagination/pco/pco_nir_tex.c
	 * (the lookup word: ms_index inserted at ROGUE_SMP_LOOKUP_MS_NUM_START
	 * 16, ROGUE_SMP_LOOKUP_MS_NUM_BITS 3 wide, with sno set) and
	 * src/imagination/include/hwdef/rogue_hw_defs.h.
	 */
	if (shape.multisampled) {
		error = shader_spirv_operand_word(reader, ids[TEXTURE_SAMPLE], 0, &word);
		if (error != 0)
			return error;
		word = shader_spirv_plain(reader, word);
		k = shader_spirv_constant_operand(7);
		word = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &word, &k, NULL);
		k = shader_spirv_constant_operand(16);
		word = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_LEFT, &word, &k, NULL);
		if ((flags & POWERVR_IR_TEXTURE_OFFSETS) != 0) {
			data[count - 1U] = shader_spirv_emit_value(reader, POWERVR_IR_BIT_OR, &data[count - 1U], &word, NULL);
		} else {
			if (count >= TEXTURE_DATA_MAX)
				return ENOTSUP;
			data[count++] = word;
		}
		flags |= POWERVR_IR_TEXTURE_SAMPLE_NUMBER;
	}
	if (count > POWERVR_IR_SOURCES)
		return ENOTSUP;

	/* The sampler: the binding's, or for a fetch a point sampler of the program's own words. */
	if (fetch) {
		error = point_sampler_request(reader, &sampler_request);
	} else {
		error = shader_spirv_request_element(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_SAMPLER_STATE, image->set, image->binding, image->offset,
						     POWERVR_SHADER_STATE_WORDS, &sampler_request);
	}
	if (error != 0)
		return error;
	if (image_request > 0xffU || sampler_request > 0xffU)
		return ENOTSUP;

	/* The lookup into a four-word value, integers when the image's are. */
	if (shape.integer && !compared)
		flags |= POWERVR_IR_TEXTURE_INTEGER;
	sampled = shader_spirv_new_value(reader, 4);
	shader_spirv_emit(reader, POWERVR_IR_TEXTURE, sampled, NULL, NULL, NULL, image_request | sampler_request << 8,
			  POWERVR_IR_TEXTURE_FORM(shape.dimensions, lod, flags, count));
	if (reader->shader->instruction_count != 0 && reader->error == 0) {
		for (index = 0; index < count; index++)
			reader->shader->instructions[reader->shader->instruction_count - 1U].source[index] = data[index];
	}

	/* A plain lookup is its words. */
	if (!compared) {
		operand_count = shader_spirv_scalar_count(reader, words[1]);
		if (operand_count == 0 || operand_count > 4)
			return ENOTSUP;
		error = shader_spirv_result_begin(reader, words[2], words[1], operand_count, &operands);
		if (error != 0)
			return error;
		for (index = 0; index < operand_count; index++)
			operands[index] = shader_spirv_value_operand(sampled, index);
		return reader->error;
	}

	/* A compared one is one or zero (the reference divided too, for a projection). */
	error = shader_spirv_operand_word(reader, words[5], 0, &reference);
	if (error != 0)
		return error;
	if (projected)
		reference = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MULTIPLY, &reference, &divisor, NULL);
	error = shader_spirv_result_begin(reader, words[2], words[1], 1, &result);
	if (error != 0)
		return error;
	error = compare_depth(reader, image, shader_spirv_value_operand(sampled, 0), reference, &result[0]);
	if (error != 0)
		return error;

	/* Reports what the lookup came to. */
	return reader->error;
}

/* Works out the shape of a lookup's image from its type: its coordinates, whether it is a cube or arrayed, and whether its texels are integers. */
static int
texture_shape_of(
	struct shader_spirv_reader *reader,
	const struct shader_spirv_id *image,
	struct texture_shape *shape)
{
	const struct shader_spirv_id *type;

	/* The image type, through a sampled image type. */
	if (image->type >= reader->bound)
		return EINVAL;
	type = &reader->ids[image->type];
	if (type->type_kind == TYPE_SAMPLED_IMAGE) {
		if (type->type >= reader->bound)
			return EINVAL;
		type = &reader->ids[type->type];
	}
	if (type->type_kind != TYPE_IMAGE || type->type >= reader->bound)
		return ENOTSUP;

	/* One, two or three coordinates by its Dim (a cube's direction three); not a rectangle, a buffer or an input attachment. */
	shape->cube = 0;
	switch (type->dimension) {
	case DIM_1D:
		shape->dimensions = 1;
		break;
	case DIM_2D:
		shape->dimensions = 2;
		break;
	case DIM_3D:
		shape->dimensions = 3;
		break;
	case DIM_CUBE:
		shape->dimensions = 3;
		shape->cube = 1;
		break;
	default:
		return ENOTSUP;
	}

	/* Arrays of one- and two-dimensional images (not of cubes or volumes), samples of flat ones, and the texels' kind. */
	shape->arrayed = type->arrayed;
	if (shape->arrayed && shape->dimensions == 3)
		return ENOTSUP;
	shape->multisampled = type->multisampled;
	if (shape->multisampled && (shape->dimensions != 2 || shape->cube))
		return ENOTSUP;
	shape->integer = reader->ids[type->type].type_kind == TYPE_INT;

	/* Succeeded: the shape. */
	return 0;
}

/*
 * Collects the ids of a lookup's image operands by the bits of the
 * mask, in the order the bits come; the bits that take no id (the
 * memory model's and the extension's) are let through, the rest
 * refused.
 */
static int
texture_operands(
	const uint32_t *words,
	uint32_t length,
	uint32_t at,
	uint32_t mask,
	uint32_t ids[TEXTURE_OPERAND_IDS])
{
	uint32_t index;

	/* None yet. */
	for (index = 0; index < TEXTURE_OPERAND_IDS; index++)
		ids[index] = 0;
	if ((mask & ~(IMAGE_OPERAND_BIAS | IMAGE_OPERAND_LOD | IMAGE_OPERAND_GRAD | IMAGE_OPERAND_CONST_OFFSET | IMAGE_OPERAND_OFFSET | IMAGE_OPERAND_SAMPLE |
		      IMAGE_OPERANDS_WITHOUT_IDS)) != 0)
		return ENOTSUP;

	/* Each operand's id in the order of the bits; a gradient has two. */
	if ((mask & IMAGE_OPERAND_BIAS) != 0) {
		if (at >= length)
			return EINVAL;
		ids[TEXTURE_BIAS] = words[at++];
	}
	if ((mask & IMAGE_OPERAND_LOD) != 0) {
		if (at >= length)
			return EINVAL;
		ids[TEXTURE_LOD] = words[at++];
	}
	if ((mask & IMAGE_OPERAND_GRAD) != 0) {
		if (at + 1U >= length)
			return EINVAL;
		ids[TEXTURE_GRADIENT_X] = words[at++];
		ids[TEXTURE_GRADIENT_Y] = words[at++];
	}
	if ((mask & IMAGE_OPERAND_CONST_OFFSET) != 0) {
		if (at >= length)
			return EINVAL;
		ids[TEXTURE_CONSTANT_OFFSET] = words[at++];
	}
	if ((mask & IMAGE_OPERAND_OFFSET) != 0) {
		if (at >= length)
			return EINVAL;
		ids[TEXTURE_OFFSET] = words[at++];
	}
	if ((mask & IMAGE_OPERAND_SAMPLE) != 0) {
		if (at >= length)
			return EINVAL;
		ids[TEXTURE_SAMPLE] = words[at++];
	}

	/* Succeeded: the ids. */
	return 0;
}

/*
 * Works out the last texel a fetch may read along each of an image's
 * dimensions at its level: the view's first level's edges in the image
 * state words, shifted down by the level and the view's base level (a
 * strided image has neither levels nor a base), at least one, less one.
 * A fetch outside them is undefined in GL and Vulkan; the texture unit
 * does not keep a strided image's fetch inside the image (it read past
 * it, out of the session's memory), so every fetch is clamped to them,
 * as Vulkan's robustImageAccess keeps an access inside the image.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/csbgen/rogue/texstate.xml (IMAGE_WORD0:
 * textype 0-2, width 34-47, height 48-61; IMAGE_WORD1: depth 4-14,
 * baselevel 60-63; TEXTYPE STRIDE 4) and src/imagination/pco/
 * pco_nir_tex.c (integer coordinates are not clamped by the hardware
 * when a level is given).
 */
static int
texture_fetch_last(
	struct shader_spirv_reader *reader,
	const struct shader_spirv_id *image,
	uint32_t lod_id,
	uint32_t dimensions,
	struct powervr_ir_operand last[3])
{
	struct powervr_ir_operand state[POWERVR_SHADER_STATE_WORDS];
	struct powervr_ir_operand level;
	struct powervr_ir_operand base;
	struct powervr_ir_operand strided;
	struct powervr_ir_operand edge;
	struct powervr_ir_operand k;
	uint32_t image_request;
	uint32_t index;
	int error;

	/* The image state words, and the level (0 unless named) from the view's first. */
	error = shader_spirv_request_element(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_IMAGE_STATE, image->set, image->binding, image->offset,
					     POWERVR_SHADER_STATE_WORDS, &image_request);
	if (error != 0)
		return error;
	for (index = 0; index < POWERVR_SHADER_STATE_WORDS; index++)
		state[index] = shader_spirv_shared_word(reader, image_request, index);
	level = shader_spirv_constant_operand(0);
	if (lod_id != 0) {
		error = shader_spirv_operand_word(reader, lod_id, 0, &level);
		if (error != 0)
			return error;
		level = shader_spirv_plain(reader, level);
	}
	k = shader_spirv_constant_operand(28);
	base = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_RIGHT, &state[3], &k, NULL);
	k = shader_spirv_constant_operand(7);
	strided = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &state[0], &k, NULL);
	k = shader_spirv_constant_operand(TEXTURE_TYPE_STRIDE);
	strided = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_EQUAL, &strided, &k, NULL);
	k = shader_spirv_constant_operand(0);
	base = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &strided, &k, &base);
	level = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &level, &base, NULL);

	/* Each dimension's edge less one, plus one, shifted, at least one, less one. */
	for (index = 0; index < dimensions && index < 3U; index++) {
		if (index == 2U) {
			k = shader_spirv_constant_operand(4);
			edge = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_RIGHT, &state[2], &k, NULL);
			k = shader_spirv_constant_operand(0x7ffU);
		} else {
			k = shader_spirv_constant_operand(index == 0U ? 2U : 16U);
			edge = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_RIGHT, &state[1], &k, NULL);
			k = shader_spirv_constant_operand(0x3fffU);
		}
		edge = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &edge, &k, NULL);
		k = shader_spirv_constant_operand(1);
		edge = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &edge, &k, NULL);
		edge = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_RIGHT, &edge, &level, NULL);
		edge = shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_MAXIMUM, &edge, &k, NULL);
		k = shader_spirv_constant_operand(0xffffffffU);
		last[index] = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &edge, &k, NULL);
	}

	/* Reports what the edges came to. */
	return reader->error;
}

/*
 * Works out the address of an array layer's texels: the layer (a
 * float's nearest whole number, or a fetch's integer) clamped to the
 * view's layers, times the view's layer size, added to the address the
 * image state words hold.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/csbgen/rogue/texstate.xml (IMAGE_WORD0:
 * textype 0-2, STRIDE 4; IMAGE_WORD1: depth 4-14, texaddr 16-53 shifted
 * by 2; STRIDE_IMAGE_WORD1: stride 0-14); the Khronos OpenGL ES 3.0
 * specification, section 3.8.10 (the layer is floor(layer + 0.5)
 * clamped to [0, layers - 1]).
 */
static int
texture_layer_address(
	struct shader_spirv_reader *reader,
	const struct shader_spirv_id *image,
	uint32_t image_request,
	struct powervr_ir_operand layer,
	uint32_t fetch,
	struct powervr_ir_operand address[2])
{
	struct powervr_ir_operand low;
	struct powervr_ir_operand high;
	struct powervr_ir_operand last;
	struct powervr_ir_operand strided;
	struct powervr_ir_operand offset;
	struct powervr_ir_operand carry;
	struct powervr_ir_operand k;
	struct powervr_ir_operand t;
	uint32_t layer_request;
	int error;

	/* The layer as a whole number. */
	if (fetch) {
		layer = shader_spirv_plain(reader, layer);
	} else {
		k = shader_spirv_constant_operand(FLOAT_HALF);
		layer = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_ADD, &layer, &k, NULL);
		layer = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_FLOOR, &layer, NULL, NULL);
		layer = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_TO_SIGNED, &layer, NULL, NULL);
	}

	/*
	 * Clamped to the view's last layer: the image state's layers less
	 * one, or 0 for a strided image, which has one layer and whose second
	 * word holds its row where a twiddled image's holds the layers.
	 */
	low = shader_spirv_shared_word(reader, image_request, 2);
	high = shader_spirv_shared_word(reader, image_request, 3);
	k = shader_spirv_constant_operand(4);
	last = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_RIGHT, &low, &k, NULL);
	k = shader_spirv_constant_operand(0x7ffU);
	last = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &last, &k, NULL);
	strided = shader_spirv_shared_word(reader, image_request, 0);
	k = shader_spirv_constant_operand(7);
	strided = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &strided, &k, NULL);
	k = shader_spirv_constant_operand(TEXTURE_TYPE_STRIDE);
	strided = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_EQUAL, &strided, &k, NULL);
	k = shader_spirv_constant_operand(0);
	last = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &strided, &k, &last);
	layer = shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_MAXIMUM, &layer, &k, NULL);
	layer = shader_spirv_emit_value(reader, POWERVR_IR_SIGNED_MINIMUM, &layer, &last, NULL);

	/* Its byte offset. */
	error = shader_spirv_request_element(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_IMAGE_LAYER_BYTES, image->set, image->binding, image->offset, 1,
					     &layer_request);
	if (error != 0)
		return error;
	t = shader_spirv_shared_word(reader, layer_request, 0);
	offset = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &layer, &t, NULL);

	/* The image's address: bits 16 to 53 of the second state word, in four-byte units. */
	k = shader_spirv_constant_operand(14);
	low = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_RIGHT, &low, &k, NULL);
	k = shader_spirv_constant_operand(~3U);
	low = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &low, &k, NULL);
	k = shader_spirv_constant_operand(18);
	t = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_LEFT, &high, &k, NULL);
	low = shader_spirv_emit_value(reader, POWERVR_IR_BIT_OR, &low, &t, NULL);
	k = shader_spirv_constant_operand(14);
	high = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_RIGHT, &high, &k, NULL);
	k = shader_spirv_constant_operand(0xffU);
	high = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &high, &k, NULL);

	/* The layer's: the offset added, with its carry. */
	low = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &low, &offset, NULL);
	carry = shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_LESS, &low, &offset, NULL);
	k = shader_spirv_constant_operand(1);
	carry = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &carry, &k, NULL);
	high = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &high, &carry, NULL);
	address[0] = low;
	address[1] = high;

	/* Reports what the address came to. */
	return reader->error;
}

/*
 * Packs a lookup's texel offsets into the word the core reads: u in
 * bits 0 to 5, v in 6 to 11, w in 12 to 15, each two's complement.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/include/hwdef/rogue_hw_defs.h
 * (ROGUE_SMP_LOOKUP_U_OFF_START 0 and _BITS 6, V 6 and 6, W 12 and 4).
 */
static int
texture_offsets(
	struct shader_spirv_reader *reader,
	uint32_t id,
	uint32_t dimensions,
	struct powervr_ir_operand *packed)
{
	static const uint32_t starts[3] = { 0, 6, 12 };
	static const uint32_t masks[3] = { 0x3fU, 0x3fU, 0xfU };
	struct powervr_ir_operand *offsets;
	struct powervr_ir_operand word;
	struct powervr_ir_operand k;
	uint32_t count;
	uint32_t index;
	uint32_t constant;
	int error;

	/* One offset a coordinate. */
	error = shader_spirv_id_operands(reader, id, &offsets, &count);
	if (error != 0)
		return error;
	if (count < dimensions)
		return EINVAL;

	/* Constants packed here, else masked, shifted and joined. */
	constant = 0;
	*packed = shader_spirv_constant_operand(0);
	for (index = 0; index < dimensions; index++) {
		if (offsets[index].kind == POWERVR_IR_OPERAND_CONSTANT) {
			constant |= (offsets[index].constant & masks[index]) << starts[index];
			continue;
		}
		word = shader_spirv_plain(reader, offsets[index]);
		k = shader_spirv_constant_operand(masks[index]);
		word = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &word, &k, NULL);
		k = shader_spirv_constant_operand(starts[index]);
		word = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_LEFT, &word, &k, NULL);
		*packed = shader_spirv_emit_value(reader, POWERVR_IR_BIT_OR, packed, &word, NULL);
	}
	k = shader_spirv_constant_operand(constant);
	if (packed->kind == POWERVR_IR_OPERAND_CONSTANT) {
		*packed = k;
	} else if (constant != 0) {
		*packed = shader_spirv_emit_value(reader, POWERVR_IR_BIT_OR, packed, &k, NULL);
	}

	/* Reports what the packing came to. */
	return reader->error;
}

/* The point sampler's state words (point_sampler_request names their sources). */
const uint32_t shader_spirv_point_sampler_words[POWERVR_SHADER_STATE_WORDS] = {
	0xdf800fffU,
	0x04004801U,
	0x00000000U,
	0x00000000U
};

/*
 * Finds or adds the four literal words of the point sampler a fetch
 * reads: nearest filters, no mip filter, clamped to the border, the
 * largest level of detail open, no anisotropy.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/csbgen/rogue/texstate.xml
 * (SAMPLER_WORD0: dadjust 0-12, minlod 13-22, maxlod 23-32, anisoctl
 * 33-35, magfilter 36-37, minfilter 38-39, mipfilter 40, addrmode_u
 * 41-43, addrmode_v 44-46, addrmode_w 56-58; ADDRMODE CLAMP_TO_BORDER 4,
 * DADJUST ZERO_UINT 4095, CLAMP MAX 959, FILTER POINT 0; SAMPLER_WORD1
 * zero) and src/imagination/vulkan/pvr_arch_cmd_buffer.c (the point
 * sampler's words).
 */
static int
point_sampler_request(
	struct shader_spirv_reader *reader,
	uint32_t *index)
{
	struct powervr_shader_program *program;
	struct powervr_shader_request *request;
	uint32_t at;
	uint32_t word;

	/* The four words already asked for, one after another. */
	program = reader->program;
	for (at = 0; at + POWERVR_SHADER_STATE_WORDS <= program->request_count; at++) {
		for (word = 0; word < POWERVR_SHADER_STATE_WORDS; word++) {
			request = &program->requests[at + word];
			if (request->kind != POWERVR_SHADER_REQUEST_LITERAL || request->words != 1 || request->literal != shader_spirv_point_sampler_words[word])
				break;
		}
		if (word == POWERVR_SHADER_STATE_WORDS) {
			*index = at;
			return 0;
		}
	}

	/* New ones, if the table has room. */
	if (program->request_count + POWERVR_SHADER_STATE_WORDS > POWERVR_SHADER_REQUESTS)
		return ENOTSUP;
	*index = program->request_count;
	for (word = 0; word < POWERVR_SHADER_STATE_WORDS; word++) {
		request = &program->requests[program->request_count++];
		request->kind = POWERVR_SHADER_REQUEST_LITERAL;
		request->part = 0;
		request->words = 1;
		request->set = 0;
		request->binding = 0;
		request->array_index = 0;
		request->offset = 0;
		request->literal = shader_spirv_point_sampler_words[word];
	}

	/* Succeeded: the first of the four. */
	return 0;
}

/*
 * Compares a reference with the depth a lookup returned (its first
 * word): one when the sampler's comparison, a VkCompareOp whose bits are
 * less, equal and greater, holds of reference and depth, else zero.  The
 * reference is clamped to [0, 1] first when the view's format is
 * unsigned normalized (Vulkan's depth compare operation).
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_nir_tex.c (lower_tex: a shadow
 * sample as a plain sample, the comparator saturated when the image
 * meta's pack information has its unorm bit, then each channel tested
 * against the reference by the comparison the sampler's descriptor
 * keeps).
 */
static int
compare_depth(
	struct shader_spirv_reader *reader,
	const struct shader_spirv_id *image,
	struct powervr_ir_operand depth,
	struct powervr_ir_operand reference,
	struct powervr_ir_operand *result)
{
	struct powervr_ir_operand passes;
	struct powervr_ir_operand test;
	struct powervr_ir_operand bit;
	struct powervr_ir_operand operation;
	struct powervr_ir_operand one;
	struct powervr_ir_operand zero;
	struct powervr_ir_operand unorm;
	struct powervr_ir_operand clamped;
	uint32_t compare_request;
	uint32_t pack_request;
	uint32_t index;
	int error;

	/* The reference, clamped to [0, 1] for an unsigned normalized view. */
	error = shader_spirv_request_element(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_IMAGE_PACK_INFO, image->set, image->binding, image->offset, 1,
					     &pack_request);
	if (error != 0)
		return error;
	unorm = shader_spirv_shared_word(reader, pack_request, 0);
	bit = shader_spirv_constant_operand(POWERVR_SHADER_PACK_INFO_UNORM);
	unorm = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &unorm, &bit, NULL);
	zero = shader_spirv_constant_operand(FLOAT_ZERO);
	one = shader_spirv_constant_operand(FLOAT_ONE);
	clamped = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MAXIMUM, &reference, &zero, NULL);
	clamped = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_MINIMUM, &clamped, &one, NULL);
	reference = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &unorm, &clamped, &reference);

	/* The sampler's comparison. */
	error = shader_spirv_request_element(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_SAMPLER_COMPARE, image->set, image->binding, image->offset, 1,
					     &compare_request);
	if (error != 0)
		return error;

	/* The bits of less, equal and greater that hold, kept where the comparison has them. */
	passes = shader_spirv_constant_operand(0);
	for (index = 0; index < 3; index++) {
		if (index == 0)
			test = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LESS, &reference, &depth, NULL);
		else if (index == 1)
			test = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_EQUAL, &reference, &depth, NULL);
		else
			test = shader_spirv_emit_value(reader, POWERVR_IR_FLOAT_LESS, &depth, &reference, NULL);
		bit = shader_spirv_constant_operand(1U << index);
		test = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &test, &bit, NULL);
		passes = shader_spirv_emit_value(reader, POWERVR_IR_BIT_OR, &passes, &test, NULL);
	}
	operation = shader_spirv_shared_word(reader, compare_request, 0);
	passes = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &passes, &operation, NULL);
	one = shader_spirv_constant_operand(FLOAT_ONE);
	zero = shader_spirv_constant_operand(FLOAT_ZERO);
	*result = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &passes, &one, &zero);

	/* Reports what the comparison came to. */
	return reader->error;
}

/*
 * Reads the size of a combined image at a level (0 without one): each
 * edge of the view's first level shifted down by the level, at least
 * one, and an array's layers, from the image state words.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/pco/usclib/tex.cl
 * (usclib_tex_state_size: the edges plus one, the level plus the base
 * level, each edge but the layers shifted and at least one).
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/csbgen/rogue/texstate.xml (IMAGE_WORD0:
 * textype 0-2, width 34-47, height 48-61; IMAGE_WORD1: depth 4-14,
 * baselevel 60-63; TEXTYPE STRIDE 4, whose second word has no base
 * level and no layers: STRIDE_IMAGE_WORD1 stride 0-14).
 */
static int
reader_texture_size(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length)
{
	struct powervr_ir_operand *result;
	struct powervr_ir_operand state[POWERVR_SHADER_STATE_WORDS];
	struct powervr_ir_operand edges[3];
	struct powervr_ir_operand level;
	struct powervr_ir_operand base;
	struct powervr_ir_operand strided;
	struct powervr_ir_operand edge;
	struct powervr_ir_operand k;
	struct shader_spirv_id *image;
	struct texture_shape shape;
	uint32_t image_request;
	uint32_t opcode;
	uint32_t count;
	uint32_t index;
	uint32_t shifted;
	int error;

	/* The image and the level. */
	opcode = words[0] & 0xffffU;
	if (length < 4 || (opcode == OP_IMAGE_QUERY_SIZE_LOD && length < 5))
		return EINVAL;
	if (words[1] >= reader->bound || words[2] >= reader->bound || words[3] >= reader->bound)
		return EINVAL;
	image = &reader->ids[words[3]];
	if (image->kind != ID_SAMPLED_IMAGE)
		return ENOTSUP;
	error = texture_shape_of(reader, image, &shape);
	if (error != 0)
		return error;
	level = shader_spirv_constant_operand(0);
	if (opcode == OP_IMAGE_QUERY_SIZE_LOD) {
		error = shader_spirv_operand_word(reader, words[4], 0, &level);
		if (error != 0)
			return error;
		level = shader_spirv_plain(reader, level);
	}

	/* The image state words. */
	error = shader_spirv_request_element(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_IMAGE_STATE, image->set, image->binding, image->offset,
					     POWERVR_SHADER_STATE_WORDS, &image_request);
	if (error != 0)
		return error;
	for (index = 0; index < POWERVR_SHADER_STATE_WORDS; index++)
		state[index] = shader_spirv_shared_word(reader, image_request, index);

	/* The level from the view's first (a strided image has one level and no base). */
	k = shader_spirv_constant_operand(28);
	base = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_RIGHT, &state[3], &k, NULL);
	k = shader_spirv_constant_operand(7);
	strided = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &state[0], &k, NULL);
	k = shader_spirv_constant_operand(TEXTURE_TYPE_STRIDE);
	strided = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_EQUAL, &strided, &k, NULL);
	k = shader_spirv_constant_operand(0);
	base = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &strided, &k, &base);
	level = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &level, &base, NULL);

	/*
	 * The edges less one: width and height in the first word, depth or
	 * layers in the second (0 for a strided image: one layer, its second
	 * word holding its row there).
	 */
	k = shader_spirv_constant_operand(2);
	edges[0] = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_RIGHT, &state[1], &k, NULL);
	k = shader_spirv_constant_operand(16);
	edges[1] = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_RIGHT, &state[1], &k, NULL);
	k = shader_spirv_constant_operand(0x3fffU);
	edges[0] = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &edges[0], &k, NULL);
	edges[1] = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &edges[1], &k, NULL);
	k = shader_spirv_constant_operand(4);
	edges[2] = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_RIGHT, &state[2], &k, NULL);
	k = shader_spirv_constant_operand(0x7ffU);
	edges[2] = shader_spirv_emit_value(reader, POWERVR_IR_BIT_AND, &edges[2], &k, NULL);
	k = shader_spirv_constant_operand(0);
	edges[2] = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &strided, &k, &edges[2]);

	/* The result's words: the shape's edges (a cube's two), then an array's layers. */
	count = shader_spirv_scalar_count(reader, words[1]);
	shifted = shape.cube ? 2U : shape.dimensions;
	if (count != shifted + shape.arrayed)
		return ENOTSUP;
	error = shader_spirv_result_begin(reader, words[2], words[1], count, &result);
	if (error != 0)
		return error;
	for (index = 0; index < count; index++) {
		k = shader_spirv_constant_operand(1);
		edge = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, index < shifted ? &edges[index] : &edges[2], &k, NULL);
		if (index < shifted) {
			edge = shader_spirv_emit_value(reader, POWERVR_IR_SHIFT_RIGHT, &edge, &level, NULL);
			edge = shader_spirv_emit_value(reader, POWERVR_IR_UNSIGNED_MAXIMUM, &edge, &k, NULL);
		}
		result[index] = edge;
	}

	/* Reports what the size came to. */
	return reader->error;
}

/* Reports whether two operands read the same word the same way. */
static int
same_operand(
	const struct powervr_ir_operand *a,
	const struct powervr_ir_operand *b)
{
	/* The kind and modifiers, then the value's word or the constant. */
	if (a->kind != b->kind || a->modifiers != b->modifiers)
		return 0;
	if (a->kind == POWERVR_IR_OPERAND_VALUE)
		return a->value == b->value && a->component == b->component;
	if (a->kind == POWERVR_IR_OPERAND_CONSTANT)
		return a->constant == b->constant;
	return 1;
}

/*
 * Reads OpGroupNonUniformElect of the subgroup scope: true, since every
 * invocation is the only one, and so the first, of its subgroup (a
 * subgroup is one invocation).  Another scope is refused.
 */
static int
reader_elect(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length)
{
	struct powervr_ir_operand *operands;
	struct powervr_ir_operand scope;
	int error;

	/* The result type, the result and the execution scope, a constant. */
	if (length != 4 || words[2] >= reader->bound)
		return EINVAL;
	error = shader_spirv_operand_word(reader, words[3], 0, &scope);
	if (error != 0)
		return error;
	if (scope.kind != POWERVR_IR_OPERAND_CONSTANT || scope.constant != SCOPE_SUBGROUP)
		return ENOTSUP;

	/* The result is the Boolean true. */
	error = shader_spirv_result_begin(reader, words[2], words[1], 1, &operands);
	if (error != 0)
		return error;
	operands[0] = shader_spirv_constant_operand(TRUE_WORD);

	/* Succeeded: the result is true. */
	return reader->error;
}

/*
 * Reads OpControlBarrier.  Within one task the instances run together
 * and a store is done before the next instruction, so a workgroup of
 * one task needs nothing; a larger one meets at a counter and a
 * sequence in its workgroup memory, under the barrier mutex: the first
 * instance of each task counts the task in, the last task to arrive
 * starts the next sequence, and the others sleep until it does.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_nir_sync.c (lower_barrier: no
 * code for a workgroup of one task, else usclib_barrier with the tasks'
 * count and two words of workgroup memory) and src/imagination/pco/
 * usclib/sync.cl (usclib_barrier).
 */
static int
reader_barrier(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length)
{
	struct powervr_ir_operand address[3];
	struct powervr_ir_operand counter_at;
	struct powervr_ir_operand sequence_at;
	struct powervr_ir_operand instance;
	struct powervr_ir_operand first;
	struct powervr_ir_operand sequence;
	struct powervr_ir_operand counter;
	struct powervr_ir_operand done;
	struct powervr_ir_operand now;
	struct powervr_ir_operand word;
	struct powervr_ir_operand zero;
	struct powervr_ir_operand one;
	struct powervr_ir_operand tasks;
	struct powervr_ir_operand scope;
	uint32_t request;
	uint32_t invocations;
	int error;

	/* The execution scope: only a workgroup's makes the instances wait. */
	if (length < 4)
		return EINVAL;
	error = shader_spirv_operand_word(reader, words[1], 0, &scope);
	if (error != 0)
		return error;
	if (scope.kind != POWERVR_IR_OPERAND_CONSTANT)
		return ENOTSUP;
	if (scope.constant != SCOPE_WORKGROUP || reader->stage != POWERVR_SHADER_COMPUTE)
		return 0;

	/* A workgroup of one task needs no code. */
	invocations = reader->program->workgroup[0] * reader->program->workgroup[1] * reader->program->workgroup[2];
	if (invocations <= POWERVR_SHADER_TASK_INSTANCES)
		return 0;
	if (reader->barrier_offset == 0)
		return ENOTSUP;

	/* The counter's and the sequence's places in the workgroup's slice. */
	error = shader_spirv_request_add(reader, POWERVR_SHADER_REQUEST_SHARED_MEMORY, 0, 0, 0, 2, &request);
	if (error != 0)
		return error;
	address[0] = shader_spirv_shared_word(reader, request, 0);
	address[1] = shader_spirv_shared_word(reader, request, 1);
	word = shader_spirv_constant_operand(reader->barrier_offset);
	counter_at = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &reader->shared_slice, &word, NULL);
	word = shader_spirv_constant_operand(reader->barrier_offset + 4U);
	sequence_at = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &reader->shared_slice, &word, NULL);
	zero = shader_spirv_constant_operand(0);
	one = shader_spirv_constant_operand(1);
	tasks = shader_spirv_constant_operand((invocations + POWERVR_SHADER_TASK_INSTANCES - 1U) / POWERVR_SHADER_TASK_INSTANCES);

	/* The task's first instance counts it in, under the mutex. */
	instance = shader_spirv_emit_value(reader, POWERVR_IR_INSTANCE, NULL, NULL, NULL);
	first = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_EQUAL, &instance, &zero, NULL);
	shader_spirv_emit(reader, POWERVR_IR_MUTEX, 0, NULL, NULL, NULL, POWERVR_IR_MUTEX_BARRIER, POWERVR_IR_MUTEX_LOCK);
	sequence = memory_load(reader, address, &sequence_at, POWERVR_IR_BYPASS);
	shader_spirv_emit(reader, POWERVR_IR_IF, 0, &first, NULL, NULL, 0, 0);
	counter = memory_load(reader, address, &counter_at, POWERVR_IR_BYPASS);
	counter = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &counter, &one, NULL);
	memory_store(reader, address, &counter_at, counter);
	shader_spirv_emit(reader, POWERVR_IR_END_IF, 0, NULL, NULL, NULL, 0, 0);

	/* The last task in starts the next sequence; the others sleep until the sequence moves on. */
	counter = memory_load(reader, address, &counter_at, POWERVR_IR_BYPASS);
	done = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_EQUAL, &counter, &tasks, NULL);
	shader_spirv_emit(reader, POWERVR_IR_IF, 0, &done, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_IF, 0, &first, NULL, NULL, 0, 0);
	memory_store(reader, address, &counter_at, zero);
	word = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &sequence, &one, NULL);
	memory_store(reader, address, &sequence_at, word);
	shader_spirv_emit(reader, POWERVR_IR_END_IF, 0, NULL, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_ELSE, 0, NULL, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_LOOP, 0, NULL, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_MUTEX, 0, NULL, NULL, NULL, POWERVR_IR_MUTEX_BARRIER, POWERVR_IR_MUTEX_RELEASE_SLEEP);
	shader_spirv_emit(reader, POWERVR_IR_MUTEX, 0, NULL, NULL, NULL, POWERVR_IR_MUTEX_BARRIER, POWERVR_IR_MUTEX_LOCK);
	now = memory_load(reader, address, &sequence_at, POWERVR_IR_BYPASS);
	now = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_NOT_EQUAL, &now, &sequence, NULL);
	shader_spirv_emit(reader, POWERVR_IR_IF, 0, &now, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_BREAK, 0, NULL, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_END_IF, 0, NULL, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_END_LOOP, 0, NULL, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_END_IF, 0, NULL, NULL, NULL, 0, 0);

	/* The mutex goes, waking the sleepers. */
	shader_spirv_emit(reader, POWERVR_IR_MUTEX, 0, NULL, NULL, NULL, POWERVR_IR_MUTEX_BARRIER, POWERVR_IR_MUTEX_RELEASE_WAKEUP);
	return reader->error;
}

/*
 * Reads an atomic instruction on buffer memory (a storage buffer, or a
 * uniform block decorated as a buffer block) or workgroup memory: a load or
 * a store as they are (around the caches), a compare and exchange as a
 * loop under the atomic mutex, the rest as the core's atomic operation.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_trans_nir.c (trans_atomic_buffer
 * and to_atom_op) and src/imagination/pco/usclib/sync.cl
 * (usclib_emu_ssbo_atomic_comp_swap).
 */
static int
reader_atomic(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length)
{
	struct powervr_ir_operand address[3];
	struct powervr_ir_operand *result;
	struct powervr_ir_operand *values;
	struct powervr_ir_operand *comparators;
	struct powervr_ir_operand value;
	struct powervr_ir_operand all_ones;
	struct shader_spirv_id *pointer;
	uint32_t opcode;
	uint32_t pointer_id;
	uint32_t value_id;
	uint32_t count;
	uint32_t code;
	int error;

	/* The pointer: the third word, or the first of a store. */
	opcode = words[0] & 0xffffU;
	pointer_id = opcode == OP_ATOMIC_STORE ? words[1] : words[3];
	if (length < 4 || pointer_id >= reader->bound)
		return EINVAL;
	pointer = &reader->ids[pointer_id];
	if (pointer->kind != ID_POINTER ||
	    (pointer->storage != STORAGE_STORAGE_BUFFER && pointer->storage != STORAGE_UNIFORM && pointer->storage != STORAGE_WORKGROUP))
		return ENOTSUP;
	if (shader_spirv_scalar_count(reader, pointer->type) != 1)
		return ENOTSUP;
	error = reader_memory_address(reader, pointer, address);
	if (error != 0)
		return error;
	reader->program->stores = 1;

	/* A store: the value, the fourth word. */
	if (opcode == OP_ATOMIC_STORE) {
		if (length < 5)
			return EINVAL;
		error = shader_spirv_id_operands(reader, words[4], &values, &count);
		if (error != 0 || count != 1)
			return EINVAL;
		memory_store(reader, address, &address[2], values[0]);
		return reader->error;
	}

	/* The rest answer one word. */
	error = shader_spirv_result_begin(reader, words[2], words[1], 1, &result);
	if (error != 0)
		return error;
	if (opcode == OP_ATOMIC_LOAD) {
		result[0] = memory_load(reader, address, &address[2], POWERVR_IR_BYPASS);
		return reader->error;
	}

	/* The value: the seventh word, the one after the two semantics of a compare and exchange, none for a step. */
	all_ones = shader_spirv_constant_operand(TRUE_WORD);
	value = shader_spirv_constant_operand(1);
	if (opcode == OP_ATOMIC_I_DECREMENT)
		value = all_ones;
	if (opcode != OP_ATOMIC_I_INCREMENT && opcode != OP_ATOMIC_I_DECREMENT) {
		value_id = opcode == OP_ATOMIC_COMPARE_EXCHANGE ? 7U : 6U;
		if (length <= value_id)
			return EINVAL;
		error = shader_spirv_id_operands(reader, words[value_id], &values, &count);
		if (error != 0 || count != 1)
			return EINVAL;
		value = shader_spirv_plain(reader, values[0]);
	}

	/* A compare and exchange: the comparator, the eighth word. */
	if (opcode == OP_ATOMIC_COMPARE_EXCHANGE) {
		if (length < 9)
			return EINVAL;
		error = shader_spirv_id_operands(reader, words[8], &comparators, &count);
		if (error != 0 || count != 1)
			return EINVAL;
		result[0] = compare_exchange(reader, address, value, shader_spirv_plain(reader, comparators[0]));
		return reader->error;
	}

	/* The core's code; a subtraction adds the negation. */
	switch (opcode) {
	case OP_ATOMIC_EXCHANGE:
		code = POWERVR_IR_ATOMIC_EXCHANGE;
		break;
	case OP_ATOMIC_I_SUB:
		value = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_MULTIPLY, &value, &all_ones, NULL);
		code = POWERVR_IR_ATOMIC_ADD;
		break;
	case OP_ATOMIC_S_MIN:
		code = POWERVR_IR_ATOMIC_SIGNED_MIN;
		break;
	case OP_ATOMIC_U_MIN:
		code = POWERVR_IR_ATOMIC_UNSIGNED_MIN;
		break;
	case OP_ATOMIC_S_MAX:
		code = POWERVR_IR_ATOMIC_SIGNED_MAX;
		break;
	case OP_ATOMIC_U_MAX:
		code = POWERVR_IR_ATOMIC_UNSIGNED_MAX;
		break;
	case OP_ATOMIC_AND:
		code = POWERVR_IR_ATOMIC_AND;
		break;
	case OP_ATOMIC_OR:
		code = POWERVR_IR_ATOMIC_OR;
		break;
	case OP_ATOMIC_XOR:
		code = POWERVR_IR_ATOMIC_XOR;
		break;
	default:
		/* The add, the increment and the decrement. */
		code = POWERVR_IR_ATOMIC_ADD;
		break;
	}

	/* The atomic, its data the fourth source. */
	result[0] = shader_spirv_value_operand(shader_spirv_new_value(reader, 1), 0);
	shader_spirv_emit(reader, POWERVR_IR_ATOMIC, result[0].value, &address[0], &address[1], &address[2], code, 0);
	if (reader->error == 0)
		reader->shader->instructions[reader->shader->instruction_count - 1U].source[3] = value;
	return reader->error;
}

/* Loads one memory word at the address pair plus the byte offset, with the load's flags (POWERVR_IR_BYPASS). */
static struct powervr_ir_operand
memory_load(
	struct shader_spirv_reader *reader,
	const struct powervr_ir_operand *address,
	const struct powervr_ir_operand *offset,
	uint32_t flags)
{
	struct powervr_ir_operand word;

	/* A new value the load writes. */
	word = shader_spirv_value_operand(shader_spirv_new_value(reader, 1), 0);
	shader_spirv_emit(reader, POWERVR_IR_LOAD, word.value, &address[0], &address[1], offset, 0, 0);
	if (reader->error == 0)
		reader->shader->instructions[reader->shader->instruction_count - 1U].flags = (uint8_t)flags;
	return word;
}

/* Stores one word at the address pair plus the byte offset. */
static void
memory_store(
	struct shader_spirv_reader *reader,
	const struct powervr_ir_operand *address,
	const struct powervr_ir_operand *offset,
	struct powervr_ir_operand word)
{
	struct powervr_ir_operand plain;

	/* The word without modifiers, as the store's fourth source. */
	plain = shader_spirv_plain(reader, word);
	shader_spirv_emit(reader, POWERVR_IR_STORE, 0, &address[0], &address[1], offset, 0, 0);
	if (reader->error == 0)
		reader->shader->instructions[reader->shader->instruction_count - 1U].source[3] = plain;
}

/*
 * Compares a memory word with the comparator and exchanges it for the
 * value when they are equal, answering the word as it was: the core has
 * no such atomic, so under the atomic mutex each instance of the task
 * in turn loads, compares and stores.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/pco/usclib/sync.cl
 * (usclib_emu_ssbo_atomic_comp_swap).
 */
static struct powervr_ir_operand
compare_exchange(
	struct shader_spirv_reader *reader,
	const struct powervr_ir_operand *address,
	struct powervr_ir_operand value,
	struct powervr_ir_operand comparator)
{
	struct powervr_ir_operand result;
	struct powervr_ir_operand turn;
	struct powervr_ir_operand instance;
	struct powervr_ir_operand mine;
	struct powervr_ir_operand old;
	struct powervr_ir_operand same;
	struct powervr_ir_operand stored;
	struct powervr_ir_operand last;
	struct powervr_ir_operand zero;
	struct powervr_ir_operand one;
	struct powervr_ir_operand instances;

	zero = shader_spirv_constant_operand(0);
	one = shader_spirv_constant_operand(1);
	instances = shader_spirv_constant_operand(POWERVR_SHADER_TASK_INSTANCES);

	/* The answer and the turn, each a value written in the loop. */
	result = shader_spirv_value_operand(shader_spirv_new_value(reader, 1), 0);
	shader_spirv_emit(reader, POWERVR_IR_MOVE, result.value, &zero, NULL, NULL, 0, 0);
	turn = shader_spirv_value_operand(shader_spirv_new_value(reader, 1), 0);
	shader_spirv_emit(reader, POWERVR_IR_MOVE, turn.value, &zero, NULL, NULL, 0, 0);
	instance = shader_spirv_emit_value(reader, POWERVR_IR_INSTANCE, NULL, NULL, NULL);

	/* Under the mutex, each instance in its turn. */
	shader_spirv_emit(reader, POWERVR_IR_MUTEX, 0, NULL, NULL, NULL, POWERVR_IR_MUTEX_ATOMIC, POWERVR_IR_MUTEX_LOCK);
	shader_spirv_emit(reader, POWERVR_IR_LOOP, 0, NULL, NULL, NULL, 0, 0);
	mine = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_EQUAL, &instance, &turn, NULL);
	shader_spirv_emit(reader, POWERVR_IR_IF, 0, &mine, NULL, NULL, 0, 0);
	old = memory_load(reader, address, &address[2], POWERVR_IR_BYPASS);
	same = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_EQUAL, &old, &comparator, NULL);
	stored = shader_spirv_emit_value(reader, POWERVR_IR_SELECT, &same, &value, &old);
	memory_store(reader, address, &address[2], stored);
	shader_spirv_emit(reader, POWERVR_IR_MOVE, result.value, &old, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_END_IF, 0, NULL, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_INTEGER_ADD, turn.value, &turn, &one, NULL, 0, 0);
	last = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_EQUAL, &turn, &instances, NULL);
	shader_spirv_emit(reader, POWERVR_IR_IF, 0, &last, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_BREAK, 0, NULL, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_END_IF, 0, NULL, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_END_LOOP, 0, NULL, NULL, NULL, 0, 0);
	shader_spirv_emit(reader, POWERVR_IR_MUTEX, 0, NULL, NULL, NULL, POWERVR_IR_MUTEX_ATOMIC, POWERVR_IR_MUTEX_RELEASE);
	return result;
}
