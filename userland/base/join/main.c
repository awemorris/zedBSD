/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Joins the lines of two files on a common field (POSIX XCU join).
 *
 *	join [-a file_number|-v file_number] [-e string] [-o list] [-t char]
 *	     [-1 field] [-2 field] file1 file2
 *
 * Both files are sorted on their join field (field 1 by default).  Each
 * pair of lines with equal join fields gives one output line: the join
 * field, the other fields of the line of file1, then those of file2.  Every
 * line of a group of equal keys in one file pairs with every line of the
 * group in the other.
 *
 * Without -t fields are separated by blanks, leading blanks are ignored and
 * the output is separated by a space; with -t the character separates
 * fields and output alike.  -a also writes the lines of a file that pair
 * with nothing, -v only those.  -o lists the fields to write (0 is the join
 * field, 1.n and 2.n the fields of each file), with -e for fields that are
 * missing.  - is standard input for one of the files.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most fields a line is split into. */
#define FIELD_MAX 1024

/* One field of the output list: file 0 is the join field. */
struct output_field {
	int file;
	unsigned long field;
};

/* A line and its fields. */
struct record {
	char *text;
	size_t length;
	size_t capacity;
	const char *fields[FIELD_MAX];
	size_t lengths[FIELD_MAX];
	size_t count;
};

/* The lines of one file with equal keys, and the line after them. */
struct group {
	struct record *records;
	size_t count;
	size_t capacity;
	struct record next;
	int have_next;
	FILE *stream;
	unsigned long key;
};

/* The options. */
struct options {
	int separator;			/* -t, or -1 for blanks */
	int unpaired[3];		/* -a */
	int only_unpaired;		/* -v */
	const char *empty;		/* -e */
	struct output_field *list;	/* -o */
	size_t list_count;
	unsigned long key[3];		/* -1 and -2 */
};

static struct options options;

static int read_options(int argc, char **argv);
static const char *option_argument(int argc, char **argv, int *index, const char *rest);
static unsigned long parse_field(const char *text);
static void parse_list(const char *text);
static FILE *open_input(const char *name);
static int read_record(struct group *group, struct record *record);
static void split_fields(struct record *record);
static void read_group(struct group *group);
static int compare_keys(const struct group *left, const struct group *right);
static void key_of(const struct group *group, const struct record *record, const char **text, size_t *length);
static void write_pair(const struct record *left, const struct record *right);
static void write_unpaired(int file, const struct record *record);
static void write_field(int *first, const char *text, size_t length);
static void write_default(const struct record *left, const struct record *right, int file);
static void write_listed(const struct record *left, const struct record *right, const struct record *either);
static void copy_record(struct record *to, const struct record *from);
static int is_blank(char value);
static void *allocate(void *memory, size_t size);
static void usage(void);

/*
 * Runs join.
 */
int
main(
	int argc,
	char **argv)
{
	struct group groups[2];
	size_t a;
	size_t b;
	int first;
	int result;

	/* The options and the two files. */
	options.separator = -1;
	options.key[1] = 1;
	options.key[2] = 1;
	first = read_options(argc, argv);
	if (argc - first != 2)
		usage();
	memset(groups, 0, sizeof(groups));
	groups[0].stream = open_input(argv[first]);
	groups[1].stream = open_input(argv[first + 1]);
	groups[0].key = options.key[1];
	groups[1].key = options.key[2];

	/* The first group of each. */
	groups[0].have_next = read_record(&groups[0], &groups[0].next);
	groups[1].have_next = read_record(&groups[1], &groups[1].next);
	read_group(&groups[0]);
	read_group(&groups[1]);

	/* Until both files end. */
	while (groups[0].count > 0 || groups[1].count > 0) {
		/* One file ended: the other's lines pair with nothing. */
		if (groups[1].count == 0)
			result = -1;
		else if (groups[0].count == 0)
			result = 1;
		else
			result = compare_keys(&groups[0], &groups[1]);

		/* The smaller key pairs with nothing. */
		if (result < 0) {
			for (a = 0; a < groups[0].count; a++)
				write_unpaired(1, &groups[0].records[a]);
			read_group(&groups[0]);
			continue;
		}

		/* The second is before: its lines pair with nothing. */
		if (result > 0) {
			for (b = 0; b < groups[1].count; b++)
				write_unpaired(2, &groups[1].records[b]);
			read_group(&groups[1]);
			continue;
		}

		/* Equal keys: every line with every line. */
		if (!options.only_unpaired) {
			for (a = 0; a < groups[0].count; a++) {
				for (b = 0; b < groups[1].count; b++) {
					write_pair(&groups[0].records[a],
						   &groups[1].records[b]);
				}
			}
		}

		/* The same key: every pair is written, then both go on. */
		read_group(&groups[0]);
		read_group(&groups[1]);
	}

	/* Succeeded. */
	return 0;
}

/* Reads the options; returns the index of the first file. */
static int
read_options(
	int argc,
	char **argv)
{
	const char *word;
	const char *argument;
	unsigned long file;
	int index;
	char letter;

	/* Each option word. */
	for (index = 1; index < argc; index++) {
		/* A file, or - alone, ends the options; so does --. */
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;
		if (word[1] == '-' && word[2] == '\0')
			return index + 1;

		/* Every option takes an argument. */
		letter = word[1];
		argument = option_argument(argc, argv, &index, word + 2);
		switch (letter) {
		case 'a':
		case 'v':
			file = parse_field(argument);
			if (file != 1 && file != 2)
				usage();
			options.unpaired[file] = 1;
			if (letter == 'v')
				options.only_unpaired = 1;
			break;
		case 'e':
			options.empty = argument;
			break;
		case 'o':
			parse_list(argument);
			break;
		case 't':
			if (argument[0] == '\0' || argument[1] != '\0')
				usage();
			options.separator = (unsigned char)argument[0];
			break;
		case '1':
			options.key[1] = parse_field(argument);
			break;
		case '2':
			options.key[2] = parse_field(argument);
			break;
		default:
			usage();
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

/* Reads a field or file number: digits, at least 1. */
static unsigned long
parse_field(
	const char *text)
{
	const char *cursor;
	unsigned long value;

	/* Digits only. */
	value = 0;
	for (cursor = text; *cursor >= '0' && *cursor <= '9'; cursor++)
		value = value * 10UL + (unsigned long)(*cursor - '0');
	if (cursor == text || *cursor != '\0' || value == 0)
		usage();

	/* Succeeded. */
	return value;
}

/* Parses an -o list: 0, 1.n and 2.n separated by commas or blanks. */
static void
parse_list(
	const char *text)
{
	struct output_field field;
	const char *cursor;
	size_t capacity;

	/* Each field of the list. */
	capacity = 0;
	cursor = text;
	for (;;) {
		/* 0, or file.field. */
		memset(&field, 0, sizeof(field));
		if (cursor[0] == '0') {
			cursor++;
		} else if ((cursor[0] == '1' || cursor[0] == '2') &&
			   cursor[1] == '.') {
			field.file = cursor[0] - '0';
			cursor += 2;
			if (*cursor < '0' || *cursor > '9')
				usage();
			while (*cursor >= '0' && *cursor <= '9') {
				field.field = field.field * 10UL +
				    (unsigned long)(*cursor - '0');
				cursor++;
			}

			/* No field 0 in a file. */
			if (field.field == 0)
				usage();
		} else {
			usage();
		}

		/* Kept. */
		if (options.list_count == capacity) {
			capacity = capacity * 2U + 8U;
			options.list = allocate(options.list,
			    capacity * sizeof(*options.list));
		}

		/* The field, added. */
		options.list[options.list_count] = field;
		options.list_count++;

		/* The next, after a comma or a blank. */
		if (*cursor == '\0')
			return;
		if (*cursor != ',' && *cursor != ' ' && *cursor != '\t')
			usage();
		cursor++;
	}
}

/* Opens a file; - is standard input. */
static FILE *
open_input(
	const char *name)
{
	FILE *stream;
	int compare;

	/* Standard input, or the file. */
	compare = strcmp(name, "-");
	if (compare == 0)
		return stdin;
	stream = fopen(name, "r");
	if (stream == NULL) {
		fprintf(stderr, "join: %s: %s\n", name, strerror(errno));
		exit(1);
	}

	/* Succeeded. */
	return stream;
}

/* Reads the next line of a file into a record.  Returns 0 at the end. */
static int
read_record(
	struct group *group,
	struct record *record)
{
	int value;
	int newline;

	/* Bytes up to the newline. */
	record->length = 0;
	newline = 0;
	for (;;) {
		/* The next byte. */
		value = getc(group->stream);
		if (value == EOF)
			break;
		if (value == '\n') {
			newline = 1;
			break;
		}

		/* Room for it and a NUL. */
		if (record->length + 2U > record->capacity) {
			record->capacity = record->capacity * 2U + 128U;
			record->text = allocate(record->text, record->capacity);
		}

		/* The byte. */
		record->text[record->length] = (char)value;
		record->length++;
	}

	/* The end of the file. */
	if (record->length == 0 && !newline)
		return 0;

	/* Succeeded: the line, split. */
	if (record->text == NULL) {
		record->capacity = 128U;
		record->text = allocate(NULL, record->capacity);
	}

	/* Succeeded: the line and its fields. */
	record->text[record->length] = '\0';
	split_fields(record);
	return 1;
}

/*
 * Splits a line into fields: at the -t character, or at runs of blanks
 * with the leading blanks ignored.
 */
static void
split_fields(
	struct record *record)
{
	size_t position;
	size_t start;
	int blank;

	/* No fields yet. */
	record->count = 0;
	position = 0;

	/* With -t: every separator ends a field. */
	if (options.separator >= 0) {
		for (;;) {
			start = position;
			while (position < record->length &&
			       (unsigned char)record->text[position] !=
			       options.separator)
				position++;
			if (record->count < FIELD_MAX) {
				record->fields[record->count] = record->text + start;
				record->lengths[record->count] = position - start;
				record->count++;
			}

			/* Past the separator, unless the line ended. */
			if (position >= record->length)
				return;
			position++;
		}
	}

	/* Without -t: runs of blanks separate fields. */
	for (;;) {
		/* Blanks, then a field of non-blanks. */
		for (; position < record->length; position++) {
			blank = is_blank(record->text[position]);
			if (!blank)
				break;
		}

		/* The end of the line. */
		if (position >= record->length)
			return;
		start = position;
		for (; position < record->length; position++) {
			blank = is_blank(record->text[position]);
			if (blank)
				break;
		}

		/* The field, when there is room for it. */
		if (record->count < FIELD_MAX) {
			record->fields[record->count] = record->text + start;
			record->lengths[record->count] = position - start;
			record->count++;
		}
	}
}

/*
 * Reads the next group: the line read ahead and every line after it with
 * the same key.
 */
static void
read_group(
	struct group *group)
{
	const char *key;
	const char *other;
	size_t key_length;
	size_t other_length;
	int compare;

	/* No lines yet. */
	group->count = 0;
	while (group->have_next) {
		/* A line with another key ends the group. */
		if (group->count > 0) {
			key_of(group, &group->records[0], &key, &key_length);
			key_of(group, &group->next, &other, &other_length);
			if (key_length != other_length)
				break;
			compare = memcmp(key, other, key_length);
			if (compare != 0)
				break;
		}

		/* The line joins the group. */
		if (group->count == group->capacity) {
			group->capacity = group->capacity * 2U + 4U;
			group->records = allocate(group->records,
			    group->capacity * sizeof(*group->records));
			memset(group->records + group->count, 0,
			       (group->capacity - group->count) *
			       sizeof(*group->records));
		}

		/* The line joins the group, and the next is read. */
		copy_record(&group->records[group->count], &group->next);
		group->count++;
		group->have_next = read_record(group, &group->next);
	}
}

/* Compares the keys of two groups. */
static int
compare_keys(
	const struct group *left,
	const struct group *right)
{
	const char *a;
	const char *b;
	size_t a_length;
	size_t b_length;
	size_t length;
	int result;

	/* The common part, then the shorter first. */
	key_of(left, &left->records[0], &a, &a_length);
	key_of(right, &right->records[0], &b, &b_length);
	length = a_length;
	if (b_length < length)
		length = b_length;
	result = memcmp(a, b, length);
	if (result != 0)
		return result;
	if (a_length < b_length)
		return -1;
	if (a_length > b_length)
		return 1;
	return 0;
}

/* Returns the join field of a line (empty when it has too few fields). */
static void
key_of(
	const struct group *group,
	const struct record *record,
	const char **text,
	size_t *length)
{
	/* The field, counted from 1. */
	if (group->key > record->count) {
		*text = "";
		*length = 0;
		return;
	}

	/* The key field. */
	*text = record->fields[group->key - 1U];
	*length = record->lengths[group->key - 1U];
}

/* Writes a pair of lines with equal keys. */
static void
write_pair(
	const struct record *left,
	const struct record *right)
{
	/* The -o list, or the default fields. */
	if (options.list_count > 0)
		write_listed(left, right, left);
	else
		write_default(left, right, 0);
	putchar('\n');
}

/* Writes a line that pairs with nothing, when -a or -v asks for it. */
static void
write_unpaired(
	int file,
	const struct record *record)
{
	/* Only the files -a or -v named. */
	if (!options.unpaired[file])
		return;

	/* The -o list with the other file missing, or the line's fields. */
	if (options.list_count > 0) {
		if (file == 1)
			write_listed(record, NULL, record);
		else
			write_listed(NULL, record, record);
	} else if (file == 1) {
		write_default(record, NULL, 1);
	} else {
		write_default(NULL, record, 2);
	}

	/* The line ends. */
	putchar('\n');
}

/* Writes a field, after the separator when it is not the first. */
static void
write_field(
	int *first,
	const char *text,
	size_t length)
{
	/* The separator: -t, or a space. */
	if (!*first) {
		if (options.separator >= 0)
			putchar(options.separator);
		else
			putchar(' ');
	}

	/* Past the first field. */
	*first = 0;

	/* The field. */
	if (length > 0)
		fwrite(text, 1, length, stdout);
}

/*
 * Writes the default fields: the join field, then the other fields of the
 * line of file1, then those of file2 (either may be missing for -a).
 */
static void
write_default(
	const struct record *left,
	const struct record *right,
	int file)
{
	const struct record *keyed;
	unsigned long key;
	size_t index;
	int first;

	/* The join field, from whichever line there is. */
	first = 1;
	keyed = left;
	key = options.key[1];
	if (file == 2) {
		keyed = right;
		key = options.key[2];
	}

	/* The key field of the file with the line. */
	if (key <= keyed->count) {
		write_field(&first, keyed->fields[key - 1U],
			    keyed->lengths[key - 1U]);
	} else {
		write_field(&first, "", 0);
	}

	/* The other fields of each line. */
	if (left != NULL) {
		for (index = 0; index < left->count; index++) {
			if (index + 1U == options.key[1])
				continue;
			write_field(&first, left->fields[index],
				    left->lengths[index]);
		}
	}

	/* The fields of the second line but its key. */
	if (right != NULL) {
		for (index = 0; index < right->count; index++) {
			if (index + 1U == options.key[2])
				continue;
			write_field(&first, right->fields[index],
				    right->lengths[index]);
		}
	}
}

/*
 * Writes the fields of the -o list; a missing one is the -e string.
 * either is the line that is there (for the join field).
 */
static void
write_listed(
	const struct record *left,
	const struct record *right,
	const struct record *either)
{
	const struct output_field *field;
	const struct record *record;
	const char *empty;
	unsigned long number;
	size_t index;
	int first;

	/* An empty field is -e, or nothing. */
	empty = "";
	if (options.empty != NULL)
		empty = options.empty;
	first = 1;
	for (index = 0; index < options.list_count; index++) {
		/* The join field, from whichever line there is. */
		field = &options.list[index];
		if (field->file == 0) {
			record = either;
			number = options.key[1];
			if (either == right)
				number = options.key[2];
		} else {
			record = left;
			if (field->file == 2)
				record = right;
			number = field->field;
		}

		/* The field, or the -e string. */
		if (record == NULL || number > record->count) {
			write_field(&first, empty, strlen(empty));
		} else {
			write_field(&first, record->fields[number - 1U],
				    record->lengths[number - 1U]);
		}
	}
}

/* Copies a record, with its own storage and fields. */
static void
copy_record(
	struct record *to,
	const struct record *from)
{
	size_t index;

	/* The text. */
	if (to->capacity < from->length + 1U) {
		to->capacity = from->length + 1U;
		to->text = allocate(to->text, to->capacity);
	}

	/* The text, copied. */
	memcpy(to->text, from->text, from->length + 1U);
	to->length = from->length;

	/* Succeeded: the fields, pointing into the copy. */
	to->count = from->count;
	for (index = 0; index < from->count; index++) {
		to->fields[index] = to->text + (from->fields[index] - from->text);
		to->lengths[index] = from->lengths[index];
	}
}

/* Reports whether a character is a blank. */
static int
is_blank(
	char value)
{
	/* Space and tab. */
	if (value == ' ' || value == '\t')
		return 1;
	return 0;
}

/* Allocates or resizes memory, ending join when there is none. */
static void *
allocate(
	void *memory,
	size_t size)
{
	void *result;

	/* The memory. */
	result = realloc(memory, size);
	if (result == NULL) {
		fprintf(stderr, "join: out of memory\n");
		exit(1);
	}

	/* Succeeded. */
	return result;
}

/* Reports the usage and ends join. */
static void
usage(
	void)
{
	/* The form. */
	fprintf(stderr, "usage: join [-a file_number|-v file_number] "
		"[-e string] [-o list] [-t char] [-1 field] [-2 field] "
		"file1 file2\n");
	exit(1);
}
