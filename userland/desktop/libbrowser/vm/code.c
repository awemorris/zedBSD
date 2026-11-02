/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Code units: the table of opcodes, making a checked code unit from the
 * words a compiler wrote, and writing one out as text for the tests.
 *
 * The check is what lets the interpreter trust its code: every opcode is
 * known, every instruction ends inside the code, every register and
 * constant number is in range, every jump and handler lands on the start
 * of an instruction, and the last instruction does not run off the end.
 */

#include "vm/bytecode.h"
#include "vm/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void code_trace(struct vm_heap *heap, struct vm_cell *cell);
static void code_finalize(struct vm_heap *heap, struct vm_cell *cell);
static int code_check(const struct vm_code *code, uint32_t *bad_offset);
static int code_check_operands(const struct vm_code *code, uint32_t offset, const uint8_t *starts);
static int code_is_start(const uint8_t *starts, uint32_t word_count, uint32_t offset);
static int code_falls_off(const struct vm_code *code, const uint8_t *starts);
static int code_is_unit(vm_value value);

/* The cell type of code units: they hold their constants and their name. */
const struct vm_cell_type vm_code_type = {
	"code", code_trace, code_finalize
};

/* The opcodes: their names and operands, in the order of enum vm_opcode. */
const struct vm_opcode_info vm_opcodes[VM_OPCODE_COUNT] = {
	{ "nop", 0, { 0 } },
	{ "mov", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "load_const", 2, { VM_OPERAND_REGISTER, VM_OPERAND_CONSTANT } },
	{ "load_int", 2, { VM_OPERAND_REGISTER, VM_OPERAND_IMMEDIATE } },
	{ "jump", 1, { VM_OPERAND_JUMP } },
	{ "jump_if_true", 2, { VM_OPERAND_REGISTER, VM_OPERAND_JUMP } },
	{ "jump_if_false", 2, { VM_OPERAND_REGISTER, VM_OPERAND_JUMP } },
	{ "call", 5, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_COUNT } },
	{ "return", 1, { VM_OPERAND_REGISTER } },
	{ "throw", 1, { VM_OPERAND_REGISTER } },
	{ "loop_hint", 0, { 0 } },
	{ "add", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "sub", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "mul", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "less", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "strict_eq", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "new_object", 1, { VM_OPERAND_REGISTER } },
	{ "new_array", 1, { VM_OPERAND_REGISTER } },
	{ "get_prop", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_CONSTANT } },
	{ "put_prop", 3, { VM_OPERAND_REGISTER, VM_OPERAND_CONSTANT, VM_OPERAND_REGISTER } },
	{ "get_elem", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "put_elem", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "get_global", 2, { VM_OPERAND_REGISTER, VM_OPERAND_CONSTANT } },
	{ "put_global", 2, { VM_OPERAND_CONSTANT, VM_OPERAND_REGISTER } },
	{ "div", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "mod", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "exp", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "bit_and", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "bit_or", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "bit_xor", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "shl", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "sar", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "shr", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "less_eq", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "greater", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "greater_eq", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "loose_eq", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "instanceof", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "in", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "neg", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "to_number", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "bit_not", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "not", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "typeof", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "inc", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "dec", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "get_global_typeof", 2, { VM_OPERAND_REGISTER, VM_OPERAND_CONSTANT } },
	{ "define_global_var", 1, { VM_OPERAND_CONSTANT } },
	{ "define_global_function", 2, { VM_OPERAND_CONSTANT, VM_OPERAND_REGISTER } },
	{ "delete_prop", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_CONSTANT } },
	{ "delete_elem", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "delete_global", 2, { VM_OPERAND_REGISTER, VM_OPERAND_CONSTANT } },
	{ "define_prop", 3, { VM_OPERAND_REGISTER, VM_OPERAND_CONSTANT, VM_OPERAND_REGISTER } },
	{ "to_numeric", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "define_elem", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "define_getter", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "define_setter", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "set_proto", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "array_push", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "array_hole", 1, { VM_OPERAND_REGISTER } },
	{ "new_env", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_IMMEDIATE } },
	{ "get_env", 4, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_IMMEDIATE, VM_OPERAND_IMMEDIATE } },
	{ "get_import", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "put_env", 4, { VM_OPERAND_REGISTER, VM_OPERAND_IMMEDIATE, VM_OPERAND_IMMEDIATE, VM_OPERAND_REGISTER } },
	{ "load_closure_env", 1, { VM_OPERAND_REGISTER } },
	{ "load_this", 1, { VM_OPERAND_REGISTER } },
	{ "load_callee", 1, { VM_OPERAND_REGISTER } },
	{ "new_closure", 3, { VM_OPERAND_REGISTER, VM_OPERAND_CONSTANT, VM_OPERAND_REGISTER } },
	{ "construct", 5, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_COUNT } },
	{ "for_in_start", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "for_in_next", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_JUMP } },
	{ "to_property_key", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "new_regexp", 3, { VM_OPERAND_REGISTER, VM_OPERAND_CONSTANT, VM_OPERAND_CONSTANT } },
	{ "to_string", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "load_empty", 1, { VM_OPERAND_REGISTER } },
	{ "check_init", 2, { VM_OPERAND_REGISTER, VM_OPERAND_CONSTANT } },
	{ "throw_error", 2, { VM_OPERAND_IMMEDIATE, VM_OPERAND_CONSTANT } },
	{ "define_global_lexical", 2, { VM_OPERAND_CONSTANT, VM_OPERAND_IMMEDIATE } },
	{ "init_global_lexical", 2, { VM_OPERAND_CONSTANT, VM_OPERAND_REGISTER } },
	{ "iter_start", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "iter_next", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "iter_rest", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "array_spread", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "copy_data", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "object_rest", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "call_array", 4, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "construct_array", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "check_coercible", 1, { VM_OPERAND_REGISTER } },
	{ "args_rest", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_IMMEDIATE } },
	{ "load_new_target", 1, { VM_OPERAND_REGISTER } },
	{ "class_setup", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "define_method", 4, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_IMMEDIATE } },
	{ "load_home", 1, { VM_OPERAND_REGISTER } },
	{ "get_super", 4, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "super_construct", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_COUNT } },
	{ "super_construct_array", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "new_private_name", 2, { VM_OPERAND_REGISTER, VM_OPERAND_CONSTANT } },
	{ "private_get", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "private_set", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "private_define", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "private_copy", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "private_in", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "for_of_next", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_JUMP } },
	{ "iter_close", 2, { VM_OPERAND_REGISTER, VM_OPERAND_IMMEDIATE } },
	{ "suspend", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "delegate_start", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "delegate_step", 5, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "suspend_result", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "async_iter_start", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "check_object", 1, { VM_OPERAND_REGISTER } },
	{ "i32.const", 2, { VM_OPERAND_REGISTER, VM_OPERAND_IMMEDIATE } },
	{ "i32.add", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "i32.sub", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "i32.mul", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "i32.lt_s", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "i32.eqz", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "br_if", 2, { VM_OPERAND_REGISTER, VM_OPERAND_JUMP } },
	{ "i64.const", 3, { VM_OPERAND_REGISTER, VM_OPERAND_IMMEDIATE, VM_OPERAND_IMMEDIATE } },
	{ "i64.add", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "f64.const", 3, { VM_OPERAND_REGISTER, VM_OPERAND_IMMEDIATE, VM_OPERAND_IMMEDIATE } },
	{ "f64.add", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "f64.mul", 3, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "box_i32", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } },
	{ "box_f64", 2, { VM_OPERAND_REGISTER, VM_OPERAND_REGISTER } }
};

/*
 * Makes a checked code unit from a model (its words, constants, handlers,
 * counts and name, which are copied).  Returns EINVAL with the word offset
 * of the first fault in *bad_offset when the code does not pass the check.
 */
int
vm_code_create(
	struct vm_heap *heap,
	const struct vm_code *model,
	struct vm_code **code,
	uint32_t *bad_offset)
{
	struct vm_code *made;
	int error;

	/* The model must pass the check before anything is made. */
	*code = NULL;
	*bad_offset = 0;
	error = code_check(model, bad_offset);
	if (error != 0)
		return error;

	/* The cell, with the model's counts and name. */
	made = vm_heap_alloc(heap, &vm_code_type, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->word_count = model->word_count;
	made->register_count = model->register_count;
	made->parameter_count = model->parameter_count;
	made->constant_count = model->constant_count;
	made->handler_count = model->handler_count;
	made->flags = model->flags;
	made->arguments_register = model->arguments_register;
	made->length = model->length;
	made->name = model->name;

	/* A copy of the words. */
	made->words = malloc((size_t)model->word_count * sizeof(uint32_t) + 1U);
	if (made->words == NULL)
		return ENOMEM;
	memcpy(made->words, model->words, (size_t)model->word_count * sizeof(uint32_t));

	/* A copy of the constants. */
	made->constants = malloc((size_t)model->constant_count * sizeof(vm_value) + 1U);
	if (made->constants == NULL)
		return ENOMEM;
	if (model->constant_count != 0)
		memcpy(made->constants, model->constants, (size_t)model->constant_count * sizeof(vm_value));

	/* A copy of the handlers. */
	made->handlers = malloc((size_t)model->handler_count * sizeof(struct vm_handler) + 1U);
	if (made->handlers == NULL)
		return ENOMEM;
	if (model->handler_count != 0)
		memcpy(made->handlers, model->handlers, (size_t)model->handler_count * sizeof(struct vm_handler));

	/* A copy of the source positions (a unit made by hand has none). */
	made->position_count = model->position_count;
	made->positions = malloc((size_t)model->position_count * sizeof(struct vm_position) + 1U);
	if (made->positions == NULL)
		return ENOMEM;
	if (model->position_count != 0)
		memcpy(made->positions, model->positions, (size_t)model->position_count * sizeof(struct vm_position));

	/* Succeeded: the code unit. */
	*code = made;
	return 0;
}

/*
 * Finds the source position of the instruction at a word offset.
 *
 * Returns 1 with the line and the column of the expression or statement
 * the instruction was compiled from, or 0 when the unit has no position
 * for it.
 */
int
vm_code_position(
	const struct vm_code *code,
	uint32_t offset,
	uint32_t *line,
	uint32_t *column)
{
	const struct vm_position *found;
	uint32_t low;
	uint32_t high;
	uint32_t middle;

	/* Searches the positions, which are in the order of their offsets, for the last one at or before the offset. */
	found = NULL;
	low = 0;
	high = code->position_count;
	while (low < high) {
		middle = low + (high - low) / 2U;
		if (code->positions[middle].offset <= offset) {
			found = &code->positions[middle];
			low = middle + 1U;
		} else {
			high = middle;
		}
	}

	/* An instruction before the first position has none. */
	if (found == NULL)
		return 0;

	/* Succeeded: the position that covers the instruction. */
	*line = found->line;
	*column = found->column;
	return 1;
}

/*
 * Writes a code unit as text, one instruction a line, for the tests and
 * for debugging.
 */
int
vm_code_dump(
	const struct vm_code *code,
	struct wb_buffer *out)
{
	const struct vm_opcode_info *info;
	uint32_t offset;
	uint32_t operand;
	int error;

	/* The unit's counts. */
	error = wb_buffer_printf(out, "code registers=%u parameters=%u constants=%u handlers=%u flags=%u\n",
	    code->register_count, code->parameter_count, code->constant_count, code->handler_count, code->flags);

	/* Each instruction: its offset, name and operands. */
	offset = 0;
	while (error == 0 && offset < code->word_count) {
		info = &vm_opcodes[code->words[offset]];
		error = wb_buffer_printf(out, "%5u %s", offset, info->name);

		/* The operands, each marked by its kind. */
		for (operand = 0; error == 0 && operand < info->operand_count; operand++) {
			if (info->kinds[operand] == VM_OPERAND_REGISTER)
				error = wb_buffer_printf(out, " r%u", code->words[offset + 1U + operand]);
			else if (info->kinds[operand] == VM_OPERAND_CONSTANT)
				error = wb_buffer_printf(out, " c%u", code->words[offset + 1U + operand]);
			else if (info->kinds[operand] == VM_OPERAND_JUMP)
				error = wb_buffer_printf(out, " @%d", (int)(offset + code->words[offset + 1U + operand]));
			else
				error = wb_buffer_printf(out, " %d", (int)code->words[offset + 1U + operand]);
		}

		/* The line's end, and the next instruction. */
		if (error == 0)
			error = wb_buffer_append_string(out, "\n");
		offset += 1U + info->operand_count;
	}

	/* Reports a buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the code is written. */
	return 0;
}

/* Marks a code unit's constants and name. */
static void
code_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct vm_code *code;
	uint32_t index;

	/* The constants. */
	code = (struct vm_code *)cell;
	for (index = 0; code->constants != NULL && index < code->constant_count; index++)
		vm_heap_mark_value(heap, code->constants[index]);

	/* The name. */
	if (code->name != NULL)
		vm_heap_mark(heap, &code->name->cell);
}

/* Frees a dead code unit's copies. */
static void
code_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct vm_code *code;

	UNUSED_PARAMETER(heap);

	/* The words, the constants and the handlers. */
	code = (struct vm_code *)cell;
	free(code->words);
	free(code->constants);
	free(code->handlers);
	free(code->positions);
	code->words = NULL;
	code->constants = NULL;
	code->handlers = NULL;
}

/* Checks a model of a code unit; returns EINVAL with the fault's offset, or 0. */
static int
code_check(
	const struct vm_code *code,
	uint32_t *bad_offset)
{
	const struct vm_opcode_info *info;
	uint8_t *starts;
	uint32_t offset;
	uint32_t index;
	int start;
	int falls;
	int error;

	/* A unit with no instruction, or with fewer registers than parameters, is refused. */
	if (code->word_count == 0 || code->register_count < code->parameter_count)
		return EINVAL;

	/* The arguments object's register is the frame's. */
	if ((code->flags & VM_CODE_ARGUMENTS) != 0U && code->arguments_register >= code->register_count)
		return EINVAL;

	/* One mark a word: whether an instruction starts there. */
	starts = calloc(code->word_count, 1);
	if (starts == NULL)
		return ENOMEM;

	/* Walks the instructions: each opcode known and whole inside the code. */
	offset = 0;
	error = 0;
	while (offset < code->word_count) {
		if (code->words[offset] >= VM_OPCODE_COUNT) {
			error = EINVAL;
			break;
		}

		/* The instruction's operands are inside the code. */
		info = &vm_opcodes[code->words[offset]];
		if (offset + 1U + info->operand_count > code->word_count) {
			error = EINVAL;
			break;
		}

		/* It starts here, and the next one after its operands. */
		starts[offset] = 1;
		offset += 1U + info->operand_count;
	}

	/* Checks each instruction's operands against the marks. */
	offset = 0;
	while (error == 0 && offset < code->word_count) {
		error = code_check_operands(code, offset, starts);
		if (error != 0)
			break;
		offset += 1U + vm_opcodes[code->words[offset]].operand_count;
	}

	/* Each handler's range and target. */
	for (index = 0; error == 0 && index < code->handler_count; index++) {
		offset = code->handlers[index].start;
		if (code->handlers[index].start > code->handlers[index].end || code->handlers[index].end > code->word_count)
			error = EINVAL;
		start = code_is_start(starts, code->word_count, code->handlers[index].handler);
		if (error == 0 && !start) {
			offset = code->handlers[index].handler;
			error = EINVAL;
		}

		/* The register that takes the exception is the frame's. */
		if (error == 0 && code->handlers[index].exception_register >= code->register_count)
			error = EINVAL;
	}

	/* The code must not run off its end. */
	if (error == 0) {
		falls = code_falls_off(code, starts);
		if (falls) {
			offset = code->word_count;
			error = EINVAL;
		}
	}

	/* The marks are no longer needed. */
	free(starts);
	if (error != 0) {
		*bad_offset = offset;
		return error;
	}

	/* Succeeded: the code may be run. */
	return 0;
}

/* Checks one instruction's operands: registers and constants in range, jumps onto instruction starts. */
static int
code_check_operands(
	const struct vm_code *code,
	uint32_t offset,
	const uint8_t *starts)
{
	const struct vm_opcode_info *info;
	uint32_t operand;
	uint32_t word;
	uint32_t target;
	uint32_t first;
	uint32_t count;
	int start;
	int is_code;

	/* Each operand by its kind. */
	info = &vm_opcodes[code->words[offset]];
	for (operand = 0; operand < info->operand_count; operand++) {
		word = code->words[offset + 1U + operand];
		switch (info->kinds[operand]) {
		case VM_OPERAND_REGISTER:
			/* A register of the frame. */
			if (word >= code->register_count)
				return EINVAL;
			break;
		case VM_OPERAND_CONSTANT:
			/* A constant of the table; new_closure's must be a code unit. */
			if (word >= code->constant_count)
				return EINVAL;
			if (code->words[offset] == VM_OP_NEW_CLOSURE) {
				is_code = code_is_unit(code->constants[word]);
				if (!is_code)
					return EINVAL;
			}

			/* The constant is sound. */
			break;
		case VM_OPERAND_JUMP:
			/* A jump lands on the start of an instruction. */
			target = offset + word;
			start = code_is_start(starts, code->word_count, target);
			if (!start)
				return EINVAL;
			break;
		case VM_OPERAND_COUNT:
			/* A count of registers from the operand before it stays inside the frame. */
			first = code->words[offset + operand];
			count = word;
			if (count > code->register_count || first > code->register_count - count)
				return EINVAL;
			break;
		default:
			break;
		}
	}

	/* Succeeded: the operands are sound. */
	return 0;
}

/* Tells whether an offset is the start of an instruction. */
static int
code_is_start(
	const uint8_t *starts,
	uint32_t word_count,
	uint32_t offset)
{
	/* Past the end is no instruction (this also catches a negative jump wrapping around). */
	if (offset >= word_count)
		return 0;

	/* The mark. */
	if (starts[offset])
		return 1;

	/* Inside another instruction. */
	return 0;
}

/* Tells whether the code runs off its end: its last instruction goes on to the next one. */
static int
code_falls_off(
	const struct vm_code *code,
	const uint8_t *starts)
{
	uint32_t offset;
	uint32_t last;
	uint32_t opcode;

	/* The last instruction. */
	last = 0;
	for (offset = 0; offset < code->word_count; offset++) {
		if (starts[offset])
			last = offset;
	}

	/* Only an instruction that never goes on to the next ends the code. */
	opcode = code->words[last];
	if (opcode == VM_OP_RETURN || opcode == VM_OP_THROW || opcode == VM_OP_JUMP)
		return 0;

	/* Anything else would run off the end. */
	return 1;
}

/* Tells whether a constant is a code unit (what new_closure makes a function of). */
static int
code_is_unit(
	vm_value value)
{
	struct vm_cell *cell;
	int is_cell;

	/* Only cells are code units. */
	is_cell = vm_value_is_cell(value);
	if (!is_cell)
		return 0;

	/* A code cell. */
	cell = vm_value_as_cell(value);
	if (cell->type == &vm_code_type)
		return 1;

	/* Another cell. */
	return 0;
}
