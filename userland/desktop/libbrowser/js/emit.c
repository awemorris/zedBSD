/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writing one function's code unit: instructions, labels and the jumps to
 * them, exception handlers, the table of constants (each value once), the
 * registers for temporary values, and the checked code unit at the end.
 *
 * A jump is written with a placeholder and patched when the unit is
 * finished, when every label has its place; a handler names its label the
 * same way.  Every failure jumps back to js_compile (js_compile_fail).
 */

#include "js/compile.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest string literal kept as an atom (longer ones are ordinary strings, so they can be freed). */
#define EMIT_ATOM_MAX		64U

/* The first size of a function's table of constant indices (a power of two). */
#define EMIT_INDEX_FIRST	64U

static void emit_push(struct js_function_compiler *fc, uint32_t word);
static void emit_note_position(struct js_function_compiler *fc, uint32_t offset);
static uint32_t emit_hash(vm_value value, uint32_t capacity);
static void emit_index_grow(struct js_function_compiler *fc);
static void emit_patch(struct js_function_compiler *fc, uint32_t word, uint32_t instruction, uint32_t label);
static uint32_t emit_label_offset(struct js_function_compiler *fc, uint32_t label);

/*
 * Starts a function's code unit: empty lists of words, constants,
 * handlers, labels and patches.
 */
void
js_emit_begin(
	struct js_function_compiler *fc)
{
	/* The lists. */
	wb_vector_init(&fc->words, sizeof(uint32_t));
	wb_vector_init(&fc->constants, sizeof(vm_value));
	wb_vector_init(&fc->handlers, sizeof(struct vm_handler));
	wb_vector_init(&fc->labels, sizeof(uint32_t));
	wb_vector_init(&fc->patches, sizeof(struct js_patch));
	wb_vector_init(&fc->positions, sizeof(struct vm_position));

	/* No source position until the first expression or statement, and no optional chain. */
	fc->line = 0;
	fc->column = 0;
	fc->chain_label = JS_LABEL_UNPLACED;

	/* No table of constant indices until the first constant. */
	fc->constant_index = NULL;
	fc->constant_capacity = 0;
	fc->released = 0;
}

/*
 * Makes a node's source position the current one, keeping the one before
 * so the caller can restore it once the node is compiled (the parent's
 * own instructions, written after its children, then carry the parent's
 * position).
 */
void
js_emit_at(
	struct js_function_compiler *fc,
	const struct js_node *node,
	uint32_t *saved_line,
	uint32_t *saved_column)
{
	/* Keeps the position before. */
	*saved_line = fc->line;
	*saved_column = fc->column;

	/* A node the parser made without a position keeps the current one. */
	if (node->line == 0)
		return;

	/* The node's position is where its instructions come from. */
	fc->line = node->line;
	fc->column = node->column;
}

/*
 * Frees what a function's code unit being written holds outside the
 * arena (once; a second call does nothing).
 */
void
js_emit_release(
	struct js_function_compiler *fc)
{
	/* Released already. */
	if (fc->released)
		return;

	/* The lists and the table. */
	wb_vector_release(&fc->words);
	wb_vector_release(&fc->constants);
	wb_vector_release(&fc->handlers);
	wb_vector_release(&fc->labels);
	wb_vector_release(&fc->patches);
	wb_vector_release(&fc->positions);
	free(fc->constant_index);
	fc->constant_index = NULL;
	fc->released = 1;
}

/*
 * Writes an instruction with its operands; reports where it starts.
 */
uint32_t
js_emit(
	struct js_function_compiler *fc,
	uint32_t opcode,
	uint32_t operand_count,
	const uint32_t *operands)
{
	uint32_t start;
	uint32_t index;

	/* The source position the instruction comes from, then the opcode and each operand. */
	start = js_here(fc);
	emit_note_position(fc, start);
	emit_push(fc, opcode);
	for (index = 0; index < operand_count; index++)
		emit_push(fc, operands[index]);

	/* Reports the instruction's start. */
	return start;
}

/*
 * Writes an instruction without operands.
 */
uint32_t
js_emit0(
	struct js_function_compiler *fc,
	uint32_t opcode)
{
	uint32_t start;

	/* The opcode alone. */
	start = js_emit(fc, opcode, 0, NULL);

	/* Reports the instruction's start. */
	return start;
}

/*
 * Writes an instruction with one operand.
 */
uint32_t
js_emit1(
	struct js_function_compiler *fc,
	uint32_t opcode,
	uint32_t first)
{
	uint32_t operands[1];
	uint32_t start;

	/* The operand. */
	operands[0] = first;
	start = js_emit(fc, opcode, 1, operands);

	/* Reports the instruction's start. */
	return start;
}

/*
 * Writes an instruction with two operands.
 */
uint32_t
js_emit2(
	struct js_function_compiler *fc,
	uint32_t opcode,
	uint32_t first,
	uint32_t second)
{
	uint32_t operands[2];
	uint32_t start;

	/* The operands in order. */
	operands[0] = first;
	operands[1] = second;
	start = js_emit(fc, opcode, 2, operands);

	/* Reports the instruction's start. */
	return start;
}

/*
 * Writes an instruction with three operands.
 */
uint32_t
js_emit3(
	struct js_function_compiler *fc,
	uint32_t opcode,
	uint32_t first,
	uint32_t second,
	uint32_t third)
{
	uint32_t operands[3];
	uint32_t start;

	/* The operands in order. */
	operands[0] = first;
	operands[1] = second;
	operands[2] = third;
	start = js_emit(fc, opcode, 3, operands);

	/* Reports the instruction's start. */
	return start;
}

/*
 * Writes an instruction with four operands.
 */
uint32_t
js_emit4(
	struct js_function_compiler *fc,
	uint32_t opcode,
	uint32_t first,
	uint32_t second,
	uint32_t third,
	uint32_t fourth)
{
	uint32_t operands[4];
	uint32_t start;

	/* The operands in order. */
	operands[0] = first;
	operands[1] = second;
	operands[2] = third;
	operands[3] = fourth;
	start = js_emit(fc, opcode, 4, operands);

	/* Reports the instruction's start. */
	return start;
}

/*
 * Writes an instruction with five operands.
 */
uint32_t
js_emit5(
	struct js_function_compiler *fc,
	uint32_t opcode,
	uint32_t first,
	uint32_t second,
	uint32_t third,
	uint32_t fourth,
	uint32_t fifth)
{
	uint32_t operands[5];
	uint32_t start;

	/* The operands in order. */
	operands[0] = first;
	operands[1] = second;
	operands[2] = third;
	operands[3] = fourth;
	operands[4] = fifth;
	start = js_emit(fc, opcode, 5, operands);

	/* Reports the instruction's start. */
	return start;
}

/*
 * Makes a label, not placed yet.
 */
uint32_t
js_label_new(
	struct js_function_compiler *fc)
{
	uint32_t unplaced;
	uint32_t label;
	int error;

	/* The label's slot in the list. */
	unplaced = JS_LABEL_UNPLACED;
	label = (uint32_t)fc->labels.count;
	error = wb_vector_push(&fc->labels, &unplaced);
	if (error != 0)
		js_compile_out_of_memory(fc->compiler);

	/* Reports the label. */
	return label;
}

/*
 * Places a label at the next instruction.
 */
void
js_label_place(
	struct js_function_compiler *fc,
	uint32_t label)
{
	uint32_t *offset;

	/* The label's place is here. */
	offset = wb_vector_at(&fc->labels, label);
	*offset = js_here(fc);
}

/*
 * Reports where the next instruction starts.
 */
uint32_t
js_here(
	const struct js_function_compiler *fc)
{
	/* The number of words written. */
	return (uint32_t)fc->words.count;
}

/*
 * Writes a jump to a label: JUMP, or JUMP_IF_TRUE or JUMP_IF_FALSE on a
 * register.
 */
void
js_emit_jump(
	struct js_function_compiler *fc,
	uint32_t opcode,
	uint32_t test_register,
	uint32_t label)
{
	uint32_t start;

	/* An unconditional jump: the target is its only operand. */
	if (opcode == VM_OP_JUMP) {
		start = js_emit1(fc, VM_OP_JUMP, 0);
		emit_patch(fc, start + 1U, start, label);
		return;
	}

	/* A conditional one: the register, then the target. */
	start = js_emit2(fc, opcode, test_register, 0);
	emit_patch(fc, start + 2U, start, label);
}

/*
 * Writes a for-in loop's step: the next key into a register, or a jump to
 * the label when there is none.
 */
void
js_emit_for_in_next(
	struct js_function_compiler *fc,
	uint32_t key_register,
	uint32_t iterator_register,
	uint32_t label)
{
	uint32_t start;

	/* The registers, then the target. */
	start = js_emit3(fc, VM_OP_FOR_IN_NEXT, key_register, iterator_register, 0);
	emit_patch(fc, start + 3U, start, label);
}

/*
 * Writes a for-of loop's step: the iteration's next value into a register,
 * or a jump to the label once it has ended (ws074-p087).
 */
void
js_emit_for_of_next(
	struct js_function_compiler *fc,
	uint32_t value_register,
	uint32_t iterator_register,
	uint32_t label)
{
	uint32_t start;

	/* The registers, then the target. */
	start = js_emit3(fc, VM_OP_FOR_OF_NEXT, value_register, iterator_register, 0);
	emit_patch(fc, start + 3U, start, label);
}

/*
 * Adds an exception handler: an exception thrown in [start, end) lands at
 * the label with the exception in a register.
 */
void
js_emit_handler(
	struct js_function_compiler *fc,
	uint32_t start,
	uint32_t end,
	uint32_t label,
	uint32_t exception_register)
{
	struct vm_handler handler;
	int error;

	/* The handler, its target a label until the unit is finished. */
	handler.start = start;
	handler.end = end;
	handler.handler = label;
	handler.exception_register = exception_register;
	error = wb_vector_push(&fc->handlers, &handler);
	if (error != 0)
		js_compile_out_of_memory(fc->compiler);
}

/*
 * Adds a constant (a value the table has already is shared); reports its
 * number.
 */
uint32_t
js_constant(
	struct js_function_compiler *fc,
	vm_value value)
{
	uint32_t slot;
	uint32_t number;
	vm_value *listed;
	int error;

	/* Room in the table of indices (kept at most half full). */
	if ((fc->constants.count + 1U) * 2U > fc->constant_capacity)
		emit_index_grow(fc);

	/* The value's slot: the constant already there, or an empty one. */
	slot = emit_hash(value, fc->constant_capacity);
	while (fc->constant_index[slot] != 0) {
		number = fc->constant_index[slot] - 1U;
		listed = wb_vector_at(&fc->constants, number);
		if (*listed == value)
			return number;
		slot = (slot + 1U) & (fc->constant_capacity - 1U);
	}

	/* A new constant. */
	number = (uint32_t)fc->constants.count;
	error = wb_vector_push(&fc->constants, &value);
	if (error != 0)
		js_compile_out_of_memory(fc->compiler);
	fc->constant_index[slot] = number + 1U;

	/* Reports its number. */
	return number;
}

/*
 * Adds a string constant (a short one as its atom, so the same text is the
 * same constant); reports its number.
 */
uint32_t
js_constant_string(
	struct js_function_compiler *fc,
	const uint16_t *text,
	size_t length)
{
	struct vm_string *string;
	uint32_t number;

	/* The string: an atom when short. */
	if (length <= EMIT_ATOM_MAX) {
		string = vm_atom_from_units(fc->compiler->realm->heap, text, length);
	} else {
		string = vm_string_from_units(fc->compiler->realm->heap, text, length);
	}

	/* Out of memory. */
	if (string == NULL)
		js_compile_out_of_memory(fc->compiler);

	/* Reports its constant. */
	number = js_constant(fc, vm_value_cell(string));
	return number;
}

/*
 * Adds the property key of a name as a constant (an index or an atom);
 * reports its number.
 */
uint32_t
js_constant_key(
	struct js_function_compiler *fc,
	const uint16_t *text,
	size_t length)
{
	struct vm_string *string;
	vm_value key;
	uint32_t number;
	int error;

	/* The name as a string, then as a key. */
	string = vm_string_from_units(fc->compiler->realm->heap, text, length);
	if (string == NULL)
		js_compile_out_of_memory(fc->compiler);
	error = vm_key_from_string(fc->compiler->realm->heap, string, &key);
	if (error != 0)
		js_compile_out_of_memory(fc->compiler);

	/* Reports its constant. */
	number = js_constant(fc, key);
	return number;
}

/*
 * Adds a number constant; reports its number.
 */
uint32_t
js_constant_number(
	struct js_function_compiler *fc,
	double number)
{
	uint32_t constant;

	/* The number as a value (an int32 when it is one). */
	constant = js_constant(fc, vm_value_number(number));

	/* Reports its constant. */
	return constant;
}

/*
 * Loads a number into a register: an int32 as an immediate, anything else
 * from the constants.
 */
void
js_load_number(
	struct js_function_compiler *fc,
	uint32_t target,
	double number)
{
	vm_value value;
	uint32_t constant;
	int is_int32;

	/* An int32 (not -0) fits the instruction. */
	value = vm_value_number(number);
	is_int32 = vm_value_is_int32(value);
	if (is_int32) {
		js_emit2(fc, VM_OP_LOAD_INT, target, (uint32_t)vm_value_as_int32(value));
		return;
	}

	/* Anything else is a constant. */
	constant = js_constant(fc, value);
	js_emit2(fc, VM_OP_LOAD_CONST, target, constant);
}

/*
 * Loads a value (undefined, null, a boolean) into a register from the
 * constants.
 */
void
js_load_value(
	struct js_function_compiler *fc,
	uint32_t target,
	vm_value value)
{
	uint32_t constant;

	/* The value's constant. */
	constant = js_constant(fc, value);
	js_emit2(fc, VM_OP_LOAD_CONST, target, constant);
}

/*
 * Takes the next register for a temporary value (the caller gives it back
 * by restoring temp_top).
 */
uint32_t
js_temp(
	struct js_function_compiler *fc)
{
	uint32_t taken;

	/* The next register; the frame grows to hold it. */
	taken = fc->temp_top;
	fc->temp_top++;
	if (fc->temp_top > fc->register_count)
		fc->register_count = fc->temp_top;

	/* Reports the register. */
	return taken;
}

/*
 * Finishes a function's code unit: patches the jumps and handlers, makes
 * the checked code unit with its name and flags, and frees the lists.
 */
struct vm_code *
js_emit_finish(
	struct js_function_compiler *fc,
	const uint16_t *name,
	size_t name_length,
	uint32_t flags)
{
	struct vm_code model;
	struct vm_code *code;
	struct vm_string *string;
	struct vm_handler *handler;
	struct js_patch *patch;
	uint32_t *word;
	uint32_t target;
	uint32_t bad_offset;
	size_t index;
	char message[160];
	int error;

	/* Each jump's offset, from its instruction's start to its label. */
	for (index = 0; index < fc->patches.count; index++) {
		patch = wb_vector_at(&fc->patches, index);
		target = emit_label_offset(fc, patch->label);
		word = wb_vector_at(&fc->words, patch->word);
		*word = target - patch->instruction;
	}

	/* Each handler's target. */
	for (index = 0; index < fc->handlers.count; index++) {
		handler = wb_vector_at(&fc->handlers, index);
		handler->handler = emit_label_offset(fc, handler->handler);
	}

	/* The name. */
	string = vm_string_from_units(fc->compiler->realm->heap, name, name_length);
	if (string == NULL)
		js_compile_out_of_memory(fc->compiler);

	/* The model of the unit. */
	memset(&model, 0, sizeof(model));
	model.words = fc->words.items;
	model.word_count = (uint32_t)fc->words.count;
	model.register_count = fc->register_count;
	model.parameter_count = fc->parameter_count;
	model.constants = fc->constants.items;
	model.constant_count = (uint32_t)fc->constants.count;
	model.handlers = fc->handlers.items;
	model.handler_count = (uint32_t)fc->handlers.count;
	model.positions = fc->positions.items;
	model.position_count = (uint32_t)fc->positions.count;
	model.flags = flags;
	model.arguments_register = fc->arguments_register;
	model.length = fc->info->length;
	model.name = string;

	/* The checked unit (a unit that fails the check is the compiler's fault). */
	error = vm_code_create(fc->compiler->realm->heap, &model, &code, &bad_offset);
	if (error == ENOMEM)
		js_compile_out_of_memory(fc->compiler);
	if (error != 0) {
		snprintf(message, sizeof(message), "internal error: the compiler wrote code that does not pass the check (word %u)", bad_offset);
		js_compile_fail(fc->compiler, fc->info->node, message);
	}

	/* The lists are no longer needed. */
	js_emit_release(fc);

	/* Succeeded: the code unit. */
	return code;
}

/*
 * Records that the instructions from an offset on come from the current
 * source position, unless the last record already says so.
 */
static void
emit_note_position(
	struct js_function_compiler *fc,
	uint32_t offset)
{
	struct vm_position *last;
	struct vm_position made;
	int error;

	/* Nothing compiled yet has no position to record. */
	if (fc->line == 0)
		return;

	/* The last record covers this instruction when its position is the same. */
	if (fc->positions.count != 0) {
		last = wb_vector_at(&fc->positions, fc->positions.count - 1U);
		if (last->line == fc->line && last->column == fc->column)
			return;

		/* A record with no instruction yet is replaced rather than kept. */
		if (last->offset == offset) {
			last->line = fc->line;
			last->column = fc->column;
			return;
		}
	}

	/* A new record from this instruction on. */
	made.offset = offset;
	made.line = fc->line;
	made.column = fc->column;
	error = wb_vector_push(&fc->positions, &made);
	if (error != 0)
		js_compile_out_of_memory(fc->compiler);
}

/* Appends one word to the code. */
static void
emit_push(
	struct js_function_compiler *fc,
	uint32_t word)
{
	int error;

	/* The word at the end. */
	error = wb_vector_push(&fc->words, &word);
	if (error != 0)
		js_compile_out_of_memory(fc->compiler);
}

/* Hashes a constant's value into a table of a capacity (a power of two). */
static uint32_t
emit_hash(
	vm_value value,
	uint32_t capacity)
{
	uint64_t mixed;

	/* Fibonacci hashing of the value's bits. */
	mixed = (value ^ (value >> 29)) * 0x9E3779B97F4A7C15ULL;

	/* The slot. */
	return (uint32_t)(mixed >> 32) & (capacity - 1U);
}

/* Doubles the table of constant indices and puts every constant back in it. */
static void
emit_index_grow(
	struct js_function_compiler *fc)
{
	uint32_t *table;
	uint32_t capacity;
	uint32_t slot;
	size_t index;
	vm_value *value;

	/* The new table. */
	capacity = EMIT_INDEX_FIRST;
	if (fc->constant_capacity != 0)
		capacity = fc->constant_capacity * 2U;
	table = calloc(capacity, sizeof(*table));
	if (table == NULL)
		js_compile_out_of_memory(fc->compiler);

	/* Each constant in its new slot. */
	for (index = 0; index < fc->constants.count; index++) {
		value = wb_vector_at(&fc->constants, index);
		slot = emit_hash(*value, capacity);
		while (table[slot] != 0)
			slot = (slot + 1U) & (capacity - 1U);
		table[slot] = (uint32_t)index + 1U;
	}

	/* The new table replaces the old one. */
	free(fc->constant_index);
	fc->constant_index = table;
	fc->constant_capacity = capacity;
}

/* Records a word to patch with a label's offset when the unit is finished. */
static void
emit_patch(
	struct js_function_compiler *fc,
	uint32_t word,
	uint32_t instruction,
	uint32_t label)
{
	struct js_patch patch;
	int error;

	/* The patch. */
	patch.word = word;
	patch.instruction = instruction;
	patch.label = label;
	error = wb_vector_push(&fc->patches, &patch);
	if (error != 0)
		js_compile_out_of_memory(fc->compiler);
}

/* Reports where a label was placed (an unplaced one is the compiler's fault). */
static uint32_t
emit_label_offset(
	struct js_function_compiler *fc,
	uint32_t label)
{
	uint32_t *offset;

	/* The label's place. */
	offset = wb_vector_at(&fc->labels, label);
	if (*offset == JS_LABEL_UNPLACED)
		js_compile_fail(fc->compiler, fc->info->node, "internal error: a label was never placed");

	/* Reports the offset. */
	return *offset;
}
