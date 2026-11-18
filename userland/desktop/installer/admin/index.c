/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The installer's index, read (index.h).  The text is checked against the
 * SHA-256 the helper was built with before it is believed, and read once,
 * from the copy that was checked.
 */

#include "index.h"

#include "userland/base/common/sha256.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The longest line and the most fields a record has. */
#define INDEX_LINE_MAX		4096U
#define INDEX_FIELDS_MAX	12U

/* How far the walk of the dependencies has got with one unit. */
enum index_visit {
	INDEX_UNSEEN = 0,
	INDEX_OPEN,
	INDEX_DONE
};

/* One line of the index, cut into its fields in place. */
struct index_line {
	char text[INDEX_LINE_MAX];
	char *field[INDEX_FIELDS_MAX];
	unsigned count;
};

static int index_next_line(const char *text, size_t length, size_t *offset, struct index_line *line);
static int index_copy(char *target, size_t capacity, const char *source);
static int index_number(const char *text, uint64_t *value);
static int index_octal(const char *text, unsigned *value);
static int index_hex_nibble(char character, unsigned *value);
static int index_read_unit(const struct index_line *line, struct installer_index *index);
static int index_read_boot(const struct index_line *line, struct installer_index *index);
static int index_link_unit(const struct index_line *line, struct installer_index *index);
static int index_read_setid(const struct index_line *line, struct installer_index *index);
static int index_visit(const struct installer_index *index, unsigned slot, unsigned char *state, unsigned *order, unsigned *order_count);

/*
 * Reads an index's text into a structure.  The units, the boot files and
 * the set-ID files are kept, the licenses and the items passed over, and
 * anything else -- another format, an unknown record, a field too long, a
 * dependency on a unit the index does not have, two units of one name --
 * is refused with EINVAL.
 */
int
index_parse(
	const char *text,
	size_t length,
	struct installer_index *index)
{
	struct index_line *line;
	size_t offset;
	int seen_format;
	int format_error;
	int format_word;
	int format_version;
	int kind_unit;
	int kind_boot;
	int kind_setid;
	int kind_license;
	int kind_item;
	int error;

	/* A missing text or result. */
	if (text == NULL || index == NULL || length > INDEX_BYTES_MAX)
		return EINVAL;

	/* The line is too large for the stack of a set-user-ID program's caller to choose. */
	line = malloc(sizeof(*line));
	if (line == NULL)
		return ENOMEM;
	memset(index, 0, sizeof(*index));
	seen_format = 0;
	error = 0;

	/* The first pass keeps every record but the units' dependencies. */
	offset = 0;
	for (;;) {
		error = index_next_line(text, length, &offset, line);
		if (error != 0)
			break;
		if (line->count == 0U)
			continue;

		/* The format comes first and is the only one this reader knows. */
		if (!seen_format) {
			/* A format line has two fields, "format" and the version 1. */
			format_error = EINVAL;
			if (line->count == 2U) {
				format_word = strcmp(line->field[0], "format");
				format_version = strcmp(line->field[1], "1");
				if (format_word == 0 && format_version == 0)
					format_error = 0;
			}

			/* Refuses an index of another format. */
			if (format_error != 0) {
				error = EINVAL;
				break;
			}

			/* Remembers that the format has been read. */
			seen_format = 1;
			continue;
		}

		/* Each kind of record is read by its own reader or passed over. */
		kind_unit = strcmp(line->field[0], "unit");
		kind_boot = strcmp(line->field[0], "boot");
		kind_setid = strcmp(line->field[0], "setid");
		kind_license = strcmp(line->field[0], "license");
		kind_item = strcmp(line->field[0], "item");
		if (kind_unit == 0) {
			error = index_read_unit(line, index);
		} else if (kind_boot == 0) {
			error = index_read_boot(line, index);
		} else if (kind_setid == 0 || kind_license == 0 || kind_item == 0) {
			error = 0;
		} else {
			error = EINVAL;
		}

		/* Ends the pass on a record that is refused. */
		if (error != 0)
			break;
	}

	/* The end of the text ends the first pass. */
	if (error == ENOENT)
		error = 0;
	if (error == 0 && !seen_format)
		error = EINVAL;

	/* The second pass names each dependency and each set-ID file's unit by its slot. */
	offset = 0;
	while (error == 0) {
		error = index_next_line(text, length, &offset, line);
		if (error != 0)
			break;
		if (line->count == 0U)
			continue;

		/* Names the dependencies of a unit, or the unit of a set-ID file. */
		kind_unit = strcmp(line->field[0], "unit");
		kind_setid = strcmp(line->field[0], "setid");
		if (kind_unit == 0)
			error = index_link_unit(line, index);
		else if (kind_setid == 0)
			error = index_read_setid(line, index);
	}

	/* The end of the text ends the second pass. */
	if (error == ENOENT)
		error = 0;
	free(line);
	if (error != 0) {
		memset(index, 0, sizeof(*index));
		return error;
	}

	/* Succeeded: the index is read. */
	return 0;
}

/*
 * Tells whether a text's SHA-256 is the expected one, given as 64
 * hexadecimal digits.  Returns 0 when it is, EINVAL for a malformed
 * expectation and EBADMSG for another digest.
 */
int
index_digest_matches(
	const char *text,
	size_t length,
	const char *expected)
{
	struct command_sha256_context context;
	uint8_t wanted[32];
	uint8_t digest[32];
	int difference;
	int error;

	/* The expected digest, read from its digits. */
	error = index_hex_digest(expected, wanted);
	if (error != 0)
		return error;

	/* The text's digest. */
	command_sha256_init(&context);
	error = command_sha256_update(&context, (const uint8_t *)text, length);
	if (error != 0)
		return EINVAL;
	command_sha256_final(&context, digest);

	/* Another digest is another index. */
	difference = memcmp(digest, wanted, sizeof(digest));
	if (difference != 0)
		return EBADMSG;

	/* Succeeded: the text is the index the helper was built with. */
	return 0;
}

/* Finds a unit by its name.  Returns 0 and its slot, or ENOENT. */
int
index_find_unit(
	const struct installer_index *index,
	const char *name,
	unsigned *slot)
{
	unsigned i;
	int difference;

	/* Compares the name with each unit's. */
	for (i = 0; i < index->unit_count; i++) {
		difference = strcmp(index->unit[i].name, name);
		if (difference == 0) {
			*slot = i;
			return 0;
		}
	}

	/* No unit has that name. */
	return ENOENT;
}

/*
 * Gives the units to unpack for a choice: the chosen ones and everything
 * they depend on, each after what it depends on and each once.  A cycle
 * of dependencies is refused with ELOOP.
 */
int
index_closure(
	const struct installer_index *index,
	const unsigned *chosen,
	unsigned chosen_count,
	unsigned *order,
	unsigned *order_count)
{
	unsigned char state[INDEX_UNITS_MAX];
	unsigned i;
	int error;

	/* Marks every unit as not yet walked and the order as empty. */
	memset(state, INDEX_UNSEEN, sizeof(state));
	*order_count = 0;

	/* Walks from each chosen unit, which puts its dependencies first. */
	for (i = 0; i < chosen_count; i++) {
		if (chosen[i] >= index->unit_count)
			return EINVAL;
		error = index_visit(index, chosen[i], state, order, order_count);
		if (error != 0)
			return error;
	}

	/* Succeeded: the order is complete. */
	return 0;
}

/*
 * Tells whether a unit may carry a file with a set-user-ID or set-group-ID
 * bit: the index must name that unit, path and mode.  The path is the
 * installed one, from the root.
 */
int
index_setid_allowed(
	const struct installer_index *index,
	unsigned unit,
	const char *path,
	unsigned mode)
{
	const struct index_setid *entry;
	unsigned i;
	int difference;

	/* Looks for the record of exactly that file. */
	for (i = 0; i < index->setid_count; i++) {
		entry = &index->setid[i];
		if (entry->unit != unit)
			continue;
		if (entry->mode != mode)
			continue;
		difference = strcmp(entry->path, path);
		if (difference == 0)
			return 0;
	}

	/* The index does not allow it. */
	return EPERM;
}

/* Reads a SHA-256 written as 64 hexadecimal digits. */
int
index_hex_digest(
	const char *text,
	uint8_t digest[32])
{
	unsigned high;
	unsigned low;
	unsigned i;
	size_t size;
	int error;

	/* Exactly 64 digits. */
	if (text == NULL)
		return EINVAL;
	size = strlen(text);
	if (size != 64U)
		return EINVAL;

	/* Two digits make each byte. */
	for (i = 0; i < 32U; i++) {
		error = index_hex_nibble(text[i * 2U], &high);
		if (error != 0)
			return error;
		error = index_hex_nibble(text[i * 2U + 1U], &low);
		if (error != 0)
			return error;
		digest[i] = (uint8_t)(high << 4 | low);
	}

	/* Succeeded: the digest is read. */
	return 0;
}

/*
 * Copies the next line from an offset and cuts it at its TABs.  Returns
 * ENOENT at the end of the text and EINVAL for a line too long or with too
 * many fields.
 */
static int
index_next_line(
	const char *text,
	size_t length,
	size_t *offset,
	struct index_line *line)
{
	size_t start;
	size_t end;
	size_t size;
	size_t actual;
	char *cursor;

	/* Nothing is left. */
	if (*offset >= length)
		return ENOENT;

	/* Finds the line's end. */
	start = *offset;
	end = start;
	while (end < length && text[end] != '\n')
		end++;
	*offset = end + 1U;

	/* Copies the line, which must fit and must not hold a NUL. */
	size = end - start;
	if (size >= sizeof(line->text))
		return EINVAL;
	memcpy(line->text, text + start, size);
	line->text[size] = '\0';
	actual = strlen(line->text);
	if (actual != size)
		return EINVAL;

	/* An empty line has no field. */
	line->count = 0;
	if (size == 0U)
		return 0;

	/* Cuts the fields at each TAB. */
	cursor = line->text;
	for (;;) {
		if (line->count == INDEX_FIELDS_MAX)
			return EINVAL;
		line->field[line->count++] = cursor;
		cursor = strchr(cursor, '\t');
		if (cursor == NULL)
			break;
		*cursor++ = '\0';
	}

	/* Succeeded: the line is cut. */
	return 0;
}

/* Copies a field that has to fit and must not be empty. */
static int
index_copy(
	char *target,
	size_t capacity,
	const char *source)
{
	size_t size;

	/* The field and its NUL have to fit. */
	size = strlen(source);
	if (size == 0U || size >= capacity)
		return EINVAL;
	memcpy(target, source, size + 1U);

	/* Succeeded: the field is copied. */
	return 0;
}

/* Reads a decimal count. */
static int
index_number(
	const char *text,
	uint64_t *value)
{
	uint64_t result;
	unsigned digit;

	/* At least one digit, and only digits. */
	if (*text == '\0')
		return EINVAL;
	result = 0;

	/* Adds each digit, refusing a count past 64 bits. */
	while (*text != '\0') {
		if (*text < '0' || *text > '9')
			return EINVAL;
		digit = (unsigned)(*text - '0');
		if (result > (UINT64_MAX - digit) / 10U)
			return EINVAL;
		result = result * 10U + digit;
		text++;
	}

	/* Stores the count. */
	*value = result;

	/* Succeeded: the count is read. */
	return 0;
}

/* Reads a mode written in octal, no more than four digits. */
static int
index_octal(
	const char *text,
	unsigned *value)
{
	unsigned result;
	size_t size;

	/* One to four octal digits. */
	size = strlen(text);
	if (size == 0U || size > 4U)
		return EINVAL;
	result = 0;

	/* Adds each digit. */
	while (*text != '\0') {
		if (*text < '0' || *text > '7')
			return EINVAL;
		result = result * 8U + (unsigned)(*text - '0');
		text++;
	}

	/* Stores the mode. */
	*value = result;

	/* Succeeded: the mode is read. */
	return 0;
}

/* Reads one hexadecimal digit, either case. */
static int
index_hex_nibble(
	char character,
	unsigned *value)
{
	/* The ten digits, then the six letters in either case. */
	if (character >= '0' && character <= '9') {
		*value = (unsigned)(character - '0');
		return 0;
	}

	/* A lowercase letter. */
	if (character >= 'a' && character <= 'f') {
		*value = (unsigned)(character - 'a' + 10);
		return 0;
	}

	/* An uppercase letter. */
	if (character >= 'A' && character <= 'F') {
		*value = (unsigned)(character - 'A' + 10);
		return 0;
	}

	/* Not a hexadecimal digit. */
	return EINVAL;
}

/* Reads a unit record but its dependencies, which the second pass names. */
static int
index_read_unit(
	const struct index_line *line,
	struct installer_index *index)
{
	struct index_unit *unit;
	const char *slash;
	unsigned slot;
	int error;

	/* Ten fields, room for one more unit, and a name no other unit has. */
	if (line->count != 10U || index->unit_count == INDEX_UNITS_MAX)
		return EINVAL;
	error = index_find_unit(index, line->field[1], &slot);
	if (error == 0)
		return EINVAL;
	unit = &index->unit[index->unit_count];

	/* The name, the label, the group and the version. */
	error = index_copy(unit->name, sizeof(unit->name), line->field[1]);
	if (error == 0)
		error = index_copy(unit->label, sizeof(unit->label), line->field[2]);
	if (error == 0)
		error = index_copy(unit->group, sizeof(unit->group), line->field[3]);
	if (error == 0)
		error = index_copy(unit->version, sizeof(unit->version), line->field[4]);

	/* The tarball's size, the unpacked size and the entries. */
	if (error == 0)
		error = index_number(line->field[5], &unit->tar_bytes);
	if (error == 0)
		error = index_number(line->field[6], &unit->bytes);
	if (error == 0)
		error = index_number(line->field[7], &unit->entries);

	/* The tarball's SHA-256. */
	if (error == 0)
		error = index_hex_digest(line->field[8], unit->sha256);
	if (error != 0)
		return EINVAL;

	/* A unit's name is a file name of /opt/installer/units. */
	slash = strchr(unit->name, '/');
	if (slash != NULL || unit->name[0] == '.')
		return EINVAL;
	index->unit_count++;

	/* Succeeded: the unit is kept. */
	return 0;
}

/* Reads a boot file's record. */
static int
index_read_boot(
	const struct index_line *line,
	struct installer_index *index)
{
	struct index_boot *boot;
	const char *slash;
	int error;

	/* Five fields and room for one more boot file. */
	if (line->count != 5U || index->boot_count == INDEX_BOOTS_MAX)
		return EINVAL;
	boot = &index->boot[index->boot_count];

	/* The name, where it goes, its size and its SHA-256. */
	error = index_copy(boot->name, sizeof(boot->name), line->field[1]);
	if (error == 0)
		error = index_copy(boot->place, sizeof(boot->place), line->field[2]);
	if (error == 0)
		error = index_number(line->field[3], &boot->bytes);
	if (error == 0)
		error = index_hex_digest(line->field[4], boot->sha256);
	if (error != 0)
		return EINVAL;

	/* A boot file's name is a file name of /opt/installer/boot. */
	slash = strchr(boot->name, '/');
	if (slash != NULL || boot->name[0] == '.')
		return EINVAL;
	index->boot_count++;

	/* Succeeded: the boot file is kept. */
	return 0;
}

/* Names a unit's dependencies by their slots. */
static int
index_link_unit(
	const struct index_line *line,
	struct installer_index *index)
{
	char names[INDEX_LINE_MAX];
	struct index_unit *unit;
	unsigned slot;
	char *name;
	char *next;
	int difference;
	int error;

	/* The unit the first pass kept under this name. */
	error = index_find_unit(index, line->field[1], &slot);
	if (error != 0)
		return EINVAL;
	unit = &index->unit[slot];

	/* A unit without dependencies says "-". */
	difference = strcmp(line->field[9], "-");
	if (difference == 0)
		return 0;

	/* Each name separated by a space is another unit of the index. */
	error = index_copy(names, sizeof(names), line->field[9]);
	if (error != 0)
		return error;
	name = names;
	while (name != NULL) {
		next = strchr(name, ' ');
		if (next != NULL)
			*next++ = '\0';
		error = index_find_unit(index, name, &slot);
		if (error != 0 || unit->dependency_count == INDEX_DEPENDENCIES_MAX)
			return EINVAL;
		unit->dependency[unit->dependency_count++] = slot;
		name = next;
	}

	/* Succeeded: the dependencies are named. */
	return 0;
}

/* Reads a set-ID file's record, its unit named by the slot. */
static int
index_read_setid(
	const struct index_line *line,
	struct installer_index *index)
{
	struct index_setid *entry;
	int error;

	/* Four fields and room for one more. */
	if (line->count != 4U || index->setid_count == INDEX_SETIDS_MAX)
		return EINVAL;
	entry = &index->setid[index->setid_count];

	/* The unit, the installed path and the mode. */
	error = index_find_unit(index, line->field[1], &entry->unit);
	if (error == 0)
		error = index_copy(entry->path, sizeof(entry->path), line->field[2]);
	if (error == 0)
		error = index_octal(line->field[3], &entry->mode);
	if (error != 0)
		return EINVAL;

	/* An installed path starts at the root. */
	if (entry->path[0] != '/')
		return EINVAL;
	index->setid_count++;

	/* Succeeded: the set-ID file is kept. */
	return 0;
}

/* Puts a unit after its dependencies, refusing a cycle. */
static int
index_visit(
	const struct installer_index *index,
	unsigned slot,
	unsigned char *state,
	unsigned *order,
	unsigned *order_count)
{
	unsigned i;
	int error;

	/* A unit already placed, or one still being walked (a cycle). */
	if (state[slot] == INDEX_DONE)
		return 0;
	if (state[slot] == INDEX_OPEN)
		return ELOOP;
	state[slot] = INDEX_OPEN;

	/* Places each dependency first. */
	for (i = 0; i < index->unit[slot].dependency_count; i++) {
		error = index_visit(index, index->unit[slot].dependency[i], state, order, order_count);
		if (error != 0)
			return error;
	}

	/* Places the unit after its dependencies and marks it placed. */
	order[(*order_count)++] = slot;
	state[slot] = INDEX_DONE;

	/* Succeeded: the unit is placed. */
	return 0;
}
