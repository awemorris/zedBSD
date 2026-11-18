/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Skipping AML without running it.
 *
 * An OperationRegion defined while a table loads keeps its offset and
 * length as AML and evaluates them on first use, when every name they
 * refer to exists.  Stepping over those TermArgs needs the shape of every
 * operator: the table below lists, for each opcode, the operands that
 * follow it.  A name that resolves to a method is followed by as many
 * TermArgs as the method takes.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "aml-internal.h"

/*
 * The operand letters of the shape strings: T a TermArg, S a SuperName or
 * Target, N a NameString, B a byte, W a word, D a double word.
 */
#define SHAPE_TERM	'T'
#define SHAPE_SUPER	'S'
#define SHAPE_NAME	'N'
#define SHAPE_BYTE	'B'
#define SHAPE_WORD	'W'
#define SHAPE_DWORD	'D'

/*
 * The operands of one operator.
 */
struct operator_shape {
	unsigned opcode;
	const char *operands;
};

/*
 * The operand shapes of every operator that can appear in a TermArg or a
 * target.
 */
static const struct operator_shape operator_shapes[] = {
	{ DRV_ACPI_OP_STORE, "TS" },
	{ DRV_ACPI_OP_REF_OF, "S" },
	{ DRV_ACPI_OP_ADD, "TTS" },
	{ DRV_ACPI_OP_CONCAT, "TTS" },
	{ DRV_ACPI_OP_SUBTRACT, "TTS" },
	{ DRV_ACPI_OP_INCREMENT, "S" },
	{ DRV_ACPI_OP_DECREMENT, "S" },
	{ DRV_ACPI_OP_MULTIPLY, "TTS" },
	{ DRV_ACPI_OP_DIVIDE, "TTSS" },
	{ DRV_ACPI_OP_SHIFT_LEFT, "TTS" },
	{ DRV_ACPI_OP_SHIFT_RIGHT, "TTS" },
	{ DRV_ACPI_OP_AND, "TTS" },
	{ DRV_ACPI_OP_NAND, "TTS" },
	{ DRV_ACPI_OP_OR, "TTS" },
	{ DRV_ACPI_OP_NOR, "TTS" },
	{ DRV_ACPI_OP_XOR, "TTS" },
	{ DRV_ACPI_OP_NOT, "TS" },
	{ DRV_ACPI_OP_FIND_SET_LEFT_BIT, "TS" },
	{ DRV_ACPI_OP_FIND_SET_RIGHT_BIT, "TS" },
	{ DRV_ACPI_OP_DEREF_OF, "T" },
	{ DRV_ACPI_OP_CONCAT_RES, "TTS" },
	{ DRV_ACPI_OP_MOD, "TTS" },
	{ DRV_ACPI_OP_NOTIFY, "ST" },
	{ DRV_ACPI_OP_SIZE_OF, "S" },
	{ DRV_ACPI_OP_INDEX, "TTS" },
	{ DRV_ACPI_OP_MATCH, "TBTBTT" },
	{ DRV_ACPI_OP_OBJECT_TYPE, "S" },
	{ DRV_ACPI_OP_LAND, "TT" },
	{ DRV_ACPI_OP_LOR, "TT" },
	{ DRV_ACPI_OP_LNOT, "T" },
	{ DRV_ACPI_OP_LEQUAL, "TT" },
	{ DRV_ACPI_OP_LGREATER, "TT" },
	{ DRV_ACPI_OP_LLESS, "TT" },
	{ DRV_ACPI_OP_TO_BUFFER, "TS" },
	{ DRV_ACPI_OP_TO_DECIMAL_STRING, "TS" },
	{ DRV_ACPI_OP_TO_HEX_STRING, "TS" },
	{ DRV_ACPI_OP_TO_INTEGER, "TS" },
	{ DRV_ACPI_OP_TO_STRING, "TTS" },
	{ DRV_ACPI_OP_COPY_OBJECT, "TS" },
	{ DRV_ACPI_OP_MID, "TTTS" },
	{ DRV_ACPI_OP_COND_REF_OF, "SS" },
	{ DRV_ACPI_OP_LOAD_TABLE, "TTTTTT" },
	{ DRV_ACPI_OP_LOAD, "NS" },
	{ DRV_ACPI_OP_STALL, "T" },
	{ DRV_ACPI_OP_SLEEP, "T" },
	{ DRV_ACPI_OP_ACQUIRE, "SW" },
	{ DRV_ACPI_OP_SIGNAL, "S" },
	{ DRV_ACPI_OP_WAIT, "ST" },
	{ DRV_ACPI_OP_RESET, "S" },
	{ DRV_ACPI_OP_RELEASE, "S" },
	{ DRV_ACPI_OP_FROM_BCD, "TS" },
	{ DRV_ACPI_OP_TO_BCD, "TS" },
	{ DRV_ACPI_OP_UNLOAD, "S" },
	{ DRV_ACPI_OP_FATAL, "BDT" },
};

static int skip_opcode_term(struct drv_acpi_eval *eval);
static int skip_operands(struct drv_acpi_eval *eval, const char *operands);
static int skip_super_name(struct drv_acpi_eval *eval);
static int skip_name_term(struct drv_acpi_eval *eval);
static int skip_bytes(struct drv_acpi_eval *eval, size_t count);
static const char *shape_of(unsigned opcode);

/*
 * Steps over one TermArg without evaluating it.
 */
int
drv_acpi_skip_term_arg(
	struct drv_acpi_eval *eval)
{
	bool named;
	int error;

	/* Refuses to go deeper than the stack budget allows. */
	error = drv_acpi_stack_check();
	if (error != 0)
		return error;

	/* A name may be a method invocation with arguments to step over; anything else starts with an opcode. */
	named = drv_acpi_stream_at_name(eval);
	if (named) {
		error = skip_name_term(eval);
	} else {
		error = skip_opcode_term(eval);
	}

	/* Reports a TermArg that could not be stepped over. */
	if (error != 0)
		return error;

	/* Succeeded: the stream is past the TermArg. */
	return 0;
}

/* Steps over a TermArg that starts with an opcode. */
static int
skip_opcode_term(
	struct drv_acpi_eval *eval)
{
	const char *operands;
	const char *text;
	const uint8_t *end;
	unsigned opcode;
	size_t length;
	int error;

	/* Reads the opcode. */
	error = drv_acpi_stream_opcode(eval, &opcode);
	if (error != 0)
		return error;

	/* Chooses the extent by the kind of opcode. */
	switch (opcode) {
	case DRV_ACPI_OP_ZERO:
	case DRV_ACPI_OP_ONE:
	case DRV_ACPI_OP_ONES:
	case DRV_ACPI_OP_REVISION:
	case DRV_ACPI_OP_DEBUG:
	case DRV_ACPI_OP_TIMER:
		/* A constant is its opcode alone. */
		error = 0;
		break;
	case DRV_ACPI_OP_BYTE_PREFIX:
		/* Steps over a byte constant. */
		error = skip_bytes(eval, 1);
		break;
	case DRV_ACPI_OP_WORD_PREFIX:
		/* Steps over a word constant. */
		error = skip_bytes(eval, 2);
		break;
	case DRV_ACPI_OP_DWORD_PREFIX:
		/* Steps over a double word constant. */
		error = skip_bytes(eval, 4);
		break;
	case DRV_ACPI_OP_QWORD_PREFIX:
		/* Steps over a quad word constant. */
		error = skip_bytes(eval, 8);
		break;
	case DRV_ACPI_OP_STRING_PREFIX:
		/* Steps over a string constant and its terminator. */
		error = drv_acpi_stream_string(eval, &text, &length);
		break;
	case DRV_ACPI_OP_BUFFER:
	case DRV_ACPI_OP_PACKAGE:
	case DRV_ACPI_OP_VAR_PACKAGE:
		/* A literal with a package length is stepped over whole. */
		error = drv_acpi_stream_package_length(eval, &end);
		if (error != 0)
			break;
		eval->position = end;
		break;
	default:
		/* Locals and arguments are single bytes. */
		if (opcode >= DRV_ACPI_OP_LOCAL0 && opcode <= DRV_ACPI_OP_ARG6) {
			error = 0;
			break;
		}

		/* Every other TermArg is an operator with a known shape. */
		operands = shape_of(opcode);
		if (operands == NULL) {
			error = EIO;
			break;
		}

		/* Steps over its operands. */
		error = skip_operands(eval, operands);
		break;
	}

	/* Reports a TermArg that could not be stepped over. */
	if (error != 0)
		return error;

	/* Succeeded: the stream is past the TermArg. */
	return 0;
}

/* Steps over the operands a shape string lists. */
static int
skip_operands(
	struct drv_acpi_eval *eval,
	const char *operands)
{
	struct drv_acpi_name name;
	const char *letter;
	int error;

	/* Steps over each operand in order. */
	for (letter = operands;
	     *letter != '\0';
	     letter++) {
		/* Chooses the step by the kind of operand. */
		switch (*letter) {
		case SHAPE_TERM:
			/* Steps over a TermArg. */
			error = drv_acpi_skip_term_arg(eval);
			break;
		case SHAPE_SUPER:
			/* Steps over a SuperName or a Target. */
			error = skip_super_name(eval);
			break;
		case SHAPE_NAME:
			/* Steps over a NameString. */
			error = drv_acpi_stream_name(eval, &name);
			break;
		case SHAPE_BYTE:
			/* Steps over a byte. */
			error = skip_bytes(eval, 1);
			break;
		case SHAPE_WORD:
			/* Steps over a word. */
			error = skip_bytes(eval, 2);
			break;
		case SHAPE_DWORD:
			/* Steps over a double word. */
			error = skip_bytes(eval, 4);
			break;
		default:
			/* Refuses a letter the shapes do not use. */
			error = EINVAL;
			break;
		}

		/* Stops at the first operand that could not be stepped over. */
		if (error != 0)
			return error;
	}

	/* Succeeded: the stream is past every operand. */
	return 0;
}

/* Steps over a SuperName or a Target. */
static int
skip_super_name(
	struct drv_acpi_eval *eval)
{
	struct drv_acpi_name name;
	const char *operands;
	unsigned opcode;
	uint8_t byte;
	bool named;
	int error;

	/* A name in a target position is not invoked. */
	named = drv_acpi_stream_at_name(eval);
	if (named) {
		error = drv_acpi_stream_name(eval, &name);
		if (error != 0)
			return error;

		/* Succeeded: the stream is past the name. */
		return 0;
	}

	/* Looks at the first byte. */
	error = drv_acpi_stream_peek(eval, &byte);
	if (error != 0)
		return error;

	/* The null name is one byte. */
	if (byte == 0x00U) {
		eval->position++;
		return 0;
	}

	/* Reads the opcode of any other target. */
	error = drv_acpi_stream_opcode(eval, &opcode);
	if (error != 0)
		return error;

	/* Locals, arguments and the debug object have no operands. */
	if (opcode >= DRV_ACPI_OP_LOCAL0 && opcode <= DRV_ACPI_OP_ARG6)
		return 0;
	if (opcode == DRV_ACPI_OP_DEBUG)
		return 0;

	/* A reference operator has its usual operands. */
	operands = shape_of(opcode);
	if (operands == NULL)
		return EIO;

	/* Steps over them. */
	error = skip_operands(eval, operands);
	if (error != 0)
		return error;

	/* Succeeded: the stream is past the target. */
	return 0;
}

/* Steps over a name in a TermArg position and any method arguments. */
static int
skip_name_term(
	struct drv_acpi_eval *eval)
{
	struct drv_acpi_name name;
	struct drv_acpi_node *node;
	unsigned count;
	unsigned index;
	int error;

	/* Reads the name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* A name that does not resolve yet is taken to have no arguments. */
	error = drv_acpi_ns_lookup(eval->scope, &name, true, &node);
	if (error != 0)
		return 0;

	/* A name of an alias is the node the alias stands for. */
	node = drv_acpi_ns_resolve_alias(node);

	/* Anything but a method has no arguments. */
	if (node->object == NULL || node->object->type != DRV_ACPI_TYPE_METHOD)
		return 0;

	/* Steps over the method's arguments. */
	count = node->object->value.method.argument_count;
	for (index = 0; index < count; index++) {
		/* Steps over one argument. */
		error = drv_acpi_skip_term_arg(eval);
		if (error != 0)
			return error;
	}

	/* Succeeded: the stream is past the name and its arguments. */
	return 0;
}

/* Steps over a number of raw bytes. */
static int
skip_bytes(
	struct drv_acpi_eval *eval,
	size_t count)
{
	/* Refuses to step past the term list. */
	if ((size_t)(eval->end - eval->position) < count)
		return EIO;

	/* Steps over the bytes. */
	eval->position += count;

	/* Succeeded: the stream is past the bytes. */
	return 0;
}

/* Finds the operand shape of an operator. */
static const char *
shape_of(
	unsigned opcode)
{
	size_t index;

	/* Looks the opcode up in the table. */
	for (index = 0; index < sizeof(operator_shapes) / sizeof(operator_shapes[0]); index++) {
		/* Reports the shape of the matching entry. */
		if (operator_shapes[index].opcode == opcode)
			return operator_shapes[index].operands;
	}

	/* Reports an opcode with no known shape. */
	return NULL;
}
