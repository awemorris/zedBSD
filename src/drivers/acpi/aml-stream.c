/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading the AML byte stream: opcodes, package lengths, names, integers
 * and strings (ACPI 6.5 section 20.2).
 *
 * Every read is bounded by the end of the term list being run, so a
 * damaged table ends in an error instead of a read past the table.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "aml-internal.h"

static bool is_lead_name_char(uint8_t byte);
static bool is_name_char(uint8_t byte);
static int read_segments(struct drv_acpi_eval *eval, uint32_t count, struct drv_acpi_name *name);

/*
 * Reads one byte.
 */
int
drv_acpi_stream_byte(
	struct drv_acpi_eval *eval,
	uint8_t *byte)
{
	/* Refuses a read past the end of the term list. */
	if (eval->position >= eval->end)
		return EIO;

	/* Consumes the byte. */
	*byte = *eval->position;
	eval->position++;

	/* Succeeded: byte holds the next byte of the AML. */
	return 0;
}

/*
 * Reads the next byte without consuming it.
 */
int
drv_acpi_stream_peek(
	struct drv_acpi_eval *eval,
	uint8_t *byte)
{
	/* Refuses a read past the end of the term list. */
	if (eval->position >= eval->end)
		return EIO;

	/* Copies the byte and leaves it in the stream. */
	*byte = *eval->position;

	/* Succeeded: byte holds the next byte, which the next read gives again. */
	return 0;
}

/*
 * Reads an opcode.
 *
 * A two-byte opcode is reported as 0x5b00 plus its second byte.
 */
int
drv_acpi_stream_opcode(
	struct drv_acpi_eval *eval,
	unsigned *opcode)
{
	uint8_t first;
	uint8_t second;
	int error;

	/* Reads the first byte. */
	error = drv_acpi_stream_byte(eval, &first);
	if (error != 0)
		return error;

	/* A one-byte opcode is complete. */
	if (first != DRV_ACPI_OP_EXT_PREFIX) {
		*opcode = first;
		return 0;
	}

	/* Reads the second byte of an extended opcode. */
	error = drv_acpi_stream_byte(eval, &second);
	if (error != 0)
		return error;

	/* Combines the two bytes. */
	*opcode = DRV_ACPI_EXT(second);

	/* Succeeded: opcode holds the extended opcode. */
	return 0;
}

/*
 * Reads a little-endian integer of 1, 2, 4 or 8 bytes.
 */
int
drv_acpi_stream_integer(
	struct drv_acpi_eval *eval,
	unsigned bytes,
	uint64_t *value)
{
	uint64_t result;
	unsigned index;

	/* Refuses an integer that runs past the term list. */
	if ((size_t)(eval->end - eval->position) < bytes)
		return EIO;

	/* Assembles the bytes, lowest first. */
	result = 0;
	for (index = 0; index < bytes; index++)
		result |= (uint64_t)eval->position[index] << (index * 8U);

	/* Consumes them and hands over the integer. */
	eval->position += bytes;
	*value = result;

	/* Succeeded: value holds the integer. */
	return 0;
}

/*
 * Reads a PkgLength and reports where the package it measures ends.
 *
 * The length counts from the first byte of the PkgLength itself.
 */
int
drv_acpi_stream_package_length(
	struct drv_acpi_eval *eval,
	const uint8_t **end)
{
	const uint8_t *start;
	uint32_t length;
	int error;

	/* Remembers where the length starts, which is where it counts from. */
	start = eval->position;

	/* Decodes the length. */
	error = drv_acpi_stream_field_length(eval, &length);
	if (error != 0)
		return error;

	/* Refuses a package that ends before its own length or past the list. */
	if (length < (uint32_t)(eval->position - start))
		return EIO;
	if (length > (size_t)(eval->end - start))
		return EIO;

	/* Finds the end of the package. */
	*end = start + length;

	/* Succeeded: end bounds the package. */
	return 0;
}

/*
 * Decodes the PkgLength encoding as a plain number, as field units use it.
 */
int
drv_acpi_stream_field_length(
	struct drv_acpi_eval *eval,
	uint32_t *length)
{
	uint8_t lead;
	uint8_t byte;
	uint32_t result;
	unsigned follow;
	unsigned index;
	int error;

	/* Reads the lead byte. */
	error = drv_acpi_stream_byte(eval, &lead);
	if (error != 0)
		return error;

	/* Its top two bits count the bytes that follow. */
	follow = (unsigned)(lead >> 6);

	/* A one-byte length keeps its value in the low six bits. */
	if (follow == 0) {
		*length = lead & 0x3fU;
		return 0;
	}

	/* A longer length has its low four bits in the lead byte. */
	result = lead & 0x0fU;

	/* Each following byte adds the next eight bits. */
	for (index = 0; index < follow; index++) {
		/* Reads the next byte of the length. */
		error = drv_acpi_stream_byte(eval, &byte);
		if (error != 0)
			return error;

		/* Puts its bits above the ones read so far. */
		result |= (uint32_t)byte << (4U + index * 8U);
	}

	/* Hands over the length. */
	*length = result;

	/* Succeeded: length holds the decoded number. */
	return 0;
}

/*
 * Reads a NameString.
 */
int
drv_acpi_stream_name(
	struct drv_acpi_eval *eval,
	struct drv_acpi_name *name)
{
	uint8_t byte;
	uint8_t count;
	bool lead;
	int error;

	/* Starts with an empty relative name. */
	kern_memset(name, 0, sizeof(*name));

	/* Looks at the first byte. */
	error = drv_acpi_stream_peek(eval, &byte);
	if (error != 0)
		return error;

	/* Reads the root prefix, or any number of parent prefixes. */
	if (byte == DRV_ACPI_OP_ROOT_CHAR) {
		name->root = true;
		eval->position++;
	} else {
		/* Counts the parent prefixes. */
		while (byte == DRV_ACPI_OP_PARENT_PREFIX) {
			name->parents++;
			eval->position++;

			/* Looks at the byte after this prefix. */
			error = drv_acpi_stream_peek(eval, &byte);
			if (error != 0)
				return error;
		}
	}

	/* Reads the byte that says what kind of name path follows. */
	error = drv_acpi_stream_peek(eval, &byte);
	if (error != 0)
		return error;

	/* Chooses the number of segments by the path's first byte. */
	switch (byte) {
	case 0x00U:
		/* The null name: prefixes only. */
		eval->position++;
		return 0;
	case DRV_ACPI_OP_DUAL_NAME_PREFIX:
		/* Two segments follow the prefix. */
		eval->position++;
		count = 2;
		break;
	case DRV_ACPI_OP_MULTI_NAME_PREFIX:
		/* The byte after the prefix counts the segments. */
		eval->position++;
		error = drv_acpi_stream_byte(eval, &count);
		if (error != 0)
			return error;
		break;
	default:
		/* A single segment must start with a lead name character. */
		lead = is_lead_name_char(byte);
		if (!lead)
			return EIO;
		count = 1;
		break;
	}

	/* Reads the segments. */
	error = read_segments(eval, count, name);
	if (error != 0)
		return error;

	/* Succeeded: name refers to the segments in the table. */
	return 0;
}

/*
 * Reports whether the next byte starts a NameString.
 */
bool
drv_acpi_stream_at_name(
	const struct drv_acpi_eval *eval)
{
	uint8_t byte;
	bool lead;

	/* Nothing starts at the end of the term list. */
	if (eval->position >= eval->end)
		return false;

	/* A prefix starts a name. */
	byte = *eval->position;
	if (byte == DRV_ACPI_OP_ROOT_CHAR || byte == DRV_ACPI_OP_PARENT_PREFIX)
		return true;
	if (byte == DRV_ACPI_OP_DUAL_NAME_PREFIX || byte == DRV_ACPI_OP_MULTI_NAME_PREFIX)
		return true;

	/* So does a lead name character; nothing else does. */
	lead = is_lead_name_char(byte);
	if (lead)
		return true;

	/* Reports a byte that starts something else. */
	return false;
}

/*
 * Reads a NUL-terminated ASCII string and reports it in place.
 */
int
drv_acpi_stream_string(
	struct drv_acpi_eval *eval,
	const char **text,
	size_t *length)
{
	const uint8_t *start;
	const uint8_t *terminator;

	/* Finds the terminator inside the term list. */
	start = eval->position;
	terminator = kern_memchr(start, 0, (size_t)(eval->end - start));
	if (terminator == NULL)
		return EIO;

	/* Consumes the characters and the terminator. */
	eval->position = terminator + 1;

	/* Hands over the text in place. */
	*text = (const char *)start;
	*length = (size_t)(terminator - start);

	/* Succeeded: the text stays in the table. */
	return 0;
}

/* Reports whether a byte may start a name segment. */
static bool
is_lead_name_char(
	uint8_t byte)
{
	/* Upper-case letters start a segment. */
	if (byte >= 'A' && byte <= 'Z')
		return true;

	/* So does the underscore. */
	if (byte == '_')
		return true;

	/* Reports a byte that cannot start a segment. */
	return false;
}

/* Reports whether a byte may continue a name segment. */
static bool
is_name_char(
	uint8_t byte)
{
	bool lead;

	/* Digits continue a segment. */
	if (byte >= '0' && byte <= '9')
		return true;

	/* So does anything that could start one. */
	lead = is_lead_name_char(byte);
	if (lead)
		return true;

	/* Reports a byte that cannot continue a segment. */
	return false;
}

/* Reads a number of four-character segments and checks their characters. */
static int
read_segments(
	struct drv_acpi_eval *eval,
	uint32_t count,
	struct drv_acpi_name *name)
{
	size_t bytes;
	size_t index;
	bool valid;

	/* Refuses segments that run past the term list. */
	bytes = (size_t)count * 4U;
	if ((size_t)(eval->end - eval->position) < bytes)
		return EIO;

	/* Checks every character, the first of each segment more strictly. */
	for (index = 0; index < bytes; index++) {
		/* The first character of a segment has its own class. */
		if (index % 4U == 0) {
			valid = is_lead_name_char(eval->position[index]);
		} else {
			valid = is_name_char(eval->position[index]);
		}

		/* Refuses a byte no name may contain. */
		if (!valid)
			return EIO;
	}

	/* Points the name at the segments and consumes them. */
	name->segments = eval->position;
	name->count = count;
	eval->position += bytes;

	/* Succeeded: the segments stay in the table. */
	return 0;
}
