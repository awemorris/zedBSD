/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The records and fields of awk, the main input, and the special
 * variables that shape them.
 *
 * $0 is split into fields as soon as it is set, with FS as it is then, so
 * that a change of FS applies from the next record.  Setting a field or NF
 * joins the fields again with OFS.
 */

#include "userland/base/awk/awk.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* One field found in a string: where it starts and how long it is. */
struct piece {
	size_t start;
	size_t length;
};

/* The fields found in a string. */
struct pieces {
	struct piece *items;
	size_t count;
	size_t capacity;
};

/*
 * The pieces of the last split, reused by every split so that splitting a
 * record does not allocate each time.  It lives until awk ends.
 */
static struct pieces split_pieces;

static void split_record(void);
static void split_string(const char *text, size_t length, const char *separator, size_t separator_length, regex_t *regex, int paragraph);
static void split_blanks(const char *text, size_t length);
static void split_character(const char *text, size_t length, char separator, int paragraph);
static void split_regex(const char *text, size_t length, regex_t *regex);
static void add_piece(size_t start, size_t length);
static void rebuild_record(void);
static void set_field_count_variable(void);
static void ensure_fields(size_t count);
static int open_next_input(void);
static void count_record(void);
static void unescape(const char *text, size_t length, struct buffer *buffer);
static int is_blank(char character);

/*
 * Sets $0 from the input, and splits it into fields.
 */
void
record_set(
	const char *text,
	size_t length)
{
	/* The record, then its fields. */
	value_set_input(&awk.record, text, length);
	split_record();
}

/*
 * Sets $0 to a value, and splits it into fields.
 */
void
record_set_value(
	const struct value *value)
{
	struct value string;

	/* The string form of the value, as input. */
	memset(&string, 0, sizeof(string));
	value_string(value, 0, &string);
	record_set(string.text, string.length);
	value_free(&string);
}

/*
 * Sets NF: drops fields past it or adds empty ones, and joins $0 again.
 */
void
record_set_field_count(
	size_t count)
{
	size_t index;

	/* Empty fields up to the new count. */
	ensure_fields(count);
	for (index = awk.field_count + 1U; index <= count; index++)
		value_set_text(&awk.fields[index], "", 0);

	/* No fields past it. */
	for (index = count + 1U; index <= awk.field_count; index++)
		value_free(&awk.fields[index]);

	/* Succeeded: the new count and the record joined again. */
	awk.field_count = count;
	set_field_count_variable();
	rebuild_record();
}

/*
 * Reads a field: $0, a field, or the empty string past NF.
 */
void
field_read(
	size_t index,
	struct value *value)
{
	/* The record. */
	if (index == 0) {
		value_copy(value, &awk.record);
		return;
	}

	/* A field past NF is an empty string. */
	if (index > awk.field_count) {
		value_set_text(value, "", 0);
		return;
	}

	/* Succeeded: the field. */
	value_copy(value, &awk.fields[index]);
}

/*
 * Writes a field: $0 splits again, and any other field joins $0 again,
 * adding empty fields up to it.
 */
void
field_write(
	size_t index,
	const struct value *value)
{
	size_t added;

	/* The record. */
	if (index == 0) {
		record_set_value(value);
		return;
	}

	/* Empty fields up to it. */
	if (index > awk.field_count) {
		ensure_fields(index);
		for (added = awk.field_count + 1U; added < index; added++)
			value_set_text(&awk.fields[added], "", 0);
		awk.field_count = index;
		set_field_count_variable();
	}

	/* Succeeded: the field, and the record joined again. */
	value_copy(&awk.fields[index], value);
	rebuild_record();
}

/*
 * Reads a record from a stream, separated by RS: its first character, or
 * blank lines when RS is empty.  Returns 0 at the end of the stream.
 */
int
record_read(
	FILE *stream,
	struct buffer *buffer)
{
	const char *separator;
	size_t separator_length;
	int character;
	int following;
	int read_any;

	/* An empty record to fill. */
	buffer->length = 0;
	buffer_append(buffer, "", 0);
	separator = special_text(SPECIAL_RS, &separator_length);

	/* One character separates records. */
	if (separator_length > 0) {
		read_any = 0;
		for (;;) {
			character = getc(stream);
			if (character == EOF)
				break;
			read_any = 1;
			if (character == (unsigned char)separator[0])
				return 1;
			buffer_append_byte(buffer, (char)character);
		}

		/* Nothing before the end is no record. */
		if (!read_any)
			return 0;
		return 1;
	}

	/* Paragraph mode: newlines before the first record do not count. */
	character = getc(stream);
	while (character == '\n')
		character = getc(stream);
	if (character == EOF)
		return 0;

	/* The lines up to a blank line, which ends the record with the rest. */
	for (;;) {
		if (character == EOF)
			return 1;
		if (character == '\n') {
			following = getc(stream);
			if (following == EOF)
				return 1;
			if (following == '\n') {
				do {
					following = getc(stream);
				} while (following == '\n');
				if (following != EOF)
					ungetc(following, stream);
				return 1;
			}

			/* One newline stays in the record. */
			buffer_append_byte(buffer, '\n');
			character = following;
			continue;
		}

		/* Any other character. */
		buffer_append_byte(buffer, (char)character);
		character = getc(stream);
	}
}

/*
 * Reads the next record of the main input: the files named in ARGV in
 * turn, with the assignments among them made when they are reached, or
 * standard input when ARGV names none.  NR and FNR count the record.
 * Returns 0 when the input has ended.
 */
int
input_next(
	struct buffer *buffer)
{
	int opened;
	int read;

	/* A record from the file being read, or from the next one. */
	for (;;) {
		if (awk.input == NULL) {
			if (awk.input_ended)
				return 0;
			opened = open_next_input();
			if (!opened) {
				awk.input_ended = 1;
				return 0;
			}
		}

		/* The next record of it; at its end, the next file. */
		read = record_read(awk.input, buffer);
		if (read)
			break;
		input_skip_file();
	}

	/* Succeeded: one more record. */
	count_record();
	return 1;
}

/*
 * Stops reading the current input file (at its end, and for nextfile).
 */
void
input_skip_file(
	void)
{
	/* The file, unless it is standard input, which stays open. */
	if (awk.input == NULL)
		return;
	if (!awk.input_is_stdin)
		fclose(awk.input);
	awk.input = NULL;
	awk.input_is_stdin = 0;
}

/*
 * Splits a string into the elements of an array, 1 on, as split() does:
 * with a regex when one is given, and otherwise with a separator string
 * that has the meaning FS gives it.  Returns the number of elements.
 */
size_t
split_text(
	const char *text,
	size_t length,
	const char *separator,
	size_t separator_length,
	regex_t *regex,
	struct array *array)
{
	struct element *element;
	char key[32];
	size_t index;
	int key_length;

	/* The pieces. */
	split_string(text, length, separator, separator_length, regex, 0);

	/* An element for each, as input. */
	array_clear(array);
	for (index = 0; index < split_pieces.count; index++) {
		key_length = snprintf(key, sizeof(key), "%lu", (unsigned long)(index + 1U));
		element = array_find(array, key, (size_t)key_length, 1);
		value_set_input(&element->value,
				text + split_pieces.items[index].start,
				split_pieces.items[index].length);
	}

	/* Succeeded: how many. */
	return split_pieces.count;
}

/*
 * Returns the text of a special variable that holds a string (FS, OFS,
 * ORS, RS, SUBSEP, CONVFMT, OFMT, FILENAME).
 */
const char *
special_text(
	int special,
	size_t *length)
{
	struct variable *variable;

	/* The string the variable holds. */
	variable = awk.specials[special];
	if (variable->cell.kind == CELL_SCALAR && variable->cell.value.text != NULL) {
		*length = variable->cell.value.length;
		return variable->cell.value.text;
	}

	/* The formats fall back on the default. */
	if (special == SPECIAL_CONVFMT || special == SPECIAL_OFMT) {
		*length = 4;
		return "%.6g";
	}

	/* Anything else unset is empty. */
	*length = 0;
	return "";
}

/*
 * Acts on the assignment of a variable that awk gives a meaning to: NF
 * changes the record, and a string variable given a number keeps its
 * string form.
 */
void
special_assigned(
	struct variable *variable)
{
	struct value *value;
	double number;

	/* What was assigned. */
	value = &variable->cell.value;

	/* The variable it was assigned to. */
	switch (variable->special) {
	case SPECIAL_NF:
		number = value_number(value);
		if (number < 0)
			awk_fatal("NF set to a negative value");
		record_set_field_count((size_t)number);
		break;
	case SPECIAL_FS:
	case SPECIAL_OFS:
	case SPECIAL_ORS:
	case SPECIAL_RS:
	case SPECIAL_SUBSEP:
	case SPECIAL_CONVFMT:
	case SPECIAL_OFMT:
	case SPECIAL_FILENAME:
		if (value->type == VALUE_NUMBER || value->type == VALUE_UNSET)
			value_string(value, 0, value);
		break;
	default:
		break;
	}
}

/*
 * Makes an assignment written as name=value (on the command line, or
 * with -v), with the escapes of a string in the value.  Returns 0 when the
 * operand is not an assignment.
 */
int
command_assignment(
	const char *operand)
{
	struct variable *variable;
	struct value value;
	struct buffer buffer;
	const char *equals;
	const char *cursor;
	char *name;

	/* A name, then =. */
	if (!((operand[0] >= 'a' && operand[0] <= 'z') ||
	      (operand[0] >= 'A' && operand[0] <= 'Z') ||
	      operand[0] == '_'))
		return 0;
	for (cursor = operand; *cursor != '\0' && *cursor != '='; cursor++) {
		if (*cursor >= 'a' && *cursor <= 'z')
			continue;
		if (*cursor >= 'A' && *cursor <= 'Z')
			continue;
		if (*cursor >= '0' && *cursor <= '9')
			continue;
		if (*cursor == '_')
			continue;
		return 0;
	}

	/* The name must end at =. */
	if (*cursor != '=')
		return 0;
	equals = cursor;

	/* The value, with its escapes, as input. */
	memset(&buffer, 0, sizeof(buffer));
	unescape(equals + 1, strlen(equals + 1), &buffer);
	memset(&value, 0, sizeof(value));
	value_set_input(&value, buffer.data, buffer.length);
	free(buffer.data);

	/* Succeeded: the variable takes it. */
	name = awk_copy(operand, (size_t)(equals - operand));
	variable = variable_find(name, 1);
	free(name);
	assign_variable(variable, &value);
	value_free(&value);
	return 1;
}

/* Splits $0 into the fields, with FS. */
static void
split_record(
	void)
{
	const char *separator;
	size_t separator_length;
	size_t record_separator_length;
	size_t index;
	int paragraph;

	/* The pieces, with newline a separator too in paragraph mode. */
	separator = special_text(SPECIAL_FS, &separator_length);
	special_text(SPECIAL_RS, &record_separator_length);
	paragraph = 0;
	if (record_separator_length == 0)
		paragraph = 1;
	split_string(awk.record.text,
		     awk.record.length,
		     separator,
		     separator_length,
		     NULL,
		     paragraph);

	/* A field for each piece, as input. */
	for (index = 1; index <= awk.field_count; index++)
		value_free(&awk.fields[index]);
	ensure_fields(split_pieces.count);
	for (index = 0; index < split_pieces.count; index++) {
		value_set_input(&awk.fields[index + 1U],
				awk.record.text + split_pieces.items[index].start,
				split_pieces.items[index].length);
	}

	/* Succeeded: NF counts them. */
	awk.field_count = split_pieces.count;
	set_field_count_variable();
}

/*
 * Splits a string into pieces: with a regex when one is given; otherwise a
 * single space is runs of blanks, another single character is itself, an
 * empty separator makes each character a piece, and anything longer is an
 * ERE.  In paragraph mode a newline separates too.
 */
static void
split_string(
	const char *text,
	size_t length,
	const char *separator,
	size_t separator_length,
	regex_t *regex,
	int paragraph)
{
	struct buffer combined;
	size_t index;

	/* No pieces in an empty string. */
	split_pieces.count = 0;
	if (length == 0)
		return;

	/* A regex given. */
	if (regex != NULL) {
		split_regex(text, length, regex);
		return;
	}

	/* Runs of blanks, which include newlines. */
	if (separator_length == 1 && separator[0] == ' ') {
		split_blanks(text, length);
		return;
	}

	/* One character. */
	if (separator_length == 1) {
		split_character(text, length, separator[0], paragraph);
		return;
	}

	/* Each character. */
	if (separator_length == 0) {
		for (index = 0; index < length; index++)
			add_piece(index, 1);
		return;
	}

	/* An ERE, or it or a newline in paragraph mode. */
	if (!paragraph) {
		regex = regex_compile(separator, separator_length, 1);
		split_regex(text, length, regex);
		return;
	}

	/* In paragraph mode, the ERE or a newline. */
	memset(&combined, 0, sizeof(combined));
	buffer_append(&combined, "(", 1);
	buffer_append(&combined, separator, separator_length);
	buffer_append(&combined, ")|\n", 3);
	regex = regex_compile(combined.data, combined.length, 1);
	free(combined.data);
	split_regex(text, length, regex);
}

/* Splits a string at runs of blanks, ignoring those at the ends. */
static void
split_blanks(
	const char *text,
	size_t length)
{
	size_t position;
	size_t start;
	int blank;

	/* Each run of non-blanks. */
	position = 0;
	for (;;) {
		/* The blanks before it. */
		while (position < length) {
			blank = is_blank(text[position]);
			if (!blank)
				break;
			position++;
		}

		/* No more fields. */
		if (position >= length)
			break;

		/* The run itself. */
		start = position;
		while (position < length) {
			blank = is_blank(text[position]);
			if (blank)
				break;
			position++;
		}

		/* The field. */
		add_piece(start, position - start);
	}
}

/* Splits a string at each occurrence of a character (or a newline too). */
static void
split_character(
	const char *text,
	size_t length,
	char separator,
	int paragraph)
{
	size_t position;
	size_t start;

	/* The pieces between the separators, and after the last one. */
	start = 0;
	for (position = 0; position < length; position++) {
		if (text[position] != separator &&
		    !(paragraph && text[position] == '\n'))
			continue;
		add_piece(start, position - start);
		start = position + 1U;
	}

	/* The piece after the last separator. */
	add_piece(start, length - start);
}

/* Splits a string at the matches of a regex; empty matches do not split. */
static void
split_regex(
	const char *text,
	size_t length,
	regex_t *regex)
{
	size_t position;
	size_t start;
	size_t match_start;
	size_t match_end;
	int found;

	/* The pieces between the matches. */
	start = 0;
	position = 0;
	while (position < length) {
		found = regex_search(regex, text, length, position, &match_start, &match_end);
		if (!found)
			break;
		if (match_end == match_start) {
			position = match_start + 1U;
			continue;
		}

		/* The piece before the match. */
		add_piece(start, match_start - start);
		start = match_end;
		position = match_end;
	}

	/* Succeeded: the piece after the last match. */
	add_piece(start, length - start);
}

/* Adds a piece to the pieces of the split. */
static void
add_piece(
	size_t start,
	size_t length)
{
	/* Room for it. */
	if (split_pieces.count >= split_pieces.capacity) {
		split_pieces.capacity = split_pieces.capacity * 2U + 16U;
		split_pieces.items = awk_reallocate(
			split_pieces.items,
			sizeof(*split_pieces.items) * split_pieces.capacity);
	}

	/* The piece. */
	split_pieces.items[split_pieces.count].start = start;
	split_pieces.items[split_pieces.count].length = length;
	split_pieces.count++;
}

/* Joins the fields into $0 with OFS. */
static void
rebuild_record(
	void)
{
	struct buffer buffer;
	struct value string;
	const char *separator;
	size_t separator_length;
	size_t index;

	/* Each field in its string form, with OFS between them. */
	separator = special_text(SPECIAL_OFS, &separator_length);
	memset(&buffer, 0, sizeof(buffer));
	memset(&string, 0, sizeof(string));
	buffer_append(&buffer, "", 0);
	for (index = 1; index <= awk.field_count; index++) {
		if (index > 1)
			buffer_append(&buffer, separator, separator_length);
		value_string(&awk.fields[index], 0, &string);
		buffer_append(&buffer, string.text, string.length);
	}

	/* The string form goes. */
	value_free(&string);

	/* Succeeded: the record, which does not split again. */
	value_set_input(&awk.record, buffer.data, buffer.length);
	free(buffer.data);
}

/* Sets the NF variable to the number of fields, without splitting again. */
static void
set_field_count_variable(
	void)
{
	struct cell *cell;

	/* The cell of NF holds the count as a number. */
	cell = &awk.specials[SPECIAL_NF]->cell;
	cell->kind = CELL_SCALAR;
	value_set_number(&cell->value, (double)awk.field_count);
}

/* Makes room for fields up to an index. */
static void
ensure_fields(
	size_t count)
{
	size_t old_capacity;

	/* Enough already. */
	if (count + 1U <= awk.field_capacity)
		return;

	/* Succeeded: more, the new ones unset. */
	old_capacity = awk.field_capacity;
	awk.field_capacity = (count + 1U) * 2U;
	awk.fields = awk_reallocate(awk.fields, sizeof(*awk.fields) * awk.field_capacity);
	memset(awk.fields + old_capacity,
	       0,
	       sizeof(*awk.fields) * (awk.field_capacity - old_capacity));
}

/*
 * Opens the next file of the main input, making the assignments before
 * it.  Returns 0 when there is none left.
 */
static int
open_next_input(
	void)
{
	struct array *arguments;
	struct element *element;
	struct value operand;
	struct value filename;
	char key[64];
	double count;
	int key_length;
	int assignment;
	int compare;

	/* The operands in ARGV, up to ARGC as it is now. */
	memset(&operand, 0, sizeof(operand));
	arguments = cell_array(&awk.specials[SPECIAL_ARGV]->cell);
	for (;;) {
		count = value_number(&awk.specials[SPECIAL_ARGC]->cell.value);
		if (awk.argument_index >= count)
			break;
		key_length = snprintf(key, sizeof(key), "%.0f", awk.argument_index);
		awk.argument_index += 1;

		/* A deleted or empty operand is skipped. */
		element = array_find(arguments, key, (size_t)key_length, 0);
		if (element == NULL)
			continue;
		value_string(&element->value, 0, &operand);
		if (operand.length == 0)
			continue;

		/* An assignment is made now. */
		assignment = command_assignment(operand.text);
		if (assignment)
			continue;

		/* A file; - is standard input. */
		awk.input_used_file = 1;
		compare = strcmp(operand.text, "-");
		if (compare == 0) {
			awk.input = stdin;
			awk.input_is_stdin = 1;
		} else {
			awk.input = fopen(operand.text, "r");
			awk.input_is_stdin = 0;
		}

		/* A file that cannot be opened is fatal. */
		if (awk.input == NULL)
			awk_fatal("cannot open file %s for reading (%s)", operand.text, strerror(errno));

		/* Succeeded: FILENAME names it, and FNR counts from it. */
		memset(&filename, 0, sizeof(filename));
		value_set_text(&filename, operand.text, operand.length);
		assign_variable(awk.specials[SPECIAL_FILENAME], &filename);
		value_free(&filename);
		value_set_number(&awk.specials[SPECIAL_FNR]->cell.value, 0);
		value_free(&operand);
		return 1;
	}

	/* No operand is left. */
	value_free(&operand);

	/* No file named: standard input, once. */
	if (awk.input_used_file)
		return 0;
	awk.input_used_file = 1;
	awk.input = stdin;
	awk.input_is_stdin = 1;
	value_set_number(&awk.specials[SPECIAL_FNR]->cell.value, 0);

	/* Succeeded. */
	return 1;
}

/* Counts a record of the main input in NR and FNR. */
static void
count_record(
	void)
{
	struct cell *cell;
	double number;

	/* NR. */
	cell = &awk.specials[SPECIAL_NR]->cell;
	number = value_number(&cell->value);
	cell->kind = CELL_SCALAR;
	value_set_number(&cell->value, number + 1);

	/* FNR. */
	cell = &awk.specials[SPECIAL_FNR]->cell;
	number = value_number(&cell->value);
	cell->kind = CELL_SCALAR;
	value_set_number(&cell->value, number + 1);
}

/* Appends a string with the escapes of an awk string made characters. */
static void
unescape(
	const char *text,
	size_t length,
	struct buffer *buffer)
{
	size_t position;
	size_t count;
	char character;
	int value;

	/* Each character; a backslash starts an escape. */
	buffer_append(buffer, "", 0);
	for (position = 0; position < length; position++) {
		character = text[position];
		if (character != '\\' || position + 1U >= length) {
			buffer_append_byte(buffer, character);
			continue;
		}

		/* The character after the backslash. */
		position++;
		character = text[position];

		/* An octal escape of up to three digits. */
		if (character >= '0' && character <= '7') {
			value = 0;
			for (count = 0; count < 3 && position < length; count++) {
				character = text[position];
				if (character < '0' || character > '7')
					break;
				value = value * 8 + (character - '0');
				position++;
			}

			/* Back to the last digit, which the loop moves past. */
			position--;
			buffer_append_byte(buffer, (char)value);
			continue;
		}

		/* The escapes of one letter; any other is the character. */
		switch (character) {
		case 'a':
			character = '\a';
			break;
		case 'b':
			character = '\b';
			break;
		case 'f':
			character = '\f';
			break;
		case 'n':
			character = '\n';
			break;
		case 'r':
			character = '\r';
			break;
		case 't':
			character = '\t';
			break;
		case 'v':
			character = '\v';
			break;
		default:
			break;
		}

		/* The character of the escape. */
		buffer_append_byte(buffer, character);
	}
}

/* Returns whether a character is a blank that separates default fields. */
static int
is_blank(
	char character)
{
	/* Space, tab and newline. */
	if (character == ' ' || character == '\t' || character == '\n')
		return 1;
	return 0;
}
