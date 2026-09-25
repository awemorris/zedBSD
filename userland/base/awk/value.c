/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The values of awk and their conversions, the global variables, the
 * arrays, and the cells that hold them.
 *
 * Every struct value is valid at all times (a zeroed one is unset), and
 * every function that sets one frees what it held first.
 */

#include "userland/base/awk/awk.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* The number of hash buckets of the global variables. */
#define VARIABLE_BUCKETS	211

/* The number of hash buckets a new array starts with. */
#define ARRAY_FIRST_BUCKETS	16

/*
 * The global variables, by the hash of their names.  A variable is made
 * the first time the program or the command line names it and lives until
 * awk ends.
 */
static struct variable *variable_buckets[VARIABLE_BUCKETS];

static unsigned long hash_text(const char *text, size_t length);
static int is_blank(char character);
static void array_grow(struct array *array);
static void element_free(struct element *element);

/*
 * Allocates memory, ending awk when there is none.
 */
void *
awk_allocate(
	size_t size)
{
	void *memory;

	/* The memory, zeroed; a request of nothing still gets a block. */
	memory = calloc(1, size + 1U);
	if (memory == NULL)
		awk_fatal("out of memory");

	/* Succeeded. */
	return memory;
}

/*
 * Resizes memory, ending awk when there is none.
 */
void *
awk_reallocate(
	void *memory,
	size_t size)
{
	void *resized;

	/* The resized block. */
	resized = realloc(memory, size + 1U);
	if (resized == NULL)
		awk_fatal("out of memory");

	/* Succeeded. */
	return resized;
}

/*
 * Copies bytes to a new string that ends with a NUL.
 */
char *
awk_copy(
	const char *text,
	size_t length)
{
	char *copy;

	/* The bytes, then the NUL. */
	copy = awk_allocate(length);
	if (length > 0)
		memcpy(copy, text, length);
	copy[length] = '\0';

	/* Succeeded. */
	return copy;
}

/*
 * Reports an error and ends awk: with status 1 for an error in the
 * program text, as gawk does, and 2 for any other.
 */
void
awk_fatal(
	const char *format,
	...)
{
	va_list arguments;

	/* What went wrong, after what has been written so far. */
	fflush(stdout);
	fprintf(stderr, "awk: ");
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fprintf(stderr, "\n");

	/* The status that tells the two apart. */
	if (awk.parsing)
		exit(1);
	exit(2);
}

/*
 * Appends bytes to a buffer.
 */
void
buffer_append(
	struct buffer *buffer,
	const char *data,
	size_t length)
{
	/* Room for the bytes and a NUL. */
	if (buffer->length + length + 1U > buffer->capacity) {
		buffer->capacity = (buffer->length + length + 1U) * 2U + 64U;
		buffer->data = awk_reallocate(buffer->data, buffer->capacity);
	}

	/* The bytes, kept ending with a NUL. */
	if (length > 0)
		memcpy(buffer->data + buffer->length, data, length);
	buffer->length += length;
	buffer->data[buffer->length] = '\0';
}

/*
 * Appends one byte to a buffer.
 */
void
buffer_append_byte(
	struct buffer *buffer,
	char byte)
{
	/* The byte as a string of one. */
	buffer_append(buffer, &byte, 1);
}

/*
 * Frees what a value holds and makes it unset.
 */
void
value_free(
	struct value *value)
{
	/* The text, if any. */
	free(value->text);
	value->type = VALUE_UNSET;
	value->number = 0;
	value->text = NULL;
	value->length = 0;
}

/*
 * Makes a value a number.
 */
void
value_set_number(
	struct value *value,
	double number)
{
	/* The number alone. */
	value_free(value);
	value->type = VALUE_NUMBER;
	value->number = number;
}

/*
 * Makes a value a string, copying the bytes.
 */
void
value_set_text(
	struct value *value,
	const char *text,
	size_t length)
{
	char *copy;

	/* The copy is made first, in case the text is the value's own. */
	copy = awk_copy(text, length);
	value_free(value);
	value->type = VALUE_STRING;
	value->text = copy;
	value->length = length;
}

/*
 * Makes a value a string from the input: a numeric string when it looks
 * like a number, which then compares as one.
 */
void
value_set_input(
	struct value *value,
	const char *text,
	size_t length)
{
	double number;
	int numeric;

	/* The string, then whether it is a number too. */
	value_set_text(value, text, length);
	numeric = text_looks_numeric(value->text, value->length, &number);
	if (numeric) {
		value->type = VALUE_STRNUM;
		value->number = number;
	}
}

/*
 * Copies a value.
 */
void
value_copy(
	struct value *to,
	const struct value *from)
{
	char *copy;

	/* A value is already a copy of itself. */
	if (to == from)
		return;

	/* The text first, then the rest. */
	copy = NULL;
	if (from->text != NULL)
		copy = awk_copy(from->text, from->length);
	value_free(to);
	to->type = from->type;
	to->number = from->number;
	to->text = copy;
	to->length = from->length;
}

/*
 * Moves a value, leaving the source unset.
 */
void
value_move(
	struct value *to,
	struct value *from)
{
	/* A value is already where it is. */
	if (to == from)
		return;

	/* The contents change hands. */
	value_free(to);
	*to = *from;
	from->type = VALUE_UNSET;
	from->number = 0;
	from->text = NULL;
	from->length = 0;
}

/*
 * Returns the numeric value of a value.
 */
double
value_number(
	const struct value *value)
{
	double number;

	/* A number and a numeric string carry their number already. */
	if (value->type == VALUE_NUMBER)
		return value->number;
	if (value->type == VALUE_STRNUM)
		return value->number;

	/* An unset value is 0. */
	if (value->type == VALUE_UNSET)
		return 0;

	/* Succeeded: the number at the start of the string. */
	number = text_number(value->text);
	return number;
}

/*
 * Converts a value to a string: a number goes through OFMT for output and
 * through CONVFMT otherwise, unless it is an integer.
 */
void
value_string(
	const struct value *value,
	int output,
	struct value *string)
{
	struct buffer buffer;
	const char *format;
	size_t format_length;

	/* A string is copied. */
	if (value->type == VALUE_STRING || value->type == VALUE_STRNUM) {
		if (value != string)
			value_set_text(string, value->text, value->length);
		string->type = VALUE_STRING;
		return;
	}

	/* Unset is the empty string. */
	if (value->type == VALUE_UNSET) {
		value_set_text(string, "", 0);
		return;
	}

	/* A number, through the format that applies. */
	format = special_text(SPECIAL_CONVFMT, &format_length);
	if (output)
		format = special_text(SPECIAL_OFMT, &format_length);
	memset(&buffer, 0, sizeof(buffer));
	format_number(value->number, format, &buffer);

	/* Succeeded: the string takes the buffer. */
	value_free(string);
	string->type = VALUE_STRING;
	string->text = buffer.data;
	string->length = buffer.length;
}

/*
 * Returns whether a value is true: a number or numeric string that is not
 * 0, or a string that is not empty.
 */
int
value_truth(
	const struct value *value)
{
	/* A number and a numeric string by their number. */
	if (value->type == VALUE_NUMBER || value->type == VALUE_STRNUM) {
		if (value->number != 0)
			return 1;
		return 0;
	}

	/* A string by whether it has anything in it. */
	if (value->type == VALUE_STRING && value->length > 0)
		return 1;

	/* Unset, or empty. */
	return 0;
}

/*
 * Returns whether a value compares as a number: a number, a numeric
 * string, or unset.
 */
int
value_is_numeric(
	const struct value *value)
{
	/* Everything but a string. */
	if (value->type == VALUE_STRING)
		return 0;
	return 1;
}

/*
 * Compares two values: as numbers when both are numeric, and otherwise as
 * strings, byte by byte.  Returns less than, equal to or more than 0.
 */
int
value_compare(
	const struct value *left,
	const struct value *right)
{
	struct value left_string;
	struct value right_string;
	double left_number;
	double right_number;
	size_t shorter;
	int left_numeric;
	int right_numeric;
	int order;

	/* Two numbers. */
	left_numeric = value_is_numeric(left);
	right_numeric = value_is_numeric(right);
	if (left_numeric && right_numeric) {
		left_number = value_number(left);
		right_number = value_number(right);
		if (left_number < right_number)
			return -1;
		if (left_number > right_number)
			return 1;
		return 0;
	}

	/* Two strings, the common part first, then the lengths. */
	memset(&left_string, 0, sizeof(left_string));
	memset(&right_string, 0, sizeof(right_string));
	value_string(left, 0, &left_string);
	value_string(right, 0, &right_string);
	shorter = left_string.length;
	if (right_string.length < shorter)
		shorter = right_string.length;
	order = 0;
	if (shorter > 0)
		order = memcmp(left_string.text, right_string.text, shorter);
	if (order == 0 && left_string.length < right_string.length)
		order = -1;
	if (order == 0 && left_string.length > right_string.length)
		order = 1;
	value_free(&left_string);
	value_free(&right_string);

	/* Succeeded: the order of the strings. */
	return order;
}

/*
 * Returns the number a string that ends with a NUL starts with (after
 * blanks), or 0.
 */
double
text_number(
	const char *text)
{
	double number;

	/* What strtod reads, which POSIX makes the rule. */
	number = strtod(text, NULL);

	/* Succeeded. */
	return number;
}

/*
 * Returns whether a string is a number with only blanks around it, and the
 * number.  The text ends with a NUL.
 */
int
text_looks_numeric(
	const char *text,
	size_t length,
	double *number)
{
	char *end;
	const char *limit;
	int blank;

	/* The number, which must be there. */
	*number = strtod(text, &end);
	if (end == text)
		return 0;

	/* Only blanks after it. */
	limit = text + length;
	for (; end < limit; end++) {
		blank = is_blank(*end);
		if (!blank)
			return 0;
	}

	/* Succeeded: a number. */
	return 1;
}

/*
 * Appends the string form of a number: an integer as an integer, and any
 * other number through the format.  Returns the length appended.
 */
size_t
format_number(
	double number,
	const char *format,
	struct buffer *buffer)
{
	char small[64];
	char *text;
	double whole;
	int length;
	int negative;
	int not_a_number;
	int infinite;

	/* Not a number, the way gawk writes it. */
	negative = signbit(number);
	not_a_number = isnan(number);
	infinite = isinf(number);
	if (not_a_number) {
		if (negative) {
			buffer_append(buffer, "-nan", 4);
		} else {
			buffer_append(buffer, "+nan", 4);
		}

		/* Four characters. */
		return 4;
	}

	/* Infinity. */
	if (infinite) {
		if (negative) {
			buffer_append(buffer, "-inf", 4);
		} else {
			buffer_append(buffer, "+inf", 4);
		}

		/* Four characters. */
		return 4;
	}

	/* Zero, of either sign, is 0. */
	if (number == 0) {
		buffer_append(buffer, "0", 1);
		return 1;
	}

	/* An integer in all its digits; anything else through the format. */
	whole = floor(number);
	if (number == whole)
		length = snprintf(small, sizeof(small), "%.0f", number);
	else
		length = snprintf(small, sizeof(small), format, number);
	if (length < 0)
		return 0;
	if ((size_t)length < sizeof(small)) {
		buffer_append(buffer, small, (size_t)length);
		return (size_t)length;
	}

	/* A long one, formatted again in a block of its size. */
	text = awk_allocate((size_t)length);
	if (number == whole)
		snprintf(text, (size_t)length + 1U, "%.0f", number);
	else
		snprintf(text, (size_t)length + 1U, format, number);
	buffer_append(buffer, text, (size_t)length);
	free(text);

	/* Succeeded. */
	return (size_t)length;
}

/*
 * Finds a global variable by name, making it when asked.  Returns NULL
 * when it does not exist and is not to be made.
 */
struct variable *
variable_find(
	const char *name,
	int create)
{
	struct variable *variable;
	unsigned long hash;
	size_t length;
	int compare;

	/* The chain the name hashes to. */
	length = strlen(name);
	hash = hash_text(name, length) % VARIABLE_BUCKETS;
	for (variable = variable_buckets[hash];
	     variable != NULL;
	     variable = variable->next) {
		compare = strcmp(variable->name, name);
		if (compare == 0)
			return variable;
	}

	/* An unknown variable, unless it is to be made. */
	if (!create)
		return NULL;

	/* A new unset variable at the head of the chain. */
	variable = awk_allocate(sizeof(*variable));
	variable->name = awk_copy(name, length);
	variable->next = variable_buckets[hash];
	variable_buckets[hash] = variable;

	/* Succeeded. */
	return variable;
}

/*
 * Makes an empty array.
 */
struct array *
array_new(
	void)
{
	struct array *array;

	/* The array and its first buckets. */
	array = awk_allocate(sizeof(*array));
	array->bucket_count = ARRAY_FIRST_BUCKETS;
	array->buckets = awk_allocate(sizeof(*array->buckets) * array->bucket_count);

	/* Succeeded. */
	return array;
}

/*
 * Finds the element of a key, making an unset one when asked.  Returns
 * NULL when there is none and none is to be made.
 */
struct element *
array_find(
	struct array *array,
	const char *key,
	size_t length,
	int create)
{
	struct element *element;
	unsigned long hash;
	size_t bucket;
	int compare;

	/* The chain the key hashes to. */
	hash = hash_text(key, length);
	bucket = hash % array->bucket_count;
	for (element = array->buckets[bucket];
	     element != NULL;
	     element = element->bucket_next) {
		if (element->hash != hash || element->key_length != length)
			continue;
		compare = memcmp(element->key, key, length);
		if (compare == 0)
			return element;
	}

	/* An absent element, unless it is to be made. */
	if (!create)
		return NULL;

	/* Room, when the chains grow long. */
	if (array->count >= array->bucket_count * 2U) {
		array_grow(array);
		bucket = hash % array->bucket_count;
	}

	/* A new unset element in its chain and at the end of the order. */
	element = awk_allocate(sizeof(*element));
	element->key = awk_copy(key, length);
	element->key_length = length;
	element->hash = hash;
	element->bucket_next = array->buckets[bucket];
	array->buckets[bucket] = element;
	element->previous = array->last;
	if (array->last != NULL)
		array->last->next = element;
	else
		array->first = element;
	array->last = element;
	array->count++;

	/* Succeeded. */
	return element;
}

/*
 * Removes the element of a key, if there is one.
 */
void
array_remove(
	struct array *array,
	const char *key,
	size_t length)
{
	struct element **link;
	struct element *element;
	unsigned long hash;
	int compare;

	/* The link to the element in its chain. */
	hash = hash_text(key, length);
	link = &array->buckets[hash % array->bucket_count];
	for (;;) {
		element = *link;
		if (element == NULL)
			return;
		if (element->hash == hash && element->key_length == length) {
			compare = memcmp(element->key, key, length);
			if (compare == 0)
				break;
		}

		/* The next link of the chain. */
		link = &element->bucket_next;
	}

	/* Out of the chain and out of the order. */
	*link = element->bucket_next;
	if (element->previous != NULL)
		element->previous->next = element->next;
	else
		array->first = element->next;
	if (element->next != NULL)
		element->next->previous = element->previous;
	else
		array->last = element->previous;
	array->count--;
	element_free(element);
}

/*
 * Removes every element of an array.
 */
void
array_clear(
	struct array *array)
{
	struct element *element;
	struct element *next;

	/* Every element, along the order. */
	for (element = array->first; element != NULL; element = next) {
		next = element->next;
		element_free(element);
	}

	/* No chains and no order. */
	memset(array->buckets, 0, sizeof(*array->buckets) * array->bucket_count);
	array->first = NULL;
	array->last = NULL;
	array->count = 0;
}

/*
 * Frees an array and its elements.
 */
void
array_free(
	struct array *array)
{
	/* The elements, then the table. */
	array_clear(array);
	free(array->buckets);
	free(array);
}

/*
 * Returns the array a cell holds, making an unset cell an array.  A
 * reference is followed to the caller's cell, which becomes the array.
 */
struct array *
cell_array(
	struct cell *cell)
{
	/* The cell a reference stands for. */
	while (cell->kind == CELL_REFERENCE)
		cell = cell->target;

	/* A scalar cannot be an array. */
	if (cell->kind == CELL_SCALAR)
		awk_fatal("attempt to use a scalar as an array");

	/* An unset cell becomes an array of its own. */
	if (cell->kind == CELL_UNSET) {
		cell->kind = CELL_ARRAY;
		cell->array = array_new();
		cell->owns_array = 1;
	}

	/* Succeeded. */
	return cell->array;
}

/*
 * Reads the scalar of a cell.  A reference reads as unset, since a scalar
 * parameter is passed by value and the caller's was unset.
 */
void
cell_read(
	struct cell *cell,
	struct value *value)
{
	struct cell *target;

	/* A reference: unset, unless the caller's cell became an array. */
	if (cell->kind == CELL_REFERENCE) {
		target = cell->target;
		while (target->kind == CELL_REFERENCE)
			target = target->target;
		if (target->kind == CELL_ARRAY)
			awk_fatal("attempt to use an array in a scalar context");
		value_free(value);
		return;
	}

	/* An array is not a scalar. */
	if (cell->kind == CELL_ARRAY)
		awk_fatal("attempt to use an array in a scalar context");

	/* Unset, or the scalar. */
	if (cell->kind == CELL_UNSET) {
		value_free(value);
		return;
	}

	/* Succeeded. */
	value_copy(value, &cell->value);
}

/*
 * Writes the scalar of a cell.  A reference becomes the parameter's own
 * scalar, leaving the caller's cell alone.
 */
void
cell_write(
	struct cell *cell,
	const struct value *value)
{
	/* An array is not a scalar. */
	if (cell->kind == CELL_ARRAY)
		awk_fatal("attempt to use an array in a scalar context");

	/* The cell holds a scalar from now on. */
	cell->kind = CELL_SCALAR;
	cell->target = NULL;
	value_copy(&cell->value, value);
}

/*
 * Frees what a cell holds and makes it unset.
 */
void
cell_release(
	struct cell *cell)
{
	/* The scalar, and the array when the cell owns it. */
	value_free(&cell->value);
	if (cell->kind == CELL_ARRAY && cell->owns_array)
		array_free(cell->array);
	cell->kind = CELL_UNSET;
	cell->array = NULL;
	cell->owns_array = 0;
	cell->target = NULL;
}

/* Hashes bytes (FNV-1a). */
static unsigned long
hash_text(
	const char *text,
	size_t length)
{
	unsigned long hash;
	size_t index;

	/* Each byte in turn. */
	hash = 2166136261UL;
	for (index = 0; index < length; index++) {
		hash ^= (unsigned char)text[index];
		hash *= 16777619UL;
	}

	/* Succeeded. */
	return hash;
}

/* Returns whether a character separates a number from what follows. */
static int
is_blank(
	char character)
{
	/* Space, tab and newline. */
	if (character == ' ' || character == '\t' || character == '\n')
		return 1;
	return 0;
}

/* Doubles the buckets of an array and rechains its elements. */
static void
array_grow(
	struct array *array)
{
	struct element *element;
	size_t bucket;

	/* Twice the buckets, all empty. */
	free(array->buckets);
	array->bucket_count *= 2U;
	array->buckets = awk_allocate(sizeof(*array->buckets) * array->bucket_count);

	/* Every element into its new chain, along the order. */
	for (element = array->first; element != NULL; element = element->next) {
		bucket = element->hash % array->bucket_count;
		element->bucket_next = array->buckets[bucket];
		array->buckets[bucket] = element;
	}
}

/* Frees an element, its key and its value. */
static void
element_free(
	struct element *element)
{
	/* The value, the key, the element. */
	value_free(&element->value);
	free(element->key);
	free(element);
}
