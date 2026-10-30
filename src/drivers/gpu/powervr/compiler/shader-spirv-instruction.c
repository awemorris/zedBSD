/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPIR-V reader's instructions: memory access through pointers,
 * composites, arithmetic, the GLSL.std.450 functions and texture
 * samples, each split into the intermediate form's scalar words.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/compiler/shader-ir.h"
#include "drivers/gpu/powervr/compiler/shader-private.h"
#include "drivers/gpu/powervr/compiler/shader-spirv.h"

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
static int reader_arithmetic(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length, uint32_t *handled);
static int reader_composite(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length, uint32_t *handled);
static int reader_extended(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
static int reader_sample(struct shader_spirv_reader *reader, const uint32_t *words, uint32_t length);
static struct powervr_ir_operand sine(struct shader_spirv_reader *reader, struct powervr_ir_operand x, uint32_t quarter);
static int same_operand(const struct powervr_ir_operand *a, const struct powervr_ir_operand *b);

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
		if (result != 0) {
			if (words[2] >= reader->bound)
				return EINVAL;
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
		/* The combined image's binding carries over. */
		if (length < 5 || words[2] >= reader->bound || words[3] >= reader->bound)
			return EINVAL;
		reader->ids[words[2]] = reader->ids[words[3]];
		return 0;
	case OP_IMAGE_SAMPLE_IMPLICIT_LOD:
		error = reader_sample(reader, words, length);
		if (error != 0)
			return error;

		/* Succeeded: the sample is read. */
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
	if (pointer.storage == STORAGE_UNIFORM || pointer.storage == STORAGE_STORAGE_BUFFER || pointer.storage == STORAGE_PUSH_CONSTANT)
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
			if (memory != 0) {
				pointer.offset += member->offset;
			} else {
				pointer.offset += member->scalar_offset;
			}
			current = member->type;
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
			/* One run-time index into buffer memory. */
			pointer.index = shader_spirv_plain(reader, index);
			pointer.index_stride = stride;
			pointer.flags |= FLAG_HAS_INDEX;
		} else {
			/* Run-time indexing of registers, or twice, is outside the subset. */
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
	struct shader_spirv_id *source;
	uint32_t count;
	uint32_t index;
	uint32_t request;
	int error;

	/* The pointer and the words to read. */
	if (pointer >= reader->bound || result >= reader->bound || type >= reader->bound)
		return EINVAL;
	source = &reader->ids[pointer];
	if (source->kind != ID_POINTER)
		return EINVAL;

	/* A sampled image: its binding is all a load gives. */
	if (source->storage == STORAGE_UNIFORM_CONSTANT) {
		reader->ids[result].kind = ID_SAMPLED_IMAGE;
		reader->ids[result].set = reader->ids[source->variable].set;
		reader->ids[result].binding = reader->ids[source->variable].binding;
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
		/* Loads from the buffer, word by word. */
		error = reader_memory_address(reader, source, address);
		if (error != 0)
			return error;
		for (index = 0; index < count; index++) {
			offset = address[2];
			if (index != 0) {
				word = shader_spirv_constant_operand(index * 4U);
				offset = shader_spirv_emit_value(reader, POWERVR_IR_INTEGER_ADD, &address[2], &word, NULL);
			}
			operands[index] = shader_spirv_value_operand(shader_spirv_new_value(reader, 1), 0);
			shader_spirv_emit(reader, POWERVR_IR_LOAD, operands[index].value, &address[0], &address[1], &offset, 0, 0);
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
	struct powervr_ir_operand address[3];
	struct powervr_ir_operand offset;
	struct powervr_ir_operand word;
	struct powervr_ir_operand *operands;
	struct powervr_ir_instruction *instruction;
	struct shader_spirv_id *target;
	uint32_t count;
	uint32_t index;
	int error;

	/* The pointer and the object's words. */
	if (pointer >= reader->bound)
		return EINVAL;
	target = &reader->ids[pointer];
	if (target->kind != ID_POINTER)
		return EINVAL;
	error = shader_spirv_id_operands(reader, object, &operands, &count);
	if (error != 0)
		return error;

	/* Writes them by where the pointer points. */
	switch (target->storage) {
	case STORAGE_OUTPUT:
	case STORAGE_PRIVATE:
	case STORAGE_FUNCTION:
		/* Moves into the variable's values. */
		for (index = 0; index < count; index++)
			shader_spirv_emit(reader, POWERVR_IR_MOVE, reader->ids[target->variable].first + target->offset + index, &operands[index], NULL, NULL, 0, 0);
		break;
	case STORAGE_UNIFORM:
	case STORAGE_STORAGE_BUFFER:
		/* Stores to the buffer, word by word. */
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
			word = shader_spirv_plain(reader, operands[index]);
			shader_spirv_emit(reader, POWERVR_IR_STORE, 0, &address[0], &address[1], &offset, 0, 0);
			instruction = &reader->shader->instructions[reader->shader->instruction_count - 1U];
			if (reader->error == 0)
				instruction->source[3] = word;
		}
		break;
	default:
		return ENOTSUP;
	}

	/* Reports what the store came to. */
	return reader->error;
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

	/* The binding's address words, asked for once per binding. */
	variable = &reader->ids[pointer->variable];
	error = shader_spirv_request_add(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_BUFFER_ADDRESS, variable->set, variable->binding, 2, &request);
	if (error != 0)
		return error;
	address[0] = shader_spirv_shared_word(reader, request, 0);
	address[1] = shader_spirv_shared_word(reader, request, 1);

	/* The byte offset: the constant part, plus the index times its stride. */
	offset = shader_spirv_constant_operand(pointer->offset);
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

	/* The sources. */
	error = shader_spirv_id_operands(reader, words[5], &a, &a_count);
	if (error != 0)
		return error;
	b = NULL;
	c = NULL;
	b_count = 0;
	c_count = 0;
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

/* Reads a sample of a combined image at two coordinates. */
static int
reader_sample(
	struct shader_spirv_reader *reader,
	const uint32_t *words,
	uint32_t length)
{
	struct powervr_ir_operand *coordinates;
	struct powervr_ir_operand *result;
	struct powervr_ir_operand u;
	struct powervr_ir_operand v;
	struct shader_spirv_id *image;
	uint32_t coordinate_count;
	uint32_t image_request;
	uint32_t sampler_request;
	uint32_t sampled;
	uint32_t index;
	int error;

	/* Only plain fragment samples of a two-dimensional image. */
	if (length != 5 || reader->stage != POWERVR_SHADER_FRAGMENT)
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

	/* The image and sampler state words of the binding. */
	error = shader_spirv_request_add(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_IMAGE_STATE, image->set, image->binding, POWERVR_SHADER_STATE_WORDS, &image_request);
	if (error != 0)
		return error;
	error = shader_spirv_request_add(reader, POWERVR_SHADER_REQUEST_DESCRIPTOR, POWERVR_SHADER_PART_SAMPLER_STATE, image->set, image->binding, POWERVR_SHADER_STATE_WORDS, &sampler_request);
	if (error != 0)
		return error;

	/* The sample into a four-word value, then each word of it. */
	u = coordinates[0];
	v = coordinates[1];
	sampled = shader_spirv_new_value(reader, 4);
	shader_spirv_emit(reader, POWERVR_IR_SAMPLE, sampled, &u, &v, NULL, image_request, sampler_request);
	error = shader_spirv_result_begin(reader, words[2], words[1], 4, &result);
	if (error != 0)
		return error;
	for (index = 0; index < 4; index++)
		result[index] = shader_spirv_value_operand(sampled, index);

	/* Reports what the sample came to. */
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
