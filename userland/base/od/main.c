/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Dumps files in octal and other formats (POSIX XCU od).
 *
 *	od [-v] [-A base] [-j skip] [-N count] [-t type]... [file...]
 *	od [-bcdosx] [file...]		(the older forms of -t)
 *
 * The input (the files one after another) is written 16 bytes to a line,
 * once for each type, each line after the first indented by the width of
 * the address.  The types are a (named characters), c (characters and
 * escapes), d, o, u, x (signed, octal, unsigned, hexadecimal integers of 1,
 * 2, 4 or 8 bytes, or C S I L) and f (floating point of 4 or 8 bytes, or F
 * D).  -b, -c, -d, -o, -s and -x are o1, c, u2, o2, d2 and x2; the default
 * is o2.  A line equal to the one before is written as a single * until one
 * differs, unless -v.  The address is octal (-A o), decimal (d),
 * hexadecimal (x) or absent (n), and the last line is the address of the
 * end.
 *
 * The fields are laid out as GNU od does: every type's fields in a line
 * take the same total width, the widest type's, with the spare width shared
 * out between the fields, so that the lines of the types line up.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How many bytes a line shows. */
#define LINE_BYTES 16U

/* The most types given. */
#define TYPE_MAX 32

/* A type: its letter, the bytes of an element and the width of a field. */
struct type {
	char letter;
	unsigned size;
	int width;
	int pad;
};

/* The options and the state of the dump. */
struct dump {
	struct type types[TYPE_MAX];
	size_t type_count;
	char address_base;
	unsigned long long skip;
	unsigned long long limit;
	int have_limit;
	int verbose;

	/* The inputs, one after another. */
	char **files;
	int file_count;
	int next_file;
	FILE *stream;
	int status;
};

static int read_options(int argc, char **argv, struct dump *dump);
static const char *option_argument(int argc, char **argv, int *index, const char *rest);
static void add_types(struct dump *dump, const char *text);
static void add_type(struct dump *dump, char letter, unsigned size);
static int integer_width(char letter, unsigned size);
static unsigned long long parse_size(const char *text);
static void layout(struct dump *dump);
static size_t read_bytes(struct dump *dump, unsigned char *buffer, size_t wanted);
static int next_input(struct dump *dump);
static void skip_input(struct dump *dump);
static void print_address(const struct dump *dump, unsigned long long address);
static void print_line(const struct dump *dump, const unsigned char *line, size_t length);
static void print_fields(const struct type *type, const unsigned char *line, size_t length);
static void format_field(const struct type *type, const unsigned char *bytes, char *text, size_t size);
static void format_integer(const struct type *type, const unsigned char *bytes, char *text, size_t size);
static void format_float(const struct type *type, const unsigned char *bytes, char *text, size_t size);
static void format_character(unsigned char value, char *text, size_t size);
static void format_named(unsigned char value, char *text, size_t size);
static void usage(void);

/*
 * Runs od.
 */
int
main(
	int argc,
	char **argv)
{
	static struct dump dump;
	unsigned char previous[LINE_BYTES];
	unsigned char line[LINE_BYTES];
	unsigned long long address;
	size_t length;
	size_t wanted;
	int same;
	int starred;
	int have_previous;

	/* The options, the types (o2 by default) and their layout. */
	dump.address_base = 'o';
	dump.status = 0;
	dump.file_count = argc - read_options(argc, argv, &dump);
	dump.files = argv + (argc - dump.file_count);
	if (dump.type_count == 0)
		add_type(&dump, 'o', 2);
	layout(&dump);

	/* The skipped bytes. */
	skip_input(&dump);
	address = dump.skip;

	/* Each line of output, with no line before it to repeat. */
	have_previous = 0;
	starred = 0;
	for (;;) {
		/* The next line of bytes, within -N. */
		wanted = LINE_BYTES;
		if (dump.have_limit &&
		    dump.limit - (address - dump.skip) < (unsigned long long)wanted)
			wanted = (size_t)(dump.limit - (address - dump.skip));
		length = 0;
		if (wanted > 0)
			length = read_bytes(&dump, line, wanted);
		if (length == 0)
			break;

		/* A full line equal to the one before is a *. */
		same = 0;
		if (!dump.verbose && have_previous && length == LINE_BYTES)
			same = memcmp(previous, line, LINE_BYTES) == 0;
		if (same) {
			if (!starred)
				printf("*\n");
			starred = 1;
			address += length;
			continue;
		}

		/* A line that differs ends a run of repeats. */
		starred = 0;

		/* The line. */
		print_address(&dump, address);
		print_line(&dump, line, length);
		memcpy(previous, line, LINE_BYTES);
		have_previous = (length == LINE_BYTES);
		address += length;
	}

	/* Succeeded: the address of the end. */
	if (dump.address_base != 'n') {
		print_address(&dump, address);
		putchar('\n');
	}

	/* Succeeded: whether every file could be read. */
	return dump.status;
}

/* Reads the options; returns the index of the first file. */
static int
read_options(
	int argc,
	char **argv,
	struct dump *dump)
{
	const char *word;
	const char *letter;
	const char *argument;
	const char *base;
	int index;

	/* Each option word. */
	for (index = 1; index < argc; index++) {
		/* A file, or - alone, ends the options; so does --. */
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;
		if (word[1] == '-' && word[2] == '\0')
			return index + 1;

		/* Each letter of the word. */
		for (letter = word + 1; *letter != '\0'; letter++) {
			/* The older forms, and -v. */
			switch (*letter) {
			case 'b':
				add_type(dump, 'o', 1);
				continue;
			case 'c':
				add_type(dump, 'c', 1);
				continue;
			case 'd':
				add_type(dump, 'u', 2);
				continue;
			case 'o':
				add_type(dump, 'o', 2);
				continue;
			case 's':
				add_type(dump, 'd', 2);
				continue;
			case 'x':
				add_type(dump, 'x', 2);
				continue;
			case 'v':
				dump->verbose = 1;
				continue;
			default:
				break;
			}

			/* -A, -j, -N and -t take the rest of the word or the next. */
			argument = option_argument(argc, argv, &index, letter + 1);
			switch (*letter) {
			case 'A':
				if (argument[0] == '\0' || argument[1] != '\0')
					usage();
				base = strchr("dnox", argument[0]);
				if (base == NULL)
					usage();
				dump->address_base = argument[0];
				break;
			case 'j':
				dump->skip = parse_size(argument);
				break;
			case 'N':
				dump->limit = parse_size(argument);
				dump->have_limit = 1;
				break;
			case 't':
				add_types(dump, argument);
				break;
			default:
				usage();
			}

			break;
		}
	}

	/* Succeeded: the first file. */
	return index;
}

/* Returns an option's argument: the rest of its word, or the next word. */
static const char *
option_argument(
	int argc,
	char **argv,
	int *index,
	const char *rest)
{
	/* The rest of the word. */
	if (*rest != '\0')
		return rest;

	/* The next word. */
	if (*index + 1 >= argc)
		usage();
	(*index)++;

	/* Succeeded. */
	return argv[*index];
}

/* Parses a type string: letters, each with an optional size. */
static void
add_types(
	struct dump *dump,
	const char *text)
{
	const char *known;
	unsigned size;
	char letter;

	/* Each type of the string. */
	while (*text != '\0') {
		/* The letter. */
		letter = *text;
		text++;
		if (letter == 'a' || letter == 'c') {
			add_type(dump, letter, 1);
			continue;
		}

		/* The numeric types. */
		known = strchr("doufx", letter);
		if (known == NULL) {
			fprintf(stderr, "od: invalid type string '%c'\n", letter);
			exit(1);
		}

		/* The size: digits, or C S I L (F D L for f). */
		size = 4;
		if (letter == 'f')
			size = 8;
		if (*text >= '0' && *text <= '9') {
			size = 0;
			while (*text >= '0' && *text <= '9') {
				size = size * 10U + (unsigned)(*text - '0');
				text++;
			}
		} else if (*text == 'C') {
			size = 1;
			text++;
		} else if (*text == 'S') {
			size = 2;
			text++;
		} else if (*text == 'I' || *text == 'F') {
			size = 4;
			text++;
		} else if (*text == 'L' || *text == 'D') {
			size = 8;
			text++;
		}

		/* The type, added. */
		add_type(dump, letter, size);
	}
}

/* Adds one type, with the width of its fields. */
static void
add_type(
	struct dump *dump,
	char letter,
	unsigned size)
{
	struct type *type;

	/* The sizes there are. */
	if (letter == 'f' && size != 4 && size != 8) {
		fprintf(stderr, "od: invalid type size %u for f\n", size);
		exit(1);
	}

	/* Sizes of 1, 2, 4 and 8 bytes. */
	if (size != 1 && size != 2 && size != 4 && size != 8) {
		fprintf(stderr, "od: invalid type size %u\n", size);
		exit(1);
	}

	/* No more types than there is room for. */
	if (dump->type_count >= TYPE_MAX)
		usage();

	/* The type and the width of its field. */
	type = &dump->types[dump->type_count];
	type->letter = letter;
	type->size = size;
	type->width = 3;
	if (letter == 'f')
		type->width = 15;
	if (letter == 'f' && size == 8)
		type->width = 24;
	if (letter == 'd' || letter == 'o' || letter == 'u' || letter == 'x')
		type->width = integer_width(letter, size);
	dump->type_count++;
}

/* Returns the widest a field of an integer type is. */
static int
integer_width(
	char letter,
	unsigned size)
{
	/* The digits of the largest value, and a sign for d. */
	switch (letter) {
	case 'o':
		return (int)((size * 8U + 2U) / 3U);
	case 'x':
		return (int)(size * 2U);
	case 'u':
		if (size == 1)
			return 3;
		if (size == 2)
			return 5;
		if (size == 4)
			return 10;
		return 20;
	default:
		break;
	}

	/* d. */
	if (size == 1)
		return 4;
	if (size == 2)
		return 6;
	if (size == 4)
		return 11;
	return 20;
}

/*
 * Reads a byte count: decimal, octal after 0, hexadecimal after 0x, with an
 * optional multiplier b (512), k (1024) or m (1048576).
 */
static unsigned long long
parse_size(
	const char *text)
{
	unsigned long long value;
	char *end;

	/* The number. */
	errno = 0;
	value = strtoull(text, &end, 0);
	if (end == text || errno != 0)
		usage();

	/* The multiplier. */
	if (*end == 'b') {
		value *= 512U;
		end++;
	} else if (*end == 'k') {
		value *= 1024U;
		end++;
	} else if (*end == 'm') {
		value *= 1048576U;
		end++;
	}

	/* Only digits and a suffix. */
	if (*end != '\0')
		usage();

	/* Succeeded. */
	return value;
}

/* Computes the padding that lines up the types (as GNU od does). */
static void
layout(
	struct dump *dump)
{
	struct type *type;
	size_t index;
	int fields;
	int block;
	int widest;

	/* The widest line of fields, a space before each. */
	widest = 0;
	for (index = 0; index < dump->type_count; index++) {
		type = &dump->types[index];
		fields = (int)(LINE_BYTES / type->size);
		block = (type->width + 1) * fields;
		if (block > widest)
			widest = block;
	}

	/* Each type's spare width in a line. */
	for (index = 0; index < dump->type_count; index++) {
		type = &dump->types[index];
		fields = (int)(LINE_BYTES / type->size);
		type->pad = widest - type->width * fields;
	}
}

/*
 * Reads up to wanted bytes from the inputs, going on to the next input at
 * the end of one.  Returns how many were read.
 */
static size_t
read_bytes(
	struct dump *dump,
	unsigned char *buffer,
	size_t wanted)
{
	size_t length;
	int value;
	int more;

	/* No bytes yet. */
	length = 0;
	while (length < wanted) {
		/* The open input, or the next one. */
		if (dump->stream == NULL) {
			more = next_input(dump);
			if (!more)
				break;
		}

		/* A byte, or the end of this input. */
		value = getc(dump->stream);
		if (value == EOF) {
			if (dump->stream != stdin)
				fclose(dump->stream);
			dump->stream = NULL;
			continue;
		}

		/* The byte. */
		buffer[length] = (unsigned char)value;
		length++;
	}

	/* Succeeded. */
	return length;
}

/* Opens the next input; standard input when there are no files. */
static int
next_input(
	struct dump *dump)
{
	const char *name;
	int compare;

	/* No files: standard input, once. */
	if (dump->file_count == 0) {
		if (dump->next_file > 0)
			return 0;
		dump->next_file = 1;
		dump->stream = stdin;
		return 1;
	}

	/* The named files in turn. */
	while (dump->next_file < dump->file_count) {
		/* The next file; - is standard input. */
		name = dump->files[dump->next_file];
		dump->next_file++;
		compare = strcmp(name, "-");
		if (compare == 0) {
			dump->stream = stdin;
			return 1;
		}

		/* A file, or the next when it cannot be opened. */
		dump->stream = fopen(name, "r");
		if (dump->stream != NULL)
			return 1;
		fprintf(stderr, "od: %s: %s\n", name, strerror(errno));
		dump->status = 1;
	}

	/* No more. */
	return 0;
}

/* Skips the -j bytes of the inputs. */
static void
skip_input(
	struct dump *dump)
{
	unsigned char buffer[4096];
	unsigned long long remaining;
	size_t chunk;
	size_t read;

	/* Read and dropped, since an input may be a pipe. */
	remaining = dump->skip;
	while (remaining > 0) {
		chunk = sizeof(buffer);
		if (remaining < (unsigned long long)chunk)
			chunk = (size_t)remaining;
		read = read_bytes(dump, buffer, chunk);
		if (read == 0)
			break;
		remaining -= read;
	}
}

/* Writes an address in the -A base. */
static void
print_address(
	const struct dump *dump,
	unsigned long long address)
{
	/* The base. */
	switch (dump->address_base) {
	case 'd':
		printf("%07llu", address);
		break;
	case 'x':
		printf("%06llx", address);
		break;
	case 'o':
		printf("%07llo", address);
		break;
	default:
		break;
	}
}

/* Writes a line once for each type, the later ones indented. */
static void
print_line(
	const struct dump *dump,
	const unsigned char *line,
	size_t length)
{
	size_t index;
	int indent;

	/* The width of the address, for the lines after the first. */
	indent = 7;
	if (dump->address_base == 'x')
		indent = 6;
	if (dump->address_base == 'n')
		indent = 0;

	/* Each type on a line of its own, under the first. */
	for (index = 0; index < dump->type_count; index++) {
		if (index > 0)
			printf("%*s", indent, "");
		print_fields(&dump->types[index], line, length);
		putchar('\n');
	}
}

/*
 * Writes the fields of one type for a line; a partial last element is
 * filled out with zero bytes.  The spare width is shared out as GNU od
 * shares it.
 */
static void
print_fields(
	const struct type *type,
	const unsigned char *line,
	size_t length)
{
	unsigned char bytes[LINE_BYTES];
	char text[64];
	int fields;
	int shown;
	int field;
	int remaining;
	int next;

	/* The bytes, zero past the end. */
	memset(bytes, 0, sizeof(bytes));
	memcpy(bytes, line, length);
	fields = (int)(LINE_BYTES / type->size);
	shown = (int)((length + type->size - 1U) / type->size);

	/* The fields, with the spare width shared among them. */
	remaining = type->pad;
	for (field = 0; field < shown; field++) {
		/* This field's share of the spare width. */
		next = type->pad * (fields - field - 1) / fields;
		format_field(type, bytes + (size_t)field * type->size, text,
			     sizeof(text));
		printf("%*s", remaining - next + type->width, text);
		remaining = next;
	}
}

/* Formats one element of a type. */
static void
format_field(
	const struct type *type,
	const unsigned char *bytes,
	char *text,
	size_t size)
{
	/* Dispatches on the letter. */
	switch (type->letter) {
	case 'a':
		format_named(bytes[0], text, size);
		break;
	case 'c':
		format_character(bytes[0], text, size);
		break;
	case 'f':
		format_float(type, bytes, text, size);
		break;
	default:
		format_integer(type, bytes, text, size);
		break;
	}
}

/* Formats an integer element (in the machine's byte order). */
static void
format_integer(
	const struct type *type,
	const unsigned char *bytes,
	char *text,
	size_t size)
{
	unsigned long long value;
	unsigned long long sign;
	long long signed_value;
	unsigned index;

	/* The value, little-endian as the machine is. */
	value = 0;
	for (index = type->size; index > 0; index--)
		value = (value << 8) | bytes[index - 1U];

	/* Dispatches on the letter. */
	switch (type->letter) {
	case 'o':
		(void)snprintf(text, size, "%0*llo", type->width, value);
		return;
	case 'x':
		(void)snprintf(text, size, "%0*llx", type->width, value);
		return;
	case 'u':
		(void)snprintf(text, size, "%llu", value);
		return;
	default:
		break;
	}

	/* d: the value extended from its sign bit. */
	sign = 1ULL << (type->size * 8U - 1U);
	if (type->size < 8U && (value & sign) != 0)
		value |= ~((sign << 1) - 1U);
	signed_value = (long long)value;
	(void)snprintf(text, size, "%lld", signed_value);
}

/*
 * Formats a floating-point element: the shortest form that reads back as
 * the same value.
 */
static void
format_float(
	const struct type *type,
	const unsigned char *bytes,
	char *text,
	size_t size)
{
	double value;
	double back;
	float single;
	float back_single;
	int precision;
	int limit;

	/* The value. */
	single = 0.0f;
	if (type->size == 4) {
		memcpy(&single, bytes, sizeof(single));
		value = single;
		limit = 9;
	} else {
		memcpy(&value, bytes, sizeof(value));
		limit = 17;
	}

	/* The fewest digits that give the value back. */
	for (precision = 1; precision < limit; precision++) {
		(void)snprintf(text, size, "%.*g", precision, value);
		if (type->size == 4) {
			back_single = strtof(text, NULL);
			if (back_single == single)
				return;
		} else {
			back = strtod(text, NULL);
			if (back == value)
				return;
		}
	}

	/* Succeeded: all the digits. */
	(void)snprintf(text, size, "%.*g", limit, value);
}

/* Formats a byte for -c: itself, an escape, or three octal digits. */
static void
format_character(
	unsigned char value,
	char *text,
	size_t size)
{
	/* The C escapes. */
	switch (value) {
	case '\0':
		(void)snprintf(text, size, "\\0");
		return;
	case '\a':
		(void)snprintf(text, size, "\\a");
		return;
	case '\b':
		(void)snprintf(text, size, "\\b");
		return;
	case '\f':
		(void)snprintf(text, size, "\\f");
		return;
	case '\n':
		(void)snprintf(text, size, "\\n");
		return;
	case '\r':
		(void)snprintf(text, size, "\\r");
		return;
	case '\t':
		(void)snprintf(text, size, "\\t");
		return;
	case '\v':
		(void)snprintf(text, size, "\\v");
		return;
	default:
		break;
	}

	/* A printable byte, or octal. */
	if (value >= 0x20U && value < 0x7fU)
		(void)snprintf(text, size, "%c", value);
	else
		(void)snprintf(text, size, "%03o", value);
}

/* Formats a byte for -t a: its name, the high bit dropped. */
static void
format_named(
	unsigned char value,
	char *text,
	size_t size)
{
	static const char *const names[] = {
		"nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel",
		"bs", "ht", "nl", "vt", "ff", "cr", "so", "si",
		"dle", "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb",
		"can", "em", "sub", "esc", "fs", "gs", "rs", "us",
		"sp"
	};

	/* Seven bits. */
	value &= 0x7fU;

	/* A control character or space has a name; so has del. */
	if (value <= 0x20U) {
		(void)snprintf(text, size, "%s", names[value]);
		return;
	}

	/* DEL. */
	if (value == 0x7fU) {
		(void)snprintf(text, size, "del");
		return;
	}

	/* Succeeded: the character. */
	(void)snprintf(text, size, "%c", value);
}

/* Reports the usage and ends od. */
static void
usage(
	void)
{
	/* The forms. */
	fprintf(stderr, "usage: od [-v] [-A base] [-j skip] [-N count] "
		"[-t type]... [file...]\n"
		"       od [-bcdosx] [file...]\n");
	exit(1);
}
