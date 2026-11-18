/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The AML operators that compute values (ACPI 6.5 section 19.6), and the
 * implicit and explicit conversions between integers, strings and buffers
 * (section 19.3.5).
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "aml-internal.h"
#include "aml-os.h"

/*
 * The comparison codes of Match.
 */
#define MATCH_TRUE	0U
#define MATCH_EQUAL	1U
#define MATCH_LESS_EQUAL	2U
#define MATCH_LESS	3U
#define MATCH_GREATER_EQUAL	4U
#define MATCH_GREATER	5U

/*
 * The end tag of a resource template: its tag byte, and its length with
 * the checksum byte.
 */
#define RESOURCE_END_TAG	0x79U
#define RESOURCE_END_LENGTH	2U

/*
 * How a buffer or an integer is written as a string.
 */
enum string_form {
	STRING_IMPLICIT = 0,
	STRING_HEX = 1,
	STRING_DECIMAL = 2
};

/*
 * The operands of one Match: the package, the two comparisons with their
 * objects, and the index the search starts at.  It holds a reference to
 * each object until match_release().
 */
struct match_request {
	struct drv_acpi_object *package;
	struct drv_acpi_object *first;
	struct drv_acpi_object *second;
	uint64_t first_operation;
	uint64_t second_operation;
	uint64_t start;
};

static int __attribute__((noinline)) op_binary(struct drv_acpi_eval *eval, unsigned opcode, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_divide(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_unary(struct drv_acpi_eval *eval, unsigned opcode, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_step(struct drv_acpi_eval *eval, unsigned opcode, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_logical(struct drv_acpi_eval *eval, unsigned opcode, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_compare(struct drv_acpi_eval *eval, unsigned opcode, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_store(struct drv_acpi_eval *eval, bool copy, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_concat(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_concat_res(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_size_of(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_index(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_ref_of(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_cond_ref_of(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_deref_of(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_object_type(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_match(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_mid(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_to_conversion(struct drv_acpi_eval *eval, unsigned opcode, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_to_string(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_notify(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_time(struct drv_acpi_eval *eval, unsigned opcode, struct drv_acpi_object **result);
static int __attribute__((noinline)) op_fatal(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static uint64_t highest_set_bit(uint64_t operand);
static uint64_t lowest_set_bit(uint64_t operand);
static uint64_t from_bcd(uint64_t operand);
static uint64_t to_bcd(uint64_t operand);
static int copy_into_node(struct drv_acpi_node *node, struct drv_acpi_object *value);
static int concat_integers(struct drv_acpi_object *left, struct drv_acpi_object *right, struct drv_acpi_object **result);
static int concat_strings(struct drv_acpi_object *left, struct drv_acpi_object *right, struct drv_acpi_object **result);
static int concat_buffers(struct drv_acpi_object *left, struct drv_acpi_object *right, struct drv_acpi_object **result);
static int cond_reference(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int node_reference(struct drv_acpi_node *node, struct drv_acpi_object **result);
static int slot_reference(struct drv_acpi_eval *eval, struct drv_acpi_target *target, struct drv_acpi_object **result);
static int deref_path(struct drv_acpi_eval *eval, struct drv_acpi_object *path, struct drv_acpi_object **result);
static int __attribute__((noinline)) debug_object(struct drv_acpi_object **result);
static int new_integer(uint64_t value, struct drv_acpi_object **result);
static uint64_t buffer_integer(const struct drv_acpi_object *buffer);
static int integer_order(struct drv_acpi_object *left, struct drv_acpi_object *right, int *order);
static int target_type(struct drv_acpi_eval *eval, struct drv_acpi_target *target, enum drv_acpi_type *kind);
static int match_operands(struct drv_acpi_eval *eval, struct match_request *request);
static int match_search(const struct match_request *request, uint64_t *answer);
static void match_release(struct match_request *request);
static int explicit_integer(struct drv_acpi_object *source, struct drv_acpi_object **result);
static int explicit_string(struct drv_acpi_object *source, enum string_form form, struct drv_acpi_object **result);
static int reference_object(struct drv_acpi_eval *eval, struct drv_acpi_object *reference, struct drv_acpi_object **result);
static int finish(struct drv_acpi_eval *eval, struct drv_acpi_object *value, struct drv_acpi_object **result);
static int finish_integer(struct drv_acpi_eval *eval, uint64_t value, struct drv_acpi_object **result);
static int target_node(struct drv_acpi_eval *eval, struct drv_acpi_target *target, struct drv_acpi_node **result);
static int target_object(struct drv_acpi_eval *eval, struct drv_acpi_target *target, struct drv_acpi_object **result);
static int slot_object(struct drv_acpi_eval *eval, struct drv_acpi_target *target, struct drv_acpi_object **result);
static int compare_objects(struct drv_acpi_object *left, struct drv_acpi_object *right, int *order);
static int match_one(struct drv_acpi_object *element, uint64_t operation, struct drv_acpi_object *match, bool *hit);
static bool order_matches(uint64_t operation, int order);
static int integer_string(uint64_t value, enum string_form form, struct drv_acpi_object **result);
static int buffer_string(const uint8_t *bytes, size_t length, enum string_form form, struct drv_acpi_object **result);
static size_t byte_text(uint8_t byte, enum string_form form, char *text);
static int string_integer(const char *text, size_t length, bool explicit_form, uint64_t *value);
static size_t template_length(const struct drv_acpi_object *buffer);
static enum drv_acpi_type object_type_of(struct drv_acpi_object *object);
static uint64_t logical(bool truth);


/*
 * Evaluates one operator whose opcode has been read.
 *
 * Reports ENOSYS for an opcode that is not an operator.
 */
int
drv_acpi_eval_operator(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result)
{
	int error;

	/* Chooses the operator by its opcode. */
	switch (opcode) {
	case DRV_ACPI_OP_ADD:
	case DRV_ACPI_OP_SUBTRACT:
	case DRV_ACPI_OP_MULTIPLY:
	case DRV_ACPI_OP_SHIFT_LEFT:
	case DRV_ACPI_OP_SHIFT_RIGHT:
	case DRV_ACPI_OP_AND:
	case DRV_ACPI_OP_NAND:
	case DRV_ACPI_OP_OR:
	case DRV_ACPI_OP_NOR:
	case DRV_ACPI_OP_XOR:
	case DRV_ACPI_OP_MOD:
		error = op_binary(eval, opcode, result);
		break;
	case DRV_ACPI_OP_DIVIDE:
		error = op_divide(eval, result);
		break;
	case DRV_ACPI_OP_NOT:
	case DRV_ACPI_OP_FIND_SET_LEFT_BIT:
	case DRV_ACPI_OP_FIND_SET_RIGHT_BIT:
	case DRV_ACPI_OP_FROM_BCD:
	case DRV_ACPI_OP_TO_BCD:
		error = op_unary(eval, opcode, result);
		break;
	case DRV_ACPI_OP_INCREMENT:
	case DRV_ACPI_OP_DECREMENT:
		error = op_step(eval, opcode, result);
		break;
	case DRV_ACPI_OP_LAND:
	case DRV_ACPI_OP_LOR:
	case DRV_ACPI_OP_LNOT:
		error = op_logical(eval, opcode, result);
		break;
	case DRV_ACPI_OP_LEQUAL:
	case DRV_ACPI_OP_LGREATER:
	case DRV_ACPI_OP_LLESS:
		error = op_compare(eval, opcode, result);
		break;
	case DRV_ACPI_OP_STORE:
		error = op_store(eval, false, result);
		break;
	case DRV_ACPI_OP_COPY_OBJECT:
		error = op_store(eval, true, result);
		break;
	case DRV_ACPI_OP_CONCAT:
		error = op_concat(eval, result);
		break;
	case DRV_ACPI_OP_CONCAT_RES:
		error = op_concat_res(eval, result);
		break;
	case DRV_ACPI_OP_SIZE_OF:
		error = op_size_of(eval, result);
		break;
	case DRV_ACPI_OP_INDEX:
		error = op_index(eval, result);
		break;
	case DRV_ACPI_OP_REF_OF:
		error = op_ref_of(eval, result);
		break;
	case DRV_ACPI_OP_COND_REF_OF:
		error = op_cond_ref_of(eval, result);
		break;
	case DRV_ACPI_OP_DEREF_OF:
		error = op_deref_of(eval, result);
		break;
	case DRV_ACPI_OP_OBJECT_TYPE:
		error = op_object_type(eval, result);
		break;
	case DRV_ACPI_OP_MATCH:
		error = op_match(eval, result);
		break;
	case DRV_ACPI_OP_MID:
		error = op_mid(eval, result);
		break;
	case DRV_ACPI_OP_TO_BUFFER:
	case DRV_ACPI_OP_TO_INTEGER:
	case DRV_ACPI_OP_TO_HEX_STRING:
	case DRV_ACPI_OP_TO_DECIMAL_STRING:
		error = op_to_conversion(eval, opcode, result);
		break;
	case DRV_ACPI_OP_TO_STRING:
		error = op_to_string(eval, result);
		break;
	case DRV_ACPI_OP_NOTIFY:
		error = op_notify(eval, result);
		break;
	case DRV_ACPI_OP_SLEEP:
	case DRV_ACPI_OP_STALL:
	case DRV_ACPI_OP_TIMER:
		error = op_time(eval, opcode, result);
		break;
	case DRV_ACPI_OP_FATAL:
		error = op_fatal(eval, result);
		break;
	case DRV_ACPI_OP_DEBUG:
		/* The debug object read as a value is itself. */
		error = debug_object(result);
		break;
	case DRV_ACPI_OP_ACQUIRE:
	case DRV_ACPI_OP_RELEASE:
	case DRV_ACPI_OP_SIGNAL:
	case DRV_ACPI_OP_WAIT:
	case DRV_ACPI_OP_RESET:
		error = drv_acpi_sync_operator(eval, opcode, result);
		break;
	case DRV_ACPI_OP_LOAD:
	case DRV_ACPI_OP_LOAD_TABLE:
	case DRV_ACPI_OP_UNLOAD:
		error = drv_acpi_table_operator(eval, opcode, result);
		break;
	default:
		/* Reports an opcode that is no operator, for the caller to log. */
		error = ENOSYS;
		break;
	}

	/* Reports a failed operator. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the value. */
	return 0;
}

/*
 * Converts an object to an integer by the implicit rules.
 *
 * A string is read as hexadecimal digits up to the first that is not one;
 * a buffer gives its first bytes, lowest first.
 */
int
drv_acpi_convert_integer(
	struct drv_acpi_object *object,
	uint64_t *value)
{
	int error;

	/* Chooses the conversion by the source's type. */
	switch (object->type) {
	case DRV_ACPI_TYPE_INTEGER:
		/* An integer is its own value. */
		*value = object->value.integer;
		error = 0;
		break;
	case DRV_ACPI_TYPE_STRING:
		/* Reads the hexadecimal digits. */
		error = string_integer(
			object->value.string.text,
			object->value.string.length,
			false,
			value);
		break;
	case DRV_ACPI_TYPE_BUFFER:
		/* Assembles the first bytes. */
		*value = buffer_integer(object);
		error = 0;
		break;
	default:
		/* Refuses a source that has no integer value. */
		drv_acpi_os_log("ACPI: an object of type %u used as an integer\n", (unsigned)object->type);
		error = EINVAL;
		break;
	}

	/* Reports a source that could not be converted. */
	if (error != 0)
		return error;

	/* Succeeded: value holds the integer. */
	return 0;
}

/*
 * Converts an object to a buffer by the implicit rules.
 *
 * An integer gives its bytes, lowest first; a string gives its characters
 * and its terminator.  A buffer is shared.
 */
int
drv_acpi_convert_buffer(
	struct drv_acpi_object *object,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *buffer;
	uint8_t bytes[8];
	uint64_t value;
	unsigned length;
	unsigned index;

	/* Chooses the conversion by the source's type. */
	switch (object->type) {
	case DRV_ACPI_TYPE_BUFFER:
		/* Shares a buffer with a reference of the caller's own. */
		drv_acpi_object_ref(object);
		buffer = object;
		break;
	case DRV_ACPI_TYPE_INTEGER:
		/* Lays the integer out lowest byte first. */
		value = object->value.integer;
		length = drv_acpi_integer_bytes();
		for (index = 0; index < length; index++)
			bytes[index] = (uint8_t)(value >> (index * 8U));

		/* Makes the buffer of the bytes. */
		buffer = drv_acpi_object_buffer_new(bytes, length);
		break;
	case DRV_ACPI_TYPE_STRING:
		/* Makes the buffer of the characters and the terminator. */
		buffer = drv_acpi_object_buffer_new(
			object->value.string.text,
			object->value.string.length + 1U);
		break;
	default:
		/* Refuses a source that has no buffer form. */
		drv_acpi_os_log("ACPI: an object of type %u used as a buffer\n", (unsigned)object->type);
		return EINVAL;
	}

	/* Reports a buffer that could not be allocated. */
	if (buffer == NULL)
		return ENOMEM;

	/* Hands over the buffer. */
	*result = buffer;

	/* Succeeded: the caller holds the buffer. */
	return 0;
}

/*
 * Converts an object to a string by the implicit rules.
 *
 * An integer gives all its hexadecimal digits; a buffer gives each byte
 * as 0x and two hexadecimal digits, separated by spaces.  A string is
 * shared.
 */
int
drv_acpi_convert_string(
	struct drv_acpi_object *object,
	struct drv_acpi_object **result)
{
	int error;

	/* Chooses the conversion by the source's type. */
	switch (object->type) {
	case DRV_ACPI_TYPE_STRING:
		/* Shares a string with a reference of the caller's own. */
		drv_acpi_object_ref(object);
		*result = object;
		error = 0;
		break;
	case DRV_ACPI_TYPE_INTEGER:
		/* Writes the integer's digits. */
		error = integer_string(object->value.integer, STRING_IMPLICIT, result);
		break;
	case DRV_ACPI_TYPE_BUFFER:
		/* Writes the buffer's bytes. */
		error = buffer_string(
			object->value.buffer.bytes,
			object->value.buffer.length,
			STRING_IMPLICIT,
			result);
		break;
	default:
		/* Refuses a source that has no string value. */
		drv_acpi_os_log("ACPI: an object of type %u used as a string\n", (unsigned)object->type);
		error = EINVAL;
		break;
	}

	/* Reports a source that could not be converted. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the string. */
	return 0;
}

/* Runs a two-operand integer operator with a target. */
static int
op_binary(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result)
{
	uint64_t left;
	uint64_t right;
	uint64_t value;
	unsigned width;
	int error;

	/* Evaluates the left operand as an integer. */
	error = drv_acpi_eval_integer(eval, &left);
	if (error != 0)
		return error;

	/* Evaluates the right operand as an integer. */
	error = drv_acpi_eval_integer(eval, &right);
	if (error != 0)
		return error;

	/* Takes the integer width in bits, which bounds the shifts. */
	width = drv_acpi_integer_bytes() * 8U;

	/* Computes the operation. */
	switch (opcode) {
	case DRV_ACPI_OP_ADD:
		value = left + right;
		break;
	case DRV_ACPI_OP_SUBTRACT:
		value = left - right;
		break;
	case DRV_ACPI_OP_MULTIPLY:
		value = left * right;
		break;
	case DRV_ACPI_OP_SHIFT_LEFT:
		/* A shift by the width or more leaves nothing. */
		value = 0;
		if (right < width)
			value = left << right;
		break;
	case DRV_ACPI_OP_SHIFT_RIGHT:
		/* A shift by the width or more leaves nothing. */
		value = 0;
		if (right < width)
			value = (left & drv_acpi_integer_mask()) >> right;
		break;
	case DRV_ACPI_OP_AND:
		value = left & right;
		break;
	case DRV_ACPI_OP_NAND:
		value = ~(left & right);
		break;
	case DRV_ACPI_OP_OR:
		value = left | right;
		break;
	case DRV_ACPI_OP_NOR:
		value = ~(left | right);
		break;
	case DRV_ACPI_OP_XOR:
		value = left ^ right;
		break;
	default:
		/* Mod refuses a zero divisor. */
		if (right == 0) {
			drv_acpi_os_log("ACPI: Mod by zero\n");
			return EDOM;
		}

		/* Mod gives the remainder of the division. */
		value = left % right;
		break;
	}

	/* Stores and reports the result at the integer width. */
	error = finish_integer(eval, value & drv_acpi_integer_mask(), result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the result. */
	return 0;
}

/* Runs a Divide: remainder and quotient each go to their own target. */
static int
op_divide(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *remainder;
	struct drv_acpi_object *ignored;
	uint64_t dividend;
	uint64_t divisor;
	int error;

	/* Evaluates the dividend. */
	error = drv_acpi_eval_integer(eval, &dividend);
	if (error != 0)
		return error;

	/* Evaluates the divisor. */
	error = drv_acpi_eval_integer(eval, &divisor);
	if (error != 0)
		return error;

	/* Refuses a zero divisor. */
	if (divisor == 0) {
		drv_acpi_os_log("ACPI: Divide by zero\n");
		return EDOM;
	}

	/* Makes the remainder. */
	remainder = drv_acpi_object_integer_new(dividend % divisor);
	if (remainder == NULL)
		return ENOMEM;

	/* Stores the remainder into the first target; its value is not Divide's. */
	error = finish(eval, remainder, &ignored);
	if (error != 0)
		return error;
	drv_acpi_object_release(ignored);

	/* Stores and reports the quotient. */
	error = finish_integer(eval, dividend / divisor, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the quotient. */
	return 0;
}

/* Runs Not, FindSetLeftBit, FindSetRightBit, FromBCD or ToBCD. */
static int
op_unary(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result)
{
	uint64_t operand;
	uint64_t value;
	int error;

	/* Evaluates the operand as an integer. */
	error = drv_acpi_eval_integer(eval, &operand);
	if (error != 0)
		return error;

	/* Keeps only the bits an integer has. */
	operand &= drv_acpi_integer_mask();

	/* Computes the operation. */
	switch (opcode) {
	case DRV_ACPI_OP_NOT:
		value = ~operand;
		break;
	case DRV_ACPI_OP_FIND_SET_LEFT_BIT:
		value = highest_set_bit(operand);
		break;
	case DRV_ACPI_OP_FIND_SET_RIGHT_BIT:
		value = lowest_set_bit(operand);
		break;
	case DRV_ACPI_OP_FROM_BCD:
		value = from_bcd(operand);
		break;
	default:
		value = to_bcd(operand);
		break;
	}

	/* Stores and reports the result at the integer width. */
	error = finish_integer(eval, value & drv_acpi_integer_mask(), result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the result. */
	return 0;
}

/* Reports the one-based position of the highest set bit, or zero for none. */
static uint64_t
highest_set_bit(
	uint64_t operand)
{
	unsigned bit;

	/* Tries each bit from the top. */
	for (bit = 64; bit != 0; bit--) {
		/* Stops at the highest set bit. */
		if ((operand >> (bit - 1U)) & 1U)
			return bit;
	}

	/* Reports that no bit is set. */
	return 0;
}

/* Reports the one-based position of the lowest set bit, or zero for none. */
static uint64_t
lowest_set_bit(
	uint64_t operand)
{
	unsigned bit;

	/* Tries each bit from the bottom. */
	for (bit = 1; bit <= 64; bit++) {
		/* Stops at the lowest set bit. */
		if ((operand >> (bit - 1U)) & 1U)
			return bit;
	}

	/* Reports that no bit is set. */
	return 0;
}

/* Reads each nibble as a decimal digit, lowest first. */
static uint64_t
from_bcd(
	uint64_t operand)
{
	uint64_t value;
	uint64_t scale;

	/* Adds each digit at its decimal place. */
	value = 0;
	scale = 1;
	while (operand != 0) {
		value += (operand & 0x0fU) * scale;
		scale *= 10U;
		operand >>= 4;
	}

	/* Reports the decimal number the digits make. */
	return value;
}

/* Writes each decimal digit as a nibble, lowest first. */
static uint64_t
to_bcd(
	uint64_t operand)
{
	uint64_t value;
	unsigned bit;

	/* Puts each digit in the next nibble while there is room. */
	value = 0;
	bit = 0;
	while (operand != 0 && bit < 64) {
		value |= (operand % 10U) << bit;
		operand /= 10U;
		bit += 4U;
	}

	/* Reports the packed digits. */
	return value;
}

/* Runs Increment or Decrement: the operand is read, changed and stored back. */
static int
op_step(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *current;
	struct drv_acpi_object *changed;
	struct drv_acpi_target target;
	uint64_t value;
	int error;

	/* Parses the operand, which is also the target. */
	error = drv_acpi_parse_target(eval, &target);
	if (error != 0)
		return error;

	/* Reads its value. */
	error = target_object(eval, &target, &current);
	if (error != 0) {
		drv_acpi_target_release(&target);
		return error;
	}

	/* Converts the value to an integer. */
	error = drv_acpi_convert_integer(current, &value);
	drv_acpi_object_release(current);
	if (error != 0) {
		drv_acpi_target_release(&target);
		return error;
	}

	/* Moves the value by one. */
	if (opcode == DRV_ACPI_OP_INCREMENT) {
		value++;
	} else {
		value--;
	}

	/* Makes the new value at the integer width. */
	changed = drv_acpi_object_integer_new(value & drv_acpi_integer_mask());
	if (changed == NULL) {
		drv_acpi_target_release(&target);
		return ENOMEM;
	}

	/* Stores it back. */
	error = drv_acpi_store(eval, changed, &target);
	drv_acpi_target_release(&target);
	if (error != 0) {
		drv_acpi_object_release(changed);
		return error;
	}

	/* Hands over the new value. */
	*result = changed;

	/* Succeeded: the caller holds the new value. */
	return 0;
}

/* Runs LAnd, LOr or LNot. */
static int
op_logical(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result)
{
	uint64_t left;
	uint64_t right;
	bool truth;
	int error;

	/* Evaluates the first operand. */
	error = drv_acpi_eval_integer(eval, &left);
	if (error != 0)
		return error;

	/* LNot has only the one. */
	if (opcode == DRV_ACPI_OP_LNOT) {
		truth = false;
		if (left == 0)
			truth = true;
	} else {
		/* Evaluates the second operand; AML does not short-circuit. */
		error = drv_acpi_eval_integer(eval, &right);
		if (error != 0)
			return error;

		/* Combines the two truths. */
		truth = false;
		if (opcode == DRV_ACPI_OP_LAND) {
			if (left != 0 && right != 0)
				truth = true;
		} else {
			if (left != 0 || right != 0)
				truth = true;
		}
	}

	/* Makes the truth value. */
	error = new_integer(logical(truth), result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the truth value. */
	return 0;
}

/* Runs LEqual, LGreater or LLess. */
static int
op_compare(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *left;
	struct drv_acpi_object *right;
	bool truth;
	int order;
	int error;

	/* Evaluates the left operand as data. */
	error = drv_acpi_eval_data(eval, &left);
	if (error != 0)
		return error;

	/* Evaluates the right operand as data. */
	error = drv_acpi_eval_data(eval, &right);
	if (error != 0) {
		drv_acpi_object_release(left);
		return error;
	}

	/* Compares them by the type of the first. */
	error = compare_objects(left, right, &order);
	drv_acpi_object_release(left);
	drv_acpi_object_release(right);
	if (error != 0)
		return error;

	/* Turns the order into the operator's truth. */
	truth = false;
	if (opcode == DRV_ACPI_OP_LEQUAL && order == 0) {
		truth = true;
	} else if (opcode == DRV_ACPI_OP_LGREATER && order > 0) {
		truth = true;
	} else if (opcode == DRV_ACPI_OP_LLESS && order < 0) {
		truth = true;
	}

	/* Makes the truth value. */
	error = new_integer(logical(truth), result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the truth value. */
	return 0;
}

/* Runs Store, or CopyObject when copy is set. */
static int
op_store(
	struct drv_acpi_eval *eval,
	bool copy,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *source;
	struct drv_acpi_target target;
	int error;

	/* Evaluates the source. */
	error = drv_acpi_eval_term_arg(eval, &source);
	if (error != 0)
		return error;

	/* Parses the destination. */
	error = drv_acpi_parse_target(eval, &target);
	if (error != 0) {
		drv_acpi_object_release(source);
		return error;
	}

	/* CopyObject to a named object replaces it; anything else is a Store. */
	if (copy && target.kind == DRV_ACPI_TARGET_NODE) {
		error = copy_into_node(target.node, source);
	} else {
		error = drv_acpi_store(eval, source, &target);
	}

	/* Lets go of the target. */
	drv_acpi_target_release(&target);

	/* Reports a failed store. */
	if (error != 0) {
		drv_acpi_object_release(source);
		return error;
	}

	/* Hands over the source, which is the value of a Store. */
	*result = source;

	/* Succeeded: the caller holds the stored value. */
	return 0;
}

/* Replaces a named object with a copy of a value of any type, as CopyObject does. */
static int
copy_into_node(
	struct drv_acpi_node *node,
	struct drv_acpi_object *value)
{
	struct drv_acpi_object *duplicate;
	int error;

	/* Copies the value. */
	error = drv_acpi_object_copy(value, &duplicate);
	if (error != 0)
		return error;

	/* The node, or the one an alias stands for, takes the copy. */
	node = drv_acpi_ns_resolve_alias(node);
	drv_acpi_ns_attach(node, duplicate);
	drv_acpi_object_release(duplicate);

	/* Succeeded: the node holds a copy of the value. */
	return 0;
}

/*
 * Runs a Concatenate.
 *
 * The first operand's type decides the result: two integers make a buffer
 * of both, and strings and buffers are joined.
 */
static int
op_concat(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *left;
	struct drv_acpi_object *right;
	struct drv_acpi_object *joined;
	int error;

	/* Evaluates the left operand as data. */
	error = drv_acpi_eval_data(eval, &left);
	if (error != 0)
		return error;

	/* Evaluates the right operand as data. */
	error = drv_acpi_eval_data(eval, &right);
	if (error != 0) {
		drv_acpi_object_release(left);
		return error;
	}

	/* Joins them by the first operand's type. */
	switch (left->type) {
	case DRV_ACPI_TYPE_INTEGER:
		error = concat_integers(left, right, &joined);
		break;
	case DRV_ACPI_TYPE_STRING:
		error = concat_strings(left, right, &joined);
		break;
	case DRV_ACPI_TYPE_BUFFER:
		error = concat_buffers(left, right, &joined);
		break;
	default:
		drv_acpi_os_log("ACPI: Concatenate of an object of type %u\n", (unsigned)left->type);
		error = EINVAL;
		break;
	}

	/* Lets go of the operands. */
	drv_acpi_object_release(left);
	drv_acpi_object_release(right);

	/* Reports a failed join. */
	if (error != 0)
		return error;

	/* Stores and reports the joined value. */
	error = finish(eval, joined, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the joined value. */
	return 0;
}

/* Joins two integers into a buffer, each laid out lowest byte first. */
static int
concat_integers(
	struct drv_acpi_object *left,
	struct drv_acpi_object *right,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *joined;
	uint64_t value;
	size_t width;
	size_t index;
	int error;

	/* Converts the second operand. */
	error = drv_acpi_convert_integer(right, &value);
	if (error != 0)
		return error;

	/* Allocates room for both. */
	width = drv_acpi_integer_bytes();
	joined = drv_acpi_object_buffer_new(NULL, width * 2U);
	if (joined == NULL)
		return ENOMEM;

	/* Lays out the first and then the second. */
	for (index = 0; index < width; index++) {
		joined->value.buffer.bytes[index] = (uint8_t)(left->value.integer >> (index * 8U));
		joined->value.buffer.bytes[width + index] = (uint8_t)(value >> (index * 8U));
	}

	/* Hands over the buffer. */
	*result = joined;

	/* Succeeded: the caller holds the buffer of both integers. */
	return 0;
}

/* Joins a string and the string form of another value. */
static int
concat_strings(
	struct drv_acpi_object *left,
	struct drv_acpi_object *right,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *converted;
	struct drv_acpi_object *joined;
	char *text;
	size_t length;
	int error;

	/* Converts the second operand. */
	error = drv_acpi_convert_string(right, &converted);
	if (error != 0)
		return error;

	/* Allocates room for both texts and the terminator. */
	length = left->value.string.length + converted->value.string.length;
	text = drv_acpi_os_alloc(length + 1U);
	if (text == NULL) {
		drv_acpi_object_release(converted);
		return ENOMEM;
	}

	/* Copies the first text and then the second, terminated. */
	kern_memcpy(text, left->value.string.text, left->value.string.length);
	kern_memcpy(text + left->value.string.length, converted->value.string.text, converted->value.string.length);
	text[length] = '\0';

	/* The converted operand is no longer needed. */
	drv_acpi_object_release(converted);

	/* Makes the string from the joined text. */
	joined = drv_acpi_object_string_new_length(text, length);
	drv_acpi_os_free(text);
	if (joined == NULL)
		return ENOMEM;

	/* Hands over the string. */
	*result = joined;

	/* Succeeded: the caller holds the joined string. */
	return 0;
}

/* Joins a buffer and the buffer form of another value. */
static int
concat_buffers(
	struct drv_acpi_object *left,
	struct drv_acpi_object *right,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *converted;
	struct drv_acpi_object *joined;
	size_t left_length;
	size_t right_length;
	int error;

	/* Converts the second operand. */
	error = drv_acpi_convert_buffer(right, &converted);
	if (error != 0)
		return error;

	/* Allocates room for both. */
	left_length = left->value.buffer.length;
	right_length = converted->value.buffer.length;
	joined = drv_acpi_object_buffer_new(NULL, left_length + right_length);
	if (joined == NULL) {
		drv_acpi_object_release(converted);
		return ENOMEM;
	}

	/* Copies the first bytes and then the second. */
	if (left_length != 0)
		kern_memcpy(joined->value.buffer.bytes, left->value.buffer.bytes, left_length);
	if (right_length != 0)
		kern_memcpy(joined->value.buffer.bytes + left_length, converted->value.buffer.bytes, right_length);

	/* The converted operand is no longer needed. */
	drv_acpi_object_release(converted);

	/* Hands over the buffer. */
	*result = joined;

	/* Succeeded: the caller holds the joined buffer. */
	return 0;
}

/*
 * Runs a ConcatenateResTemplate.
 *
 * The result is the two resource templates without their end tags,
 * followed by one end tag with a zero checksum.
 */
static int
op_concat_res(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *left;
	struct drv_acpi_object *right;
	struct drv_acpi_object *joined;
	size_t left_length;
	size_t right_length;
	int error;

	/* Evaluates the first template. */
	error = drv_acpi_eval_data(eval, &left);
	if (error != 0)
		return error;

	/* Evaluates the second template. */
	error = drv_acpi_eval_data(eval, &right);
	if (error != 0) {
		drv_acpi_object_release(left);
		return error;
	}

	/* Refuses operands that are not buffers. */
	if (left->type != DRV_ACPI_TYPE_BUFFER || right->type != DRV_ACPI_TYPE_BUFFER) {
		drv_acpi_object_release(left);
		drv_acpi_object_release(right);
		return EINVAL;
	}

	/* Measures each template up to its end tag. */
	left_length = template_length(left);
	right_length = template_length(right);

	/* Allocates room for both and an end tag. */
	joined = drv_acpi_object_buffer_new(NULL, left_length + right_length + RESOURCE_END_LENGTH);
	if (joined == NULL) {
		drv_acpi_object_release(left);
		drv_acpi_object_release(right);
		return ENOMEM;
	}

	/* Joins them. */
	if (left_length != 0)
		kern_memcpy(joined->value.buffer.bytes, left->value.buffer.bytes, left_length);
	if (right_length != 0)
		kern_memcpy(joined->value.buffer.bytes + left_length, right->value.buffer.bytes, right_length);

	/* Ends the result with a fresh end tag. */
	joined->value.buffer.bytes[left_length + right_length] = RESOURCE_END_TAG;
	joined->value.buffer.bytes[left_length + right_length + 1U] = 0;

	/* The operands are no longer needed. */
	drv_acpi_object_release(left);
	drv_acpi_object_release(right);

	/* Stores and reports the template. */
	error = finish(eval, joined, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the joined template. */
	return 0;
}

/* Runs a SizeOf. */
static int
op_size_of(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;
	struct drv_acpi_target target;
	uint64_t value;
	int error;

	/* Parses the operand. */
	error = drv_acpi_parse_target(eval, &target);
	if (error != 0)
		return error;

	/* Reads the object the operand names. */
	error = target_object(eval, &target, &object);
	drv_acpi_target_release(&target);
	if (error != 0)
		return error;

	/* Measures it by its type. */
	switch (object->type) {
	case DRV_ACPI_TYPE_STRING:
		value = object->value.string.length;
		break;
	case DRV_ACPI_TYPE_BUFFER:
		value = object->value.buffer.length;
		break;
	case DRV_ACPI_TYPE_PACKAGE:
		value = object->value.package.count;
		break;
	default:
		drv_acpi_os_log("ACPI: SizeOf an object of type %u\n", (unsigned)object->type);
		drv_acpi_object_release(object);
		return EINVAL;
	}

	/* The size is known; the object is no longer needed. */
	drv_acpi_object_release(object);

	/* Makes the size. */
	error = new_integer(value, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the size. */
	return 0;
}

/* Runs an Index: a reference to one element of a package, buffer or string. */
static int
op_index(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *source;
	struct drv_acpi_object *reference;
	uint64_t index;
	uint64_t count;
	int error;

	/* Evaluates the container. */
	error = drv_acpi_eval_data(eval, &source);
	if (error != 0)
		return error;

	/* Evaluates the index. */
	error = drv_acpi_eval_integer(eval, &index);
	if (error != 0) {
		drv_acpi_object_release(source);
		return error;
	}

	/* Measures the container. */
	switch (source->type) {
	case DRV_ACPI_TYPE_PACKAGE:
		count = source->value.package.count;
		break;
	case DRV_ACPI_TYPE_BUFFER:
		count = source->value.buffer.length;
		break;
	case DRV_ACPI_TYPE_STRING:
		count = source->value.string.length;
		break;
	default:
		drv_acpi_os_log("ACPI: Index into an object of type %u\n", (unsigned)source->type);
		drv_acpi_object_release(source);
		return EINVAL;
	}

	/* Refuses an index past the end. */
	if (index >= count) {
		drv_acpi_os_log("ACPI: Index %llu past the end (%llu)\n",
				(unsigned long long)index,
				(unsigned long long)count);
		drv_acpi_object_release(source);
		return EINVAL;
	}

	/* Allocates the reference, which keeps the container alive. */
	reference = drv_acpi_object_reference_new(DRV_ACPI_REFERENCE_INDEX);
	if (reference == NULL) {
		drv_acpi_object_release(source);
		return ENOMEM;
	}

	/* Points it at the element; it takes over the reference to the container. */
	reference->value.reference.target = source;
	reference->value.reference.index = (uint32_t)index;

	/* Stores and reports the reference. */
	error = finish(eval, reference, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the reference to the element. */
	return 0;
}

/* Runs a RefOf. */
static int
op_ref_of(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_target target;
	int error;

	/* Parses what the reference is to. */
	error = drv_acpi_parse_target(eval, &target);
	if (error != 0)
		return error;

	/*
	 * A name gives a node reference, and a reference target already is
	 * one, which the caller takes over; a local or an argument gives a
	 * reference to what it holds.
	 */
	if (target.kind == DRV_ACPI_TARGET_NODE) {
		error = node_reference(target.node, result);
	} else if (target.kind == DRV_ACPI_TARGET_REFERENCE) {
		*result = target.reference;
		error = 0;
	} else {
		error = slot_reference(eval, &target, result);
	}

	/* Reports a reference that could not be made. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the reference. */
	return 0;
}

/* Makes a reference to a namespace node. */
static int
node_reference(
	struct drv_acpi_node *node,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *reference;

	/* Allocates the reference. */
	reference = drv_acpi_object_reference_new(DRV_ACPI_REFERENCE_NODE);
	if (reference == NULL)
		return ENOMEM;

	/* Points it at the node and hands it over. */
	reference->value.reference.node = node;
	*result = reference;

	/* Succeeded: the caller holds the reference. */
	return 0;
}

/* Makes a reference to what a local or an argument holds. */
static int
slot_reference(
	struct drv_acpi_eval *eval,
	struct drv_acpi_target *target,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *reference;
	struct drv_acpi_object *object;
	int error;

	/* Reads what the slot holds. */
	error = target_object(eval, target, &object);
	if (error != 0)
		return error;

	/* A reference the slot already holds is passed on; any other object is wrapped. */
	if (object->type == DRV_ACPI_TYPE_REFERENCE) {
		reference = object;
	} else {
		/* Allocates the wrapping reference. */
		reference = drv_acpi_object_reference_new(DRV_ACPI_REFERENCE_OBJECT);
		if (reference == NULL) {
			drv_acpi_object_release(object);
			return ENOMEM;
		}

		/* It takes over the reference to the object. */
		reference->value.reference.target = object;
	}

	/* Hands over the reference. */
	*result = reference;

	/* Succeeded: the caller holds the reference. */
	return 0;
}

/*
 * Runs a CondRefOf.
 *
 * It stores a reference to the named object into the target and reports
 * true, or reports false when the name does not exist.
 */
static int
op_cond_ref_of(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *reference;
	struct drv_acpi_target target;
	bool exists;
	int error;

	/* A name that does not exist is not an error here; anything else is RefOf of it. */
	exists = drv_acpi_stream_at_name(eval);
	if (exists) {
		error = cond_reference(eval, &reference);
	} else {
		error = op_ref_of(eval, &reference);
	}

	/* Reports an operand that could not be evaluated. */
	if (error != 0)
		return error;

	/* Parses the target. */
	error = drv_acpi_parse_target(eval, &target);
	if (error != 0) {
		drv_acpi_object_release(reference);
		return error;
	}

	/* Stores the reference only when the object exists. */
	error = 0;
	if (reference != NULL)
		error = drv_acpi_store(eval, reference, &target);
	drv_acpi_target_release(&target);
	if (error != 0) {
		drv_acpi_object_release(reference);
		return error;
	}

	/* The object exists when there is a reference to it. */
	exists = false;
	if (reference != NULL)
		exists = true;

	/* The reference is in the target now. */
	drv_acpi_object_release(reference);

	/* Makes the truth value. */
	error = new_integer(logical(exists), result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds whether the object exists. */
	return 0;
}

/* Makes a reference to a named object, or reports NULL when the name does not exist. */
static int
cond_reference(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_name name;
	struct drv_acpi_node *node;
	int error;

	/* Starts with no reference, which a name that does not exist leaves. */
	*result = NULL;

	/* Reads the name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* A name that does not resolve gives no reference. */
	error = drv_acpi_ns_lookup(eval->scope, &name, true, &node);
	if (error != 0)
		return 0;

	/* Makes the reference to the node, or to the one an alias stands for. */
	node = drv_acpi_ns_resolve_alias(node);
	error = node_reference(node, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the reference. */
	return 0;
}

/* Runs a DerefOf: the object a reference, or a path string, points at. */
static int
op_deref_of(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *operand;
	int error;

	/* Evaluates the operand. */
	error = drv_acpi_eval_term_arg(eval, &operand);
	if (error != 0)
		return error;

	/* A string is the path of a named object; a reference is followed. */
	if (operand->type == DRV_ACPI_TYPE_STRING) {
		error = deref_path(eval, operand, result);
	} else if (operand->type == DRV_ACPI_TYPE_REFERENCE) {
		error = reference_object(eval, operand, result);
	} else {
		drv_acpi_os_log("ACPI: DerefOf an object of type %u\n", (unsigned)operand->type);
		error = EINVAL;
	}

	/* The operand is no longer needed. */
	drv_acpi_object_release(operand);

	/* Reports an operand that could not be followed. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds what the operand points at. */
	return 0;
}

/* Reads the named object a path string names. */
static int
deref_path(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *path,
	struct drv_acpi_object **result)
{
	struct drv_acpi_node *node;
	int error;

	/* Resolves the path with the search rules of references. */
	error = drv_acpi_lookup_path(eval->scope, path->value.string.text, true, &node);
	if (error != 0)
		return error;

	/* Reads the named object. */
	error = drv_acpi_read_node(eval, node, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the object's value. */
	return 0;
}

/* Runs an ObjectType. */
static int
op_object_type(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_target target;
	enum drv_acpi_type kind;
	int error;

	/* Parses the operand without reading a field or invoking a method. */
	error = drv_acpi_parse_target(eval, &target);
	if (error != 0)
		return error;

	/* Finds the type of the object the operand stands for. */
	error = target_type(eval, &target, &kind);
	drv_acpi_target_release(&target);
	if (error != 0)
		return error;

	/* Makes the type number. */
	error = new_integer((uint64_t)kind, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the type number. */
	return 0;
}

/* Finds the ObjectType of what a target stands for, looking through references. */
static int
target_type(
	struct drv_acpi_eval *eval,
	struct drv_acpi_target *target,
	enum drv_acpi_type *kind)
{
	struct drv_acpi_object *object;
	struct drv_acpi_node *node;
	int error;

	/* A name is the type of its object; the debug object is its own. */
	if (target->kind == DRV_ACPI_TARGET_NODE) {
		node = drv_acpi_ns_resolve_alias(target->node);
		*kind = object_type_of(node->object);
		return 0;
	} else if (target->kind == DRV_ACPI_TARGET_DEBUG) {
		*kind = DRV_ACPI_TYPE_DEBUG;
		return 0;
	}

	/* A local, an argument or a reference is looked into. */
	error = target_object(eval, target, &object);
	if (error != 0)
		return error;

	/* Takes the type of what it holds. */
	*kind = object_type_of(object);

	/* A node reference reports the type of the node's object. */
	node = drv_acpi_object_reference_node(object);
	if (node != NULL) {
		node = drv_acpi_ns_resolve_alias(node);
		*kind = object_type_of(node->object);
	}

	/* The object is no longer needed. */
	drv_acpi_object_release(object);

	/* Succeeded: kind is the ObjectType. */
	return 0;
}

/* Runs a Match: the index of the first package element that satisfies both tests. */
static int
op_match(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct match_request request;
	uint64_t answer;
	int error;

	/* Evaluates the package, the two tests and the start index. */
	kern_memset(&request, 0, sizeof(request));
	error = match_operands(eval, &request);
	if (error != 0) {
		match_release(&request);
		return error;
	}

	/* Searches the package. */
	error = match_search(&request, &answer);
	match_release(&request);
	if (error != 0)
		return error;

	/* Makes the answer: the index, or Ones when nothing matched. */
	error = new_integer(answer, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the answer. */
	return 0;
}

/* Evaluates the operands of a Match. */
static int
match_operands(
	struct drv_acpi_eval *eval,
	struct match_request *request)
{
	int error;

	/* Evaluates the package to search. */
	error = drv_acpi_eval_data(eval, &request->package);
	if (error != 0)
		return error;

	/* Refuses anything but a package. */
	if (request->package->type != DRV_ACPI_TYPE_PACKAGE)
		return EINVAL;

	/* Reads the first comparison code. */
	error = drv_acpi_stream_integer(eval, 1, &request->first_operation);
	if (error != 0)
		return error;

	/* Evaluates the first comparison's object. */
	error = drv_acpi_eval_data(eval, &request->first);
	if (error != 0)
		return error;

	/* Reads the second comparison code. */
	error = drv_acpi_stream_integer(eval, 1, &request->second_operation);
	if (error != 0)
		return error;

	/* Evaluates the second comparison's object. */
	error = drv_acpi_eval_data(eval, &request->second);
	if (error != 0)
		return error;

	/* Evaluates the index to start at. */
	error = drv_acpi_eval_integer(eval, &request->start);
	if (error != 0)
		return error;

	/* Refuses a comparison code Match does not define. */
	if (request->first_operation > MATCH_GREATER || request->second_operation > MATCH_GREATER)
		return EINVAL;

	/* Succeeded: the request holds every operand. */
	return 0;
}

/* Finds the first element from the start index that passes both tests. */
static int
match_search(
	const struct match_request *request,
	uint64_t *answer)
{
	struct drv_acpi_object *element;
	uint64_t index;
	bool first_hit;
	bool second_hit;
	int error;

	/* Starts with Ones, the answer when nothing matches. */
	*answer = drv_acpi_integer_mask();

	/* Tries each element in order. */
	for (index = request->start; index < request->package->value.package.count; index++) {
		/* Tests the first condition. */
		element = request->package->value.package.elements[index];
		error = match_one(element, request->first_operation, request->first, &first_hit);
		if (error != 0)
			return error;

		/* Skips an element that fails it. */
		if (!first_hit)
			continue;

		/* Tests the second condition. */
		error = match_one(element, request->second_operation, request->second, &second_hit);
		if (error != 0)
			return error;

		/* Stops at the first element that passes both. */
		if (second_hit) {
			*answer = index;
			break;
		}
	}

	/* Succeeded: answer is the element's index, or Ones when nothing matched. */
	return 0;
}

/* Releases the objects a Match request holds. */
static void
match_release(
	struct match_request *request)
{
	/* Releases each object; one may be NULL when evaluation stopped early. */
	drv_acpi_object_release(request->package);
	drv_acpi_object_release(request->first);
	drv_acpi_object_release(request->second);
}

/* Runs a Mid: part of a string or a buffer. */
static int
op_mid(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *source;
	struct drv_acpi_object *part;
	uint64_t index;
	uint64_t length;
	uint64_t available;
	int error;

	/* Evaluates the source. */
	error = drv_acpi_eval_data(eval, &source);
	if (error != 0)
		return error;

	/* Evaluates the index. */
	error = drv_acpi_eval_integer(eval, &index);
	if (error != 0) {
		drv_acpi_object_release(source);
		return error;
	}

	/* Evaluates the length. */
	error = drv_acpi_eval_integer(eval, &length);
	if (error != 0) {
		drv_acpi_object_release(source);
		return error;
	}

	/* Measures the source. */
	if (source->type == DRV_ACPI_TYPE_STRING) {
		available = source->value.string.length;
	} else if (source->type == DRV_ACPI_TYPE_BUFFER) {
		available = source->value.buffer.length;
	} else {
		drv_acpi_object_release(source);
		return EINVAL;
	}

	/* Keeps the part inside the source; past the end it is empty. */
	if (index > available)
		index = available;
	if (length > available - index)
		length = available - index;

	/* Copies the part. */
	if (source->type == DRV_ACPI_TYPE_STRING) {
		part = drv_acpi_object_string_new_length(source->value.string.text + index, (size_t)length);
	} else {
		part = drv_acpi_object_buffer_new(source->value.buffer.bytes + index, (size_t)length);
	}

	/* Lets go of the source. */
	drv_acpi_object_release(source);

	/* Reports a part that could not be allocated. */
	if (part == NULL)
		return ENOMEM;

	/* Stores and reports it. */
	error = finish(eval, part, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the part. */
	return 0;
}

/* Runs ToBuffer, ToInteger, ToHexString or ToDecimalString. */
static int
op_to_conversion(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *source;
	struct drv_acpi_object *converted;
	int error;

	/* Evaluates the operand as data. */
	error = drv_acpi_eval_data(eval, &source);
	if (error != 0)
		return error;

	/* Converts it by the operator. */
	switch (opcode) {
	case DRV_ACPI_OP_TO_BUFFER:
		error = drv_acpi_convert_buffer(source, &converted);
		break;
	case DRV_ACPI_OP_TO_INTEGER:
		error = explicit_integer(source, &converted);
		break;
	case DRV_ACPI_OP_TO_DECIMAL_STRING:
		error = explicit_string(source, STRING_DECIMAL, &converted);
		break;
	default:
		error = explicit_string(source, STRING_HEX, &converted);
		break;
	}

	/* Lets go of the source. */
	drv_acpi_object_release(source);

	/* Reports a failed conversion. */
	if (error != 0)
		return error;

	/* Stores and reports the result. */
	error = finish(eval, converted, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the converted value. */
	return 0;
}

/*
 * Converts a value to an integer as ToInteger does: a string is read in
 * decimal, or in hexadecimal after 0x.
 */
static int
explicit_integer(
	struct drv_acpi_object *source,
	struct drv_acpi_object **result)
{
	uint64_t value;
	int error;

	/* Reads a string explicitly and anything else by the implicit rules. */
	if (source->type == DRV_ACPI_TYPE_STRING) {
		error = string_integer(source->value.string.text, source->value.string.length, true, &value);
	} else {
		error = drv_acpi_convert_integer(source, &value);
	}

	/* Reports a value that could not be converted. */
	if (error != 0)
		return error;

	/* Makes the integer at the integer width. */
	error = new_integer(value & drv_acpi_integer_mask(), result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the integer. */
	return 0;
}

/*
 * Converts a value to a string as ToHexString or ToDecimalString does: a
 * string stays as it is, and integers and buffers are written out.
 */
static int
explicit_string(
	struct drv_acpi_object *source,
	enum string_form form,
	struct drv_acpi_object **result)
{
	int error;

	/* Chooses the conversion by the source's type. */
	switch (source->type) {
	case DRV_ACPI_TYPE_STRING:
		/* Shares a string with a reference of the caller's own. */
		drv_acpi_object_ref(source);
		*result = source;
		error = 0;
		break;
	case DRV_ACPI_TYPE_INTEGER:
		/* Writes the integer's digits. */
		error = integer_string(source->value.integer, form, result);
		break;
	case DRV_ACPI_TYPE_BUFFER:
		/* Writes the buffer's bytes. */
		error = buffer_string(source->value.buffer.bytes, source->value.buffer.length, form, result);
		break;
	default:
		/* Refuses a source that has no string form. */
		error = EINVAL;
		break;
	}

	/* Reports a source that could not be converted. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the string. */
	return 0;
}

/* Runs a ToString: the characters of a buffer up to a NUL or a length. */
static int
op_to_string(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *source;
	struct drv_acpi_object *text;
	const uint8_t *terminator;
	uint64_t length;
	size_t available;
	int error;

	/* Evaluates the buffer. */
	error = drv_acpi_eval_data(eval, &source);
	if (error != 0)
		return error;

	/* Evaluates the length. */
	error = drv_acpi_eval_integer(eval, &length);
	if (error != 0) {
		drv_acpi_object_release(source);
		return error;
	}

	/* Refuses a source that is not a buffer. */
	if (source->type != DRV_ACPI_TYPE_BUFFER) {
		drv_acpi_object_release(source);
		return EINVAL;
	}

	/* Takes no more bytes than the length allows. */
	available = source->value.buffer.length;
	if (length < available)
		available = (size_t)length;

	/* Ends the bytes at the first NUL among them. */
	terminator = NULL;
	if (available != 0)
		terminator = kern_memchr(source->value.buffer.bytes, 0, available);
	if (terminator != NULL)
		available = (size_t)(terminator - source->value.buffer.bytes);

	/* Makes the string of the bytes; the buffer is no longer needed. */
	text = drv_acpi_object_string_new_length((const char *)source->value.buffer.bytes, available);
	drv_acpi_object_release(source);
	if (text == NULL)
		return ENOMEM;

	/* Stores and reports the string. */
	error = finish(eval, text, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the string. */
	return 0;
}

/* Runs a Notify. */
static int
op_notify(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_target target;
	struct drv_acpi_node *node;
	uint64_t value;
	int error;

	/* Parses the object to notify. */
	error = drv_acpi_parse_target(eval, &target);
	if (error != 0)
		return error;

	/* Resolves it to its node. */
	error = target_node(eval, &target, &node);
	drv_acpi_target_release(&target);
	if (error != 0)
		return error;

	/* Evaluates the notification value. */
	error = drv_acpi_eval_integer(eval, &value);
	if (error != 0)
		return error;

	/* Hands the notification to the handlers. */
	error = drv_acpi_notify(node, (uint32_t)value);
	if (error != 0)
		return error;

	/* Makes the value of Notify, which has none of its own. */
	error = new_integer(0, result);
	if (error != 0)
		return error;

	/* Succeeded: the handlers saw the notification. */
	return 0;
}

/* Runs Sleep, Stall or Timer. */
static int
op_time(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result)
{
	uint64_t value;
	int error;

	/* Timer reads the clock in 100-nanosecond units. */
	if (opcode == DRV_ACPI_OP_TIMER) {
		value = drv_acpi_os_timer();
	} else {
		/* Evaluates the duration. */
		error = drv_acpi_eval_integer(eval, &value);
		if (error != 0)
			return error;

		/* Sleeps in milliseconds or spins in microseconds. */
		if (opcode == DRV_ACPI_OP_SLEEP) {
			drv_acpi_sleep(value);
		} else {
			drv_acpi_os_stall(value);
		}

		/* Neither has a value of its own. */
		value = 0;
	}

	/* Makes the value. */
	error = new_integer(value, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the time, or zero. */
	return 0;
}

/* Runs a Fatal: firmware reports an error it cannot recover from. */
static int
op_fatal(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	uint64_t type;
	uint64_t code;
	uint64_t argument;
	int error;

	/* Reads the type. */
	error = drv_acpi_stream_integer(eval, 1, &type);
	if (error != 0)
		return error;

	/* Reads the code. */
	error = drv_acpi_stream_integer(eval, 4, &code);
	if (error != 0)
		return error;

	/* Evaluates the argument. */
	error = drv_acpi_eval_integer(eval, &argument);
	if (error != 0)
		return error;

	/* Logs it; the system carries on as other operating systems do. */
	drv_acpi_os_log(
		"ACPI: Fatal type 0x%llx code 0x%llx argument 0x%llx\n",
		(unsigned long long)type,
		(unsigned long long)code,
		(unsigned long long)argument);

	/* Makes the value of Fatal, which has none of its own. */
	error = new_integer(0, result);
	if (error != 0)
		return error;

	/* Succeeded: the report is logged. */
	return 0;
}

/* Makes the debug object, which is the value the Debug operand reads as. */
static int
debug_object(
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;

	/* Allocates the debug object. */
	object = drv_acpi_object_new(DRV_ACPI_TYPE_DEBUG);
	if (object == NULL)
		return ENOMEM;

	/* Hands it over. */
	*result = object;

	/* Succeeded: the caller holds the debug object. */
	return 0;
}

/* Makes an integer object for an operator's value. */
static int
new_integer(
	uint64_t value,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;

	/* Allocates the integer. */
	object = drv_acpi_object_integer_new(value);
	if (object == NULL)
		return ENOMEM;

	/* Hands it over. */
	*result = object;

	/* Succeeded: the caller holds the integer. */
	return 0;
}

/* Assembles an integer from a buffer's first bytes, lowest first, as many as an integer holds. */
static uint64_t
buffer_integer(
	const struct drv_acpi_object *buffer)
{
	uint64_t assembled;
	size_t length;
	size_t width;
	size_t index;

	/* Takes as many bytes as an integer holds. */
	length = buffer->value.buffer.length;
	width = drv_acpi_integer_bytes();
	if (length > width)
		length = width;

	/* Assembles the integer from its bytes, lowest first. */
	assembled = 0;
	for (index = 0; index < length; index++)
		assembled |= (uint64_t)buffer->value.buffer.bytes[index] << (index * 8U);

	/* Reports the assembled integer. */
	return assembled;
}

/* Stores a computed value into the target that follows and hands it back. */
static int
finish(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *value,
	struct drv_acpi_object **result)
{
	struct drv_acpi_target target;
	int error;

	/* Parses the target. */
	error = drv_acpi_parse_target(eval, &target);
	if (error != 0) {
		drv_acpi_object_release(value);
		return error;
	}

	/* Stores the value into it. */
	error = drv_acpi_store(eval, value, &target);
	drv_acpi_target_release(&target);
	if (error != 0) {
		drv_acpi_object_release(value);
		return error;
	}

	/* Hands over the value. */
	*result = value;

	/* Succeeded: the caller holds the value, which the target holds too. */
	return 0;
}

/* Makes an integer, stores it into the target that follows and hands it back. */
static int
finish_integer(
	struct drv_acpi_eval *eval,
	uint64_t value,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;
	int error;

	/* Makes the integer. */
	object = drv_acpi_object_integer_new(value);
	if (object == NULL)
		return ENOMEM;

	/* Stores and reports it. */
	error = finish(eval, object, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the integer. */
	return 0;
}

/* Finds the namespace node a SuperName stands for. */
static int
target_node(
	struct drv_acpi_eval *eval,
	struct drv_acpi_target *target,
	struct drv_acpi_node **result)
{
	struct drv_acpi_object *object;
	struct drv_acpi_node *node;
	int error;

	/* A name is the node; anything else must hold a node reference. */
	if (target->kind == DRV_ACPI_TARGET_NODE) {
		node = target->node;
	} else {
		/* Reads what the target holds. */
		error = target_object(eval, target, &object);
		if (error != 0)
			return error;

		/* Takes the node it refers to. */
		node = drv_acpi_object_reference_node(object);
		drv_acpi_object_release(object);
	}

	/* Refuses a target that names no node. */
	if (node == NULL)
		return EINVAL;

	/* Hands over the node, or the one an alias stands for. */
	*result = drv_acpi_ns_resolve_alias(node);

	/* Succeeded: the caller has the node. */
	return 0;
}

/*
 * Reads the object a target stands for without converting it: a local's or
 * argument's object, a named object's value, or what a reference points at.
 */
static int
target_object(
	struct drv_acpi_eval *eval,
	struct drv_acpi_target *target,
	struct drv_acpi_object **result)
{
	int error;

	/* Chooses the read by the kind of target. */
	switch (target->kind) {
	case DRV_ACPI_TARGET_LOCAL:
	case DRV_ACPI_TARGET_ARGUMENT:
		/* Reads what the slot holds. */
		error = slot_object(eval, target, result);
		break;
	case DRV_ACPI_TARGET_NODE:
		/* Reads the named object. */
		error = drv_acpi_read_node(eval, target->node, result);
		break;
	case DRV_ACPI_TARGET_REFERENCE:
		/* Reads what the reference points at. */
		error = reference_object(eval, target->reference, result);
		break;
	default:
		/* Refuses a target that holds no object. */
		error = EINVAL;
		break;
	}

	/* Reports an object that could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the object. */
	return 0;
}

/* Reads the object a local or an argument holds. */
static int
slot_object(
	struct drv_acpi_eval *eval,
	struct drv_acpi_target *target,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;
	int error;

	/* Finds the slot's object. */
	if (target->kind == DRV_ACPI_TARGET_LOCAL) {
		object = eval->frame->locals[target->index];
	} else {
		object = eval->frame->arguments[target->index];
	}

	/* A slot that was never set has no value. */
	if (object == NULL)
		return EINVAL;

	/* An argument that holds a node reference reads as the node's value; anything else is shared. */
	if (target->kind == DRV_ACPI_TARGET_ARGUMENT &&
	    object->type == DRV_ACPI_TYPE_REFERENCE &&
	    object->value.reference.kind == DRV_ACPI_REFERENCE_NODE) {
		error = drv_acpi_read_node(eval, object->value.reference.node, result);
	} else {
		drv_acpi_object_ref(object);
		*result = object;
		error = 0;
	}

	/* Reports a node that could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the object. */
	return 0;
}

/* Reads what a reference points at. */
static int
reference_object(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *reference,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;
	int error;

	/* Follows the reference by its kind. */
	switch (reference->value.reference.kind) {
	case DRV_ACPI_REFERENCE_NODE:
		/* Reads the node. */
		error = drv_acpi_read_node(eval, reference->value.reference.node, result);
		break;
	case DRV_ACPI_REFERENCE_INDEX:
		/* Reads the element. */
		error = drv_acpi_index_read(eval, reference, result);
		break;
	case DRV_ACPI_REFERENCE_OBJECT:
		/* Shares the object the reference holds. */
		object = reference->value.reference.target;
		drv_acpi_object_ref(object);
		*result = object;
		error = 0;
		break;
	default:
		/* Refuses a name reference that never resolved. */
		error = EINVAL;
		break;
	}

	/* Reports a reference that could not be followed. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds what the reference points at. */
	return 0;
}

/*
 * Compares two objects by the type of the left one: integers by value,
 * strings and buffers byte by byte and then by length.
 */
static int
compare_objects(
	struct drv_acpi_object *left,
	struct drv_acpi_object *right,
	int *order)
{
	struct drv_acpi_object *converted;
	const uint8_t *left_bytes;
	const uint8_t *right_bytes;
	size_t left_length;
	size_t right_length;
	size_t shorter;
	int compared;
	int error;

	/* Integers compare by value. */
	if (left->type == DRV_ACPI_TYPE_INTEGER) {
		error = integer_order(left, right, order);
		if (error != 0)
			return error;

		/* Succeeded: order holds the order of the two values. */
		return 0;
	}

	/* Strings and buffers compare their bytes; the right operand takes the left one's type. */
	if (left->type == DRV_ACPI_TYPE_STRING) {
		/* Converts the right operand to a string. */
		error = drv_acpi_convert_string(right, &converted);
		if (error != 0)
			return error;

		/* Takes the characters of both. */
		left_bytes = (const uint8_t *)left->value.string.text;
		left_length = left->value.string.length;
		right_bytes = (const uint8_t *)converted->value.string.text;
		right_length = converted->value.string.length;
	} else if (left->type == DRV_ACPI_TYPE_BUFFER) {
		/* Converts the right operand to a buffer. */
		error = drv_acpi_convert_buffer(right, &converted);
		if (error != 0)
			return error;

		/* Takes the bytes of both. */
		left_bytes = left->value.buffer.bytes;
		left_length = left->value.buffer.length;
		right_bytes = converted->value.buffer.bytes;
		right_length = converted->value.buffer.length;
	} else {
		drv_acpi_os_log("ACPI: comparison of an object of type %u\n", (unsigned)left->type);
		return EINVAL;
	}

	/* Compares the common part. */
	shorter = left_length;
	if (right_length < shorter)
		shorter = right_length;
	compared = 0;
	if (shorter != 0)
		compared = kern_memcmp(left_bytes, right_bytes, shorter);

	/* The converted operand is no longer needed. */
	drv_acpi_object_release(converted);

	/* Orders by the first differing byte, or by length when there is none. */
	*order = 0;
	if (compared < 0) {
		*order = -1;
	} else if (compared > 0) {
		*order = 1;
	} else if (left_length < right_length) {
		*order = -1;
	} else if (left_length > right_length) {
		*order = 1;
	}

	/* Succeeded: order holds the order of the two values. */
	return 0;
}

/* Orders an integer and another value converted to an integer. */
static int
integer_order(
	struct drv_acpi_object *left,
	struct drv_acpi_object *right,
	int *order)
{
	uint64_t value;
	int error;

	/* Converts the right operand. */
	error = drv_acpi_convert_integer(right, &value);
	if (error != 0)
		return error;

	/* Orders the two values. */
	*order = 0;
	if (left->value.integer < value) {
		*order = -1;
	} else if (left->value.integer > value) {
		*order = 1;
	}

	/* Succeeded: order holds the order of the two values. */
	return 0;
}

/* Tests one package element against one Match condition. */
static int
match_one(
	struct drv_acpi_object *element,
	uint64_t operation,
	struct drv_acpi_object *match,
	bool *hit)
{
	int order;
	int error;

	/* The always-true test needs no comparison. */
	*hit = false;
	if (operation == MATCH_TRUE) {
		*hit = true;
		return 0;
	}

	/* Only data elements can be compared; others never match. */
	if (element == NULL)
		return 0;
	if (element->type != DRV_ACPI_TYPE_INTEGER &&
	    element->type != DRV_ACPI_TYPE_STRING &&
	    element->type != DRV_ACPI_TYPE_BUFFER)
		return 0;

	/*
	 * Compares the element against the match object.  A comparison that
	 * fails -- a match object that does not convert to the element's type,
	 * or memory that runs out while converting it -- counts as no match
	 * and the search goes on, as ACPICA's AcpiExDoMatch treats it; the
	 * error is not reported.
	 */
	error = compare_objects(element, match, &order);
	if (error != 0)
		return 0;

	/* Applies the comparison code. */
	*hit = order_matches(operation, order);

	/* Succeeded: hit says whether the element passes the test. */
	return 0;
}

/* Reports whether an order satisfies a Match comparison code. */
static bool
order_matches(
	uint64_t operation,
	int order)
{
	/* Chooses the test by the comparison code. */
	switch (operation) {
	case MATCH_EQUAL:
		if (order == 0)
			return true;
		break;
	case MATCH_LESS_EQUAL:
		if (order <= 0)
			return true;
		break;
	case MATCH_LESS:
		if (order < 0)
			return true;
		break;
	case MATCH_GREATER_EQUAL:
		if (order >= 0)
			return true;
		break;
	default:
		if (order > 0)
			return true;
		break;
	}

	/* Reports an order that fails the test. */
	return false;
}

/*
 * Writes an integer as a string: all its hexadecimal digits for the
 * implicit form, 0x and the digits without leading zeros for ToHexString,
 * or decimal digits.
 */
static int
integer_string(
	uint64_t value,
	enum string_form form,
	struct drv_acpi_object **result)
{
	static const char digits[] = "0123456789ABCDEF";
	struct drv_acpi_object *object;
	char text[24];
	char reversed[24];
	size_t length;
	size_t count;
	unsigned width;

	/* Writes the digits lowest first. */
	count = 0;
	if (form == STRING_DECIMAL) {
		/* Writes every decimal digit, at least one. */
		do {
			reversed[count] = digits[value % 10U];
			count++;
			value /= 10U;
		} while (value != 0);
	} else if (form == STRING_IMPLICIT) {
		/* Writes every hexadecimal digit of the integer, leading zeros included. */
		width = drv_acpi_integer_bytes() * 2U;
		while (count < width) {
			reversed[count] = digits[value & 0x0fU];
			count++;
			value >>= 4;
		}
	} else {
		/* Writes the hexadecimal digits without leading zeros, at least one. */
		do {
			reversed[count] = digits[value & 0x0fU];
			count++;
			value >>= 4;
		} while (value != 0);
	}

	/* Writes the prefix of ToHexString. */
	length = 0;
	if (form == STRING_HEX) {
		text[0] = '0';
		text[1] = 'x';
		length = 2;
	}

	/* Puts the digits in reading order behind any prefix. */
	while (count != 0) {
		count--;
		text[length] = reversed[count];
		length++;
	}

	/* Makes the string. */
	object = drv_acpi_object_string_new_length(text, length);
	if (object == NULL)
		return ENOMEM;

	/* Hands over the string. */
	*result = object;

	/* Succeeded: the caller holds the integer's digits. */
	return 0;
}

/*
 * Writes a buffer as a string: 0x-prefixed bytes separated by spaces for
 * the implicit form and by commas for ToHexString, and decimal bytes
 * separated by commas.
 */
static int
buffer_string(
	const uint8_t *bytes,
	size_t length,
	enum string_form form,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;
	char *text;
	size_t used;
	size_t index;

	/* Allocates room for the longest form, five characters a byte. */
	text = drv_acpi_os_alloc(length * 5U + 1U);
	if (text == NULL)
		return ENOMEM;

	/* Writes each byte after the separator from the one before. */
	used = 0;
	for (index = 0; index < length; index++) {
		/* The implicit form separates with spaces, the others with commas. */
		if (index != 0 && form == STRING_IMPLICIT) {
			text[used] = ' ';
			used++;
		} else if (index != 0) {
			text[used] = ',';
			used++;
		}

		/* Writes the byte. */
		used += byte_text(bytes[index], form, text + used);
	}

	/* Makes the string. */
	object = drv_acpi_object_string_new_length(text, used);
	drv_acpi_os_free(text);
	if (object == NULL)
		return ENOMEM;

	/* Hands over the string. */
	*result = object;

	/* Succeeded: the caller holds the buffer's text. */
	return 0;
}

/* Writes one byte in decimal or in hexadecimal and reports how many characters it took. */
static size_t
byte_text(
	uint8_t byte,
	enum string_form form,
	char *text)
{
	static const char digits[] = "0123456789ABCDEF";
	size_t used;

	/* Decimal is written without leading zeros. */
	used = 0;
	if (form == STRING_DECIMAL) {
		/* Writes the hundreds, when there are any. */
		if (byte >= 100) {
			text[used] = digits[byte / 100U];
			used++;
		}

		/* Writes the tens, when the number has them. */
		if (byte >= 10) {
			text[used] = digits[(byte / 10U) % 10U];
			used++;
		}

		/* Writes the units. */
		text[used] = digits[byte % 10U];
		used++;
		return used;
	}

	/* Hexadecimal bytes are written with 0x in front, implicit or not. */
	text[used] = '0';
	text[used + 1U] = 'x';
	used += 2U;

	/* Hexadecimal is always two digits. */
	text[used] = digits[byte >> 4];
	text[used + 1U] = digits[byte & 0x0fU];
	used += 2U;

	/* Reports how many characters the byte took. */
	return used;
}

/*
 * Reads an integer from text.  The implicit form reads hexadecimal digits;
 * the explicit form of ToInteger reads decimal, or hexadecimal after 0x.
 * Both stop at the first character that is not a digit.
 */
static int
string_integer(
	const char *text,
	size_t length,
	bool explicit_form,
	uint64_t *value)
{
	uint64_t accumulated;
	unsigned base;
	unsigned digit;
	size_t index;
	char character;

	/* Chooses the base and skips any prefix. */
	base = 16;
	index = 0;
	if (explicit_form) {
		/* Skips leading white space. */
		while (index < length && (text[index] == ' ' || text[index] == '\t'))
			index++;

		/* Takes 0x as a hexadecimal prefix; anything else is decimal. */
		base = 10;
		if (index + 1U < length &&
		    text[index] == '0' &&
		    (text[index + 1U] == 'x' ||
		     text[index + 1U] == 'X')) {
			base = 16;
			index += 2U;
		}
	}

	/* Accumulates the digits until one is not a digit of the base. */
	accumulated = 0;
	for (;
	     index < length;
	     index++) {
		/* Decodes the character as a digit; anything else ends the number. */
		character = text[index];
		if (character >= '0' && character <= '9') {
			digit = (unsigned)(character - '0');
		} else if (character >= 'a' && character <= 'f') {
			digit = (unsigned)(character - 'a') + 10U;
		} else if (character >= 'A' && character <= 'F') {
			digit = (unsigned)(character - 'A') + 10U;
		} else {
			break;
		}

		/* Stops at a digit the base does not have. */
		if (digit >= base)
			break;

		/* Adds the digit at the lowest place. */
		accumulated = accumulated * base + digit;
	}

	/* Hands over the value, cut to the integer width. */
	*value = accumulated & drv_acpi_integer_mask();

	/* Succeeded: value holds the number the text starts with. */
	return 0;
}

/* Measures a resource template up to, not including, its end tag. */
static size_t
template_length(
	const struct drv_acpi_object *buffer)
{
	size_t length;

	/* A template ends with a two-byte end tag. */
	length = buffer->value.buffer.length;
	if (length >= RESOURCE_END_LENGTH &&
	    buffer->value.buffer.bytes[length - RESOURCE_END_LENGTH] == RESOURCE_END_TAG)
		return length - RESOURCE_END_LENGTH;

	/* A buffer without one is taken whole. */
	return length;
}

/* Reports the ObjectType number of an object. */
static enum drv_acpi_type
object_type_of(
	struct drv_acpi_object *object)
{
	/* A missing object is uninitialized. */
	if (object == NULL)
		return DRV_ACPI_TYPE_UNINITIALIZED;

	/* The internal kinds fold into what ObjectType can report. */
	if (object->type == DRV_ACPI_TYPE_SCOPE)
		return DRV_ACPI_TYPE_UNINITIALIZED;
	if (object->type == DRV_ACPI_TYPE_ALIAS)
		return DRV_ACPI_TYPE_UNINITIALIZED;

	/* Reports the object's own type, which ObjectType numbers the same. */
	return (enum drv_acpi_type)object->type;
}

/* Reports the integer AML uses for a truth value. */
static uint64_t
logical(
	bool truth)
{
	/* True is all ones at the integer width. */
	if (truth)
		return drv_acpi_integer_mask();

	/* False is zero. */
	return 0;
}
