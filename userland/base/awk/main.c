/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pattern scanning language (POSIX XCU awk).
 *
 *	awk [-F sepstring] [-v assignment]... program [argument...]
 *	awk [-F sepstring] -f progfile [-f progfile]... [-v assignment]...
 *	    [argument...]
 *
 * The program is parsed, the special variables, ARGV and ENVIRON are set,
 * the -v assignments are made, and then the BEGIN rules run, the main
 * rules run on each record of the input (when there are main or END
 * rules), and the END rules run.
 *
 * gawk's options are taken too: -e program-text (--source), which may be
 * given with -f, the long forms (--field-separator, --assign, --file),
 * --version, --posix (system() then gives the status as wait does, as
 * with POSIXLY_CORRECT), and --traditional, which changes nothing here.
 * The options end at the program or the first operand, as in gawk.
 */

#include "userland/base/awk/awk.h"
#include "userland/base/common/command.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "include/kern/os-name.h"

/* The codes of the long options that have no letter. */
#define OPTION_VERSION	256
#define OPTION_IGNORED	257
#define OPTION_HELP	258
#define OPTION_POSIX	259

/*
 * The options written in full.
 *
 * The table is read by the scan of the command line only; the letter a
 * long option shares its code with makes the two forms one case.
 */
static const struct command_long_option awk_long_options[] = {
	{"assign", COMMAND_VALUE_REQUIRED, 'v'},
	{"field-separator", COMMAND_VALUE_REQUIRED, 'F'},
	{"file", COMMAND_VALUE_REQUIRED, 'f'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"posix", COMMAND_VALUE_NONE, OPTION_POSIX},
	{"re-interval", COMMAND_VALUE_NONE, OPTION_IGNORED},
	{"source", COMMAND_VALUE_REQUIRED, 'e'},
	{"traditional", COMMAND_VALUE_NONE, OPTION_IGNORED},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{NULL, 0, 0}
};

/* A special variable, and the value it starts with. */
struct special_default {
	const char *name;
	int special;
	const char *text;
	double number;
};

/*
 * The special variables and their first values: text, or the number when
 * text is NULL.  ENVIRON and ARGV, which are arrays, and ARGC are set from
 * the environment and the operands.  The table is constant.
 */
static const struct special_default special_defaults[] = {
	{ "NF", SPECIAL_NF, NULL, 0 },
	{ "NR", SPECIAL_NR, NULL, 0 },
	{ "FNR", SPECIAL_FNR, NULL, 0 },
	{ "FS", SPECIAL_FS, " ", 0 },
	{ "OFS", SPECIAL_OFS, " ", 0 },
	{ "ORS", SPECIAL_ORS, "\n", 0 },
	{ "RS", SPECIAL_RS, "\n", 0 },
	{ "SUBSEP", SPECIAL_SUBSEP, "\034", 0 },
	{ "CONVFMT", SPECIAL_CONVFMT, "%.6g", 0 },
	{ "OFMT", SPECIAL_OFMT, "%.6g", 0 },
	{ "RSTART", SPECIAL_RSTART, NULL, 0 },
	{ "RLENGTH", SPECIAL_RLENGTH, NULL, -1 },
	{ "FILENAME", SPECIAL_FILENAME, "", 0 },
	{ "ENVIRON", SPECIAL_ENVIRON, NULL, 0 },
	{ "ARGC", SPECIAL_ARGC, NULL, 0 },
	{ "ARGV", SPECIAL_ARGV, NULL, 0 },
	{ NULL, 0, NULL, 0 }
};

/*
 * The state of the program being run, shared by every file of awk.  It is
 * filled by the parser and by main and lives until awk ends.
 */
struct awk_state awk;

/* The environment, which ENVIRON is made from. */
extern char **environ;

static void set_specials(void);
static void set_environment(void);
static void set_arguments(int count, char **operands);
static void read_program_file(const char *name, struct buffer *program);
static void usage(void);

/*
 * Runs awk.
 */
int
main(
	int argc,
	char **argv)
{
	struct command_options scan;
	const char *posix;
	struct buffer program;
	struct buffer record;
	struct value separator;
	const char *word;
	const char *argument;
	char **assignments;
	size_t assignment_count;
	size_t index;
	int have_file;
	int assigned;
	int flow;
	int more;
	int first;
	int code;

	/* The variables awk gives a meaning to, and whether POSIX is asked. */
	set_specials();
	posix = getenv("POSIXLY_CORRECT");
	if (posix != NULL)
		awk.posix = 1;

	/* The options: -F, -v, -f and -e, until the program or --. */
	memset(&program, 0, sizeof(program));
	memset(&separator, 0, sizeof(separator));
	assignments = awk_allocate(sizeof(*assignments) * (size_t)argc);
	assignment_count = 0;
	have_file = 0;
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "awk";
	scan.letters = "F:v:f:e:";
	scan.names = awk_long_options;
	command_options_start(&scan);
	scan.permute = 0;
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;

		/* The option of its code. */
		argument = scan.value;
		switch (code) {
		case 'F':
			value_set_text(&separator, argument, strlen(argument));
			break;
		case 'v':
			assignments[assignment_count] = (char *)argument;
			assignment_count++;
			break;
		case 'f':
			/* A file of the program, after a newline. */
			if (have_file)
				buffer_append_byte(&program, '\n');
			read_program_file(argument, &program);
			have_file = 1;
			break;
		case 'e':
			/* gawk's -e: program text, after a newline. */
			if (have_file)
				buffer_append_byte(&program, '\n');
			buffer_append(&program, argument, strlen(argument));
			have_file = 1;
			break;
		case OPTION_VERSION:
			printf("awk (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
		case OPTION_POSIX:
			awk.posix = 1;
			break;
		case OPTION_IGNORED:
			/* Nothing changes. */
			break;
		default:
			usage();
		}
	}

	/* The operands follow argv[0]. */
	first = 1;
	argc = first + scan.operand_count;

	/* The program text, when no -f gave it. */
	if (!have_file) {
		if (first >= argc)
			usage();
		buffer_append(&program, argv[first], strlen(argv[first]));
		first++;
	}

	/* The text ends with a NUL. */
	buffer_append(&program, "", 0);

	/* The program, parsed. */
	parse_program(program.data, program.length);

	/* FS from -F, with the escapes of a string; t alone is a tab. */
	if (separator.text != NULL) {
		if (separator.length == 1 && separator.text[0] == 't')
			value_set_text(&separator, "\t", 1);
		word = separator.text;
		argument = "FS=";
		memset(&record, 0, sizeof(record));
		buffer_append(&record, argument, strlen(argument));
		buffer_append(&record, word, strlen(word));
		command_assignment(record.data);
		free(record.data);
		value_free(&separator);
	}

	/* The environment, the operands, and the -v assignments. */
	set_environment();
	set_arguments(argc - first, argv + first);
	for (index = 0; index < assignment_count; index++) {
		assigned = command_assignment(assignments[index]);
		if (!assigned) {
			fprintf(stderr, "awk: invalid -v argument: %s\n", assignments[index]);
			exit(2);
		}
	}

	/* The list of assignments is done with. */
	free(assignments);

	/* BEGIN. */
	run_rules(awk.begin_rules);

	/* The main rules on each record, when there are rules to read for. */
	memset(&record, 0, sizeof(record));
	if (awk.main_rules != NULL || awk.end_rules != NULL) {
		for (;;) {
			more = input_next(&record);
			if (!more)
				break;
			record_set(record.data, record.length);
			flow = run_main_rules();
			if (flow == FLOW_NEXTFILE)
				input_skip_file();
		}
	}

	/* The record's buffer goes. */
	free(record.data);

	/* Succeeded: END, and the end. */
	run_exit();
	return 0;
}

/* Makes the special variables with their first values. */
static void
set_specials(
	void)
{
	const struct special_default *entry;
	struct variable *variable;

	/* Each variable, marked with its meaning. */
	for (entry = special_defaults; entry->name != NULL; entry++) {
		variable = variable_find(entry->name, 1);
		variable->special = entry->special;
		awk.specials[entry->special] = variable;
		if (entry->special == SPECIAL_ENVIRON || entry->special == SPECIAL_ARGV)
			continue;
		variable->cell.kind = CELL_SCALAR;
		if (entry->text != NULL)
			value_set_text(&variable->cell.value, entry->text, strlen(entry->text));
		else
			value_set_number(&variable->cell.value, entry->number);
	}
}

/* Fills ENVIRON from the environment. */
static void
set_environment(
	void)
{
	struct array *array;
	struct element *element;
	const char *equals;
	char **entry;

	/* Each name=value, as input. */
	array = cell_array(&awk.specials[SPECIAL_ENVIRON]->cell);
	for (entry = environ; entry != NULL && *entry != NULL; entry++) {
		equals = strchr(*entry, '=');
		if (equals == NULL)
			continue;
		element = array_find(array, *entry, (size_t)(equals - *entry), 1);
		value_set_input(&element->value, equals + 1, strlen(equals + 1));
	}
}

/* Fills ARGV with awk and the operands, and ARGC with their number. */
static void
set_arguments(
	int count,
	char **operands)
{
	struct array *array;
	struct element *element;
	char key[32];
	int key_length;
	int index;

	/* ARGV[0] is the name of the command. */
	array = cell_array(&awk.specials[SPECIAL_ARGV]->cell);
	element = array_find(array, "0", 1, 1);
	value_set_text(&element->value, "awk", 3);

	/* Each operand, as input. */
	for (index = 0; index < count; index++) {
		key_length = snprintf(key, sizeof(key), "%d", index + 1);
		element = array_find(array, key, (size_t)key_length, 1);
		value_set_input(&element->value, operands[index], strlen(operands[index]));
	}

	/* Succeeded: ARGC, and the operands are read from ARGV[1]. */
	value_set_number(&awk.specials[SPECIAL_ARGC]->cell.value, (double)(count + 1));
	awk.argument_index = 1;
}

/* Appends a program file (- is standard input) to the program text. */
static void
read_program_file(
	const char *name,
	struct buffer *program)
{
	char block[4096];
	FILE *stream;
	size_t length;
	int compare;

	/* The file. */
	compare = strcmp(name, "-");
	stream = stdin;
	if (compare != 0)
		stream = fopen(name, "r");
	if (stream == NULL)
		awk_fatal("can't open source file %s (%s)", name, strerror(errno));

	/* Its contents. */
	for (;;) {
		length = fread(block, 1, sizeof(block), stream);
		if (length == 0)
			break;
		buffer_append(program, block, length);
	}

	/* Succeeded. */
	if (stream != stdin)
		fclose(stream);
}

/* Reports the usage and ends awk. */
static void
usage(
	void)
{
	/* The forms. */
	fprintf(stderr, "usage: awk [-F fs] [-v var=value] [prog | -f progfile] [file ...]\n");
	exit(2);
}
