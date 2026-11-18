/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The stream editor (POSIX XCU sed, with the GNU extensions scripts use):
 * the options, and the script made of the -e and -f pieces.
 *
 *	sed [options] script [file...]
 *	sed [options] -e script [-e script]... [-f file]... [file...]
 *
 * The options are POSIX's -n, -e, -f and -E, and GNU's -r, -i[SUFFIX], -s,
 * -z, -l N, -u and the long forms.  Options may follow operands, as GNU
 * sed takes them (sed -i FILE -e SCRIPT), unless POSIXLY_CORRECT is set.
 *
 * The pieces of the script are joined with newlines in the order given, so
 * that a text of a, i or c may continue into the next piece.
 */

#include "userland/base/sed/sed.h"
#include "userland/base/common/command.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/kern/os-name.h"

/* The codes of the long options that have no letter. */
#define OPTION_POSIX		256
#define OPTION_DEBUG		257
#define OPTION_SANDBOX		258
#define OPTION_FOLLOW_SYMLINKS	259
#define OPTION_HELP		260
#define OPTION_VERSION		261

/* The status of an input sed cannot edit in place (GNU's). */
#define STATUS_PANIC 4

/*
 * The options written in full.
 *
 * The table is read by the scan of the command line only; the letter a
 * long option shares its code with makes the two forms one case.
 */
static const struct command_long_option sed_long_options[] = {
	{"debug", COMMAND_VALUE_NONE, OPTION_DEBUG},
	{"expression", COMMAND_VALUE_REQUIRED, 'e'},
	{"file", COMMAND_VALUE_REQUIRED, 'f'},
	{"follow-symlinks", COMMAND_VALUE_NONE, OPTION_FOLLOW_SYMLINKS},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"in-place", COMMAND_VALUE_OPTIONAL, 'i'},
	{"line-length", COMMAND_VALUE_REQUIRED, 'l'},
	{"null-data", COMMAND_VALUE_NONE, 'z'},
	{"posix", COMMAND_VALUE_NONE, OPTION_POSIX},
	{"quiet", COMMAND_VALUE_NONE, 'n'},
	{"regexp-extended", COMMAND_VALUE_NONE, 'E'},
	{"sandbox", COMMAND_VALUE_NONE, OPTION_SANDBOX},
	{"separate", COMMAND_VALUE_NONE, 's'},
	{"silent", COMMAND_VALUE_NONE, 'n'},
	{"unbuffered", COMMAND_VALUE_NONE, 'u'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{"zero-terminated", COMMAND_VALUE_NONE, 'z'},
	{NULL, 0, 0}
};

/* The script as it is put together. */
struct script {
	char *text;
	size_t length;
	size_t capacity;
	int pieces;
};

static int read_options(int argc, char **argv, struct script *script, struct sed_settings *settings, int *extended, int *sandbox);
static void apply_option(int code, const char *value, struct script *script, struct sed_settings *settings, int *extended, int *sandbox);
static unsigned long parse_line_length(const char *text);
static void script_add(struct script *script, const char *text, size_t length);
static void script_add_file(struct script *script, const char *name);
static void version(void);
static void usage(int status);

/*
 * Runs sed.
 */
int
main(
	int argc,
	char **argv)
{
	struct sed_program program;
	struct sed_settings settings;
	struct script script;
	char **operands;
	int count;
	int extended;
	int sandbox;
	int compiled;
	int status;

	/* The options and the script. */
	memset(&script, 0, sizeof(script));
	memset(&settings, 0, sizeof(settings));
	count = read_options(argc, argv, &script, &settings, &extended,
			     &sandbox);
	operands = argv + 1;
	if (script.pieces == 0) {
		/* Without -e or -f, the first operand is the script. */
		if (count == 0)
			usage(1);
		script_add(&script, operands[0], strlen(operands[0]));
		operands++;
		count--;
	}

	/* -i needs files to edit. */
	if (settings.in_place && count == 0) {
		fprintf(stderr, "sed: no input files\n");
		return STATUS_PANIC;
	}

	/* The script, compiled. */
	memset(&program, 0, sizeof(program));
	program.extended = extended;
	program.sandbox = sandbox;
	compiled = sed_compile(script.text, &program);
	if (!compiled)
		return 1;
	if (program.quiet)
		settings.quiet = 1;

	/* Succeeded: the status of running it over the files. */
	status = sed_execute(&program, operands, count, &settings);
	return status;
}

/*
 * Allocates memory, ending sed when there is none.
 */
void *
sed_malloc(
	size_t size)
{
	void *memory;

	/* At least one byte, so that NULL means failure. */
	if (size == 0)
		size = 1;
	memory = malloc(size);
	if (memory == NULL)
		sed_fatal("out of memory", NULL);

	/* Succeeded. */
	return memory;
}

/*
 * Resizes memory, ending sed when there is none.
 */
void *
sed_realloc(
	void *memory,
	size_t size)
{
	void *resized;

	/* At least one byte, so that NULL means failure. */
	if (size == 0)
		size = 1;
	resized = realloc(memory, size);
	if (resized == NULL)
		sed_fatal("out of memory", NULL);

	/* Succeeded. */
	return resized;
}

/*
 * Copies length bytes of text as a string.
 */
char *
sed_strndup(
	const char *text,
	size_t length)
{
	char *copy;

	/* The bytes and a terminating NUL. */
	copy = sed_malloc(length + 1U);
	memcpy(copy, text, length);
	copy[length] = '\0';

	/* Succeeded. */
	return copy;
}

/*
 * Reports an error, with a detail when there is one, and ends sed.
 */
void
sed_fatal(
	const char *message,
	const char *detail)
{
	/* The message. */
	fflush(stdout);
	if (detail != NULL)
		fprintf(stderr, "sed: %s: %s\n", message, detail);
	else
		fprintf(stderr, "sed: %s\n", message);

	/* The status of a usage or script error. */
	exit(1);
}

/*
 * Reads the options, adding -e and -f pieces to the script.  Returns the
 * number of operands, which are left in argv from argv[1] on.
 */
static int
read_options(
	int argc,
	char **argv,
	struct script *script,
	struct sed_settings *settings,
	int *extended,
	int *sandbox)
{
	struct command_options scan;
	const char *posix;
	int code;

	/* No options yet; POSIXLY_CORRECT asks for POSIX's N. */
	*extended = 0;
	*sandbox = 0;
	settings->line_length = 70;
	posix = getenv("POSIXLY_CORRECT");
	if (posix != NULL)
		settings->posix = 1;

	/* The scan of the command line. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "sed";
	scan.letters = "Enrsuze:f:i::l:";
	scan.names = sed_long_options;
	command_options_start(&scan);

	/* Each option in turn. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;
		if (code == COMMAND_OPTION_ERROR)
			usage(1);
		apply_option(code, scan.value, script, settings, extended,
			     sandbox);
	}

	/* Succeeded: the operands follow argv[0]. */
	return scan.operand_count;
}

/* Applies one option. */
static void
apply_option(
	int code,
	const char *value,
	struct script *script,
	struct sed_settings *settings,
	int *extended,
	int *sandbox)
{
	/* The option of its code. */
	switch (code) {
	case 'n':
		settings->quiet = 1;
		break;
	case 'E':
	case 'r':
		*extended = 1;
		break;
	case 'e':
		/* -e script: a piece of the script. */
		script_add(script, value, strlen(value));
		break;
	case 'f':
		/* -f file: a piece read from a file. */
		script_add_file(script, value);
		break;
	case 'i':
		/* -i[SUFFIX]: in place, which treats the files separately. */
		settings->in_place = 1;
		settings->separate = 1;
		settings->suffix = NULL;
		if (value != NULL && value[0] != '\0')
			settings->suffix = value;
		break;
	case 's':
		settings->separate = 1;
		break;
	case 'z':
		settings->null_data = 1;
		break;
	case 'l':
		settings->line_length = parse_line_length(value);
		break;
	case 'u':
		settings->unbuffered = 1;
		break;
	case OPTION_POSIX:
		settings->posix = 1;
		break;
	case OPTION_SANDBOX:
		*sandbox = 1;
		break;
	case OPTION_FOLLOW_SYMLINKS:
		settings->follow_symlinks = 1;
		break;
	case OPTION_DEBUG:
		/* The annotations of --debug are not written. */
		break;
	case OPTION_VERSION:
		version();
		break;
	case OPTION_HELP:
		usage(0);
		break;
	default:
		usage(1);
	}
}

/* Reads the width of -l, ending sed when it is not a number. */
static unsigned long
parse_line_length(
	const char *text)
{
	unsigned long width;
	const char *cursor;

	/* At least one digit, and nothing but digits. */
	if (*text == '\0')
		sed_fatal("invalid line length", text);
	width = 0;
	for (cursor = text; *cursor != '\0'; cursor++) {
		if (*cursor < '0' || *cursor > '9')
			sed_fatal("invalid line length", text);
		width = width * 10UL + (unsigned long)(*cursor - '0');
	}

	/* Succeeded. */
	return width;
}

/* Adds a piece to the script, after a newline when it is not the first. */
static void
script_add(
	struct script *script,
	const char *text,
	size_t length)
{
	size_t needed;

	/* Room for a newline, the piece and a NUL. */
	needed = script->length + length + 2U;
	if (needed > script->capacity) {
		script->capacity = needed * 2U;
		script->text = sed_realloc(script->text, script->capacity);
	}

	/* The newline between pieces, then the piece. */
	if (script->pieces > 0)
		script->text[script->length++] = '\n';
	memcpy(script->text + script->length, text, length);
	script->length += length;
	script->text[script->length] = '\0';
	script->pieces++;
}

/* Adds the contents of a file to the script, less a final newline. */
static void
script_add_file(
	struct script *script,
	const char *name)
{
	char chunk[4096];
	char *text;
	size_t length;
	size_t count;
	FILE *stream;
	int compare;

	/* The file; - is standard input. */
	stream = stdin;
	compare = strcmp(name, "-");
	if (compare != 0)
		stream = fopen(name, "r");
	if (stream == NULL)
		sed_fatal(name, strerror(errno));

	/* Its whole contents. */
	text = NULL;
	length = 0;
	for (;;) {
		count = fread(chunk, 1, sizeof(chunk), stream);
		if (count == 0)
			break;
		text = sed_realloc(text, length + count);
		memcpy(text + length, chunk, count);
		length += count;
	}

	/* The file is done with. */
	if (stream != stdin)
		fclose(stream);

	/* The final newline separates pieces anyway. */
	if (length > 0 && text[length - 1U] == '\n')
		length--;
	script_add(script, text, length);
	free(text);
}

/* Writes the version and ends sed. */
static void
version(
	void)
{
	/* The name and where it comes from. */
	printf("sed (" ZEDBSD_OS_NAME ") 1.0\n");
	exit(0);
}

/* Reports the usage (on standard output for --help) and ends sed. */
static void
usage(
	int status)
{
	FILE *stream;

	/* --help writes to standard output and succeeds. */
	stream = stderr;
	if (status == 0)
		stream = stdout;

	/* The two forms and the options. */
	fprintf(stream, "usage: sed [-nErsuz] [-i[suffix]] [-l length] "
		"script [file...]\n"
		"       sed [-nErsuz] [-i[suffix]] [-l length] [-e script]... "
		"[-f file]... [file...]\n");
	exit(status);
}
