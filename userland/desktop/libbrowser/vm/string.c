/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Strings in the heap.
 *
 * A string holds Latin-1 bytes when every unit is below 256 and UTF-16
 * units otherwise; the makers narrow automatically, so two strings with
 * the same characters always have the same form.  The characters follow
 * the header in the same cell and never change.
 */

#include "vm/internal.h"

#include <assert.h>
#include <string.h>

/* The longest string, in units (lengths are 32-bit and JavaScript caps them lower). */
#define STRING_MAX_LENGTH	((1U << 30) - 1U)

static int string_fits_latin1(const uint16_t *units, size_t length);

/*
 * The type of every string cell: a string refers to no other cell and
 * owns nothing outside its cell.
 */
const struct vm_cell_type vm_string_type = {
	"string",
	NULL,
	NULL
};

/*
 * Makes a string of length Latin-1 bytes.
 */
struct vm_string *
vm_string_from_latin1(
	struct vm_heap *heap,
	const unsigned char *bytes,
	size_t length)
{
	struct vm_string *string;

	/* Allocates a narrow string and copies the bytes. */
	string = vm_string_alloc(heap, length, 0);
	if (string == NULL)
		return NULL;
	if (length != 0)
		memcpy(vm_string_latin1_mutable(string), bytes, length);

	/* Succeeded: the string holds the bytes. */
	return string;
}

/*
 * Makes a string of length UTF-16 units, narrowed to Latin-1 when every
 * unit is below 256.
 */
struct vm_string *
vm_string_from_units(
	struct vm_heap *heap,
	const uint16_t *units,
	size_t length)
{
	struct vm_string *string;
	unsigned char *narrow;
	size_t index;
	int fits;

	/* Allocates the narrow form when the units allow it. */
	fits = string_fits_latin1(units, length);
	string = vm_string_alloc(heap, length, !fits);
	if (string == NULL)
		return NULL;

	/* Copies the units, narrowing each to a byte in the narrow form. */
	if (fits) {
		narrow = vm_string_latin1_mutable(string);
		for (index = 0; index < length; index++)
			narrow[index] = (unsigned char)units[index];
	} else {
		memcpy(vm_string_units_mutable(string), units, length * sizeof(*units));
	}

	/* Succeeded: the string holds the units. */
	return string;
}

/*
 * Makes a string from UTF-8 bytes (malformed sequences become U+FFFD).
 */
struct vm_string *
vm_string_from_utf8(
	struct vm_heap *heap,
	const char *bytes,
	size_t length)
{
	struct vm_string *string;
	struct wb_units units;
	int error;

	/* Decodes the bytes into UTF-16. */
	wb_units_init(&units);
	error = wb_utf8_to_units((const unsigned char *)bytes, length, &units);
	if (error != 0) {
		wb_units_release(&units);
		return NULL;
	}

	/* Makes the string from the units and frees them. */
	string = vm_string_from_units(heap, units.data, units.length);
	wb_units_release(&units);
	if (string == NULL)
		return NULL;

	/* Succeeded: the string holds the decoded text. */
	return string;
}

/*
 * Makes the string of left followed by right.
 */
struct vm_string *
vm_string_concat(
	struct vm_heap *heap,
	const struct vm_string *left,
	const struct vm_string *right)
{
	struct vm_string *string;
	uint16_t *wide;
	size_t length;
	size_t index;
	int is_wide;

	/* Refuses a result longer than a string may be. */
	length = (size_t)left->length + right->length;
	if (length > STRING_MAX_LENGTH)
		return NULL;

	/* The result is wide when either side is. */
	is_wide = 0;
	if ((left->flags & VM_STRING_WIDE) != 0 || (right->flags & VM_STRING_WIDE) != 0)
		is_wide = 1;
	string = vm_string_alloc(heap, length, is_wide);
	if (string == NULL)
		return NULL;

	/* Two narrow sides are copied as bytes. */
	if (!is_wide) {
		memcpy(vm_string_latin1_mutable(string), vm_string_latin1(left), left->length);
		memcpy(vm_string_latin1_mutable(string) + left->length, vm_string_latin1(right), right->length);
		return string;
	}

	/* Otherwise every unit is copied as UTF-16. */
	wide = vm_string_units_mutable(string);
	for (index = 0; index < left->length; index++)
		wide[index] = vm_string_at(left, index);
	for (index = 0; index < right->length; index++)
		wide[left->length + index] = vm_string_at(right, index);

	/* Succeeded: the string holds both sides. */
	return string;
}

/*
 * Finds the Latin-1 bytes of a narrow string.
 */
const unsigned char *
vm_string_latin1(
	const struct vm_string *string)
{
	assert((string->flags & VM_STRING_WIDE) == 0);

	/* The bytes follow the header. */
	return (const unsigned char *)(string + 1);
}

/*
 * Finds the UTF-16 units of a wide string.
 */
const uint16_t *
vm_string_units(
	const struct vm_string *string)
{
	assert((string->flags & VM_STRING_WIDE) != 0);

	/* The units follow the header. */
	return (const uint16_t *)(const void *)(string + 1);
}

/*
 * Reads the unit at index, whatever the string's form.
 */
uint16_t
vm_string_at(
	const struct vm_string *string,
	size_t index)
{
	assert(index < string->length);

	/* A wide string stores the unit. */
	if ((string->flags & VM_STRING_WIDE) != 0)
		return ((const uint16_t *)(const void *)(string + 1))[index];

	/* A narrow string stores its low byte. */
	return ((const unsigned char *)(string + 1))[index];
}

/*
 * Reports the string's hash, computing it the first time.
 *
 * A narrow and a wide string with the same characters hash alike.
 */
uint32_t
vm_string_hash(
	struct vm_string *string)
{
	/* A hash computed before is kept in the string. */
	if ((string->flags & VM_STRING_HASHED) != 0)
		return string->hash;

	/* Hashes the characters in their form. */
	if ((string->flags & VM_STRING_WIDE) != 0) {
		string->hash = wb_hash_units(vm_string_units(string), string->length);
	} else {
		string->hash = wb_hash_bytes(vm_string_latin1(string), string->length);
	}

	/* The flag says the hash field is filled; the characters never change, so it stays right. */
	string->flags |= VM_STRING_HASHED;

	/* Reports the hash. */
	return string->hash;
}

/*
 * Tells whether two strings have the same characters.
 */
int
vm_string_equal(
	const struct vm_string *left,
	const struct vm_string *right)
{
	int differs;

	/* The same cell, and two atoms that are not the same cell, answer at once. */
	if (left == right)
		return 1;
	if ((left->flags & VM_STRING_ATOM) != 0 && (right->flags & VM_STRING_ATOM) != 0)
		return 0;

	/* Different lengths or forms cannot hold the same characters (the makers narrow). */
	if (left->length != right->length)
		return 0;
	if ((left->flags & VM_STRING_WIDE) != (right->flags & VM_STRING_WIDE))
		return 0;

	/* Compares the characters. */
	if ((left->flags & VM_STRING_WIDE) != 0) {
		differs = memcmp(vm_string_units(left), vm_string_units(right), left->length * sizeof(uint16_t));
	} else {
		differs = memcmp(vm_string_latin1(left), vm_string_latin1(right), left->length);
	}

	/* A difference anywhere means the strings differ. */
	if (differs != 0)
		return 0;

	/* The characters are the same. */
	return 1;
}

/*
 * Tells whether a string holds exactly the given ASCII text.
 */
int
vm_string_equal_ascii(
	const struct vm_string *string,
	const char *ascii)
{
	uint16_t unit;
	size_t length;
	size_t index;

	/* The lengths must agree. */
	length = strlen(ascii);
	if (length != string->length)
		return 0;

	/* Compares unit by unit. */
	for (index = 0; index < length; index++) {
		/* One differing unit settles it. */
		unit = vm_string_at(string, index);
		if (unit != (unsigned char)ascii[index])
			return 0;
	}

	/* Every unit matched. */
	return 1;
}

/*
 * Tells whether a string holds exactly length UTF-16 units.
 */
int
vm_string_equal_units(
	const struct vm_string *string,
	const uint16_t *units,
	size_t length)
{
	uint16_t unit;
	size_t index;

	/* The lengths must agree. */
	if (length != string->length)
		return 0;

	/* Compares unit by unit. */
	for (index = 0; index < length; index++) {
		/* One differing unit settles it. */
		unit = vm_string_at(string, index);
		if (unit != units[index])
			return 0;
	}

	/* Every unit matched. */
	return 1;
}

/*
 * Compares two strings by their UTF-16 units, as JavaScript's < does.
 *
 * Returns a negative number, zero or a positive number.
 */
int
vm_string_compare(
	const struct vm_string *left,
	const struct vm_string *right)
{
	uint16_t left_unit;
	uint16_t right_unit;
	size_t length;
	size_t index;

	/* Compares the units the two share. */
	length = left->length;
	if (right->length < length)
		length = right->length;
	for (index = 0; index < length; index++) {
		left_unit = vm_string_at(left, index);
		right_unit = vm_string_at(right, index);

		/* The first difference decides. */
		if (left_unit != right_unit)
			return (int)left_unit - (int)right_unit;
	}

	/* A prefix sorts first. */
	if (left->length < right->length)
		return -1;
	if (left->length > right->length)
		return 1;

	/* The strings are equal. */
	return 0;
}

/*
 * Appends the string to a byte buffer as UTF-8 (lone surrogates become
 * U+FFFD).
 */
int
vm_string_to_utf8(
	const struct vm_string *string,
	struct wb_buffer *buffer)
{
	const unsigned char *narrow;
	size_t index;
	int error;

	/* A wide string is encoded from its UTF-16. */
	if ((string->flags & VM_STRING_WIDE) != 0) {
		error = wb_units_to_utf8(vm_string_units(string), string->length, buffer);
		if (error != 0)
			return error;

		/* Succeeded: the text is at the end of the buffer. */
		return 0;
	}

	/* A narrow string is encoded byte by byte. */
	narrow = vm_string_latin1(string);
	for (index = 0; index < string->length; index++) {
		error = wb_buffer_append_utf8(buffer, narrow[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the text is at the end of the buffer. */
	return 0;
}

/*
 * Appends the string's characters to a list of UTF-16 units.
 */
int
vm_string_append_units(
	const struct vm_string *string,
	struct wb_units *units)
{
	const unsigned char *narrow;
	uint16_t unit;
	size_t index;
	int error;

	/* A wide string's units as they are. */
	if ((string->flags & VM_STRING_WIDE) != 0) {
		error = wb_units_append(units, vm_string_units(string), string->length);
		if (error != 0)
			return error;
		return 0;
	}

	/* A narrow string's bytes, each a unit. */
	narrow = vm_string_latin1(string);
	for (index = 0; index < string->length; index++) {
		unit = narrow[index];
		error = wb_units_append(units, &unit, 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the characters are at the end of the list. */
	return 0;
}

/*
 * Allocates a string of length units in the narrow or the wide form; the
 * characters are left zero for the caller to fill.
 */
struct vm_string *
vm_string_alloc(
	struct vm_heap *heap,
	size_t length,
	int wide)
{
	struct vm_string *string;
	size_t unit_size;

	/* Refuses a string longer than a string may be. */
	if (length > STRING_MAX_LENGTH)
		return NULL;

	/* Allocates the header and the characters in one cell. */
	unit_size = 1;
	if (wide)
		unit_size = 2;
	string = vm_heap_alloc(heap, &vm_string_type, sizeof(*string) + length * unit_size);
	if (string == NULL)
		return NULL;

	/* Records the length and the form. */
	string->length = (uint32_t)length;
	if (wide)
		string->flags = VM_STRING_WIDE;

	/* Succeeded: the caller fills the characters. */
	return string;
}

/*
 * Finds the bytes of a narrow string that is still being filled.
 */
unsigned char *
vm_string_latin1_mutable(
	struct vm_string *string)
{
	/* The bytes follow the header. */
	return (unsigned char *)(string + 1);
}

/*
 * Finds the units of a wide string that is still being filled.
 */
uint16_t *
vm_string_units_mutable(
	struct vm_string *string)
{
	/* The units follow the header. */
	return (uint16_t *)(void *)(string + 1);
}

/* Tells whether every unit is below 256, so the string can be narrow. */
static int
string_fits_latin1(
	const uint16_t *units,
	size_t length)
{
	size_t index;

	/* Looks for a unit that needs more than a byte. */
	for (index = 0; index < length; index++) {
		/* One wide unit makes the whole string wide. */
		if (units[index] > 0xffU)
			return 0;
	}

	/* Every unit fits a byte. */
	return 1;
}
