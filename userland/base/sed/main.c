/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The stream editor (POSIX XCU sed): the options, and the script made of
 * the -e and -f pieces.
 *
 *	sed [-nE] script [file...]
 *	sed [-nE] -e script [-e script]... [-f file]... [file...]
 *
 * The pieces of the script are joined with newlines in the order given, so
 * that a text of a, i or c may continue into the next piece.
 */

#include "userland/base/sed/sed.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The script as it is put together. */
struct script {
	char *text;
	size_t length;
	size_t capacity;
	int pieces;
};

static int read_options(int argc, char **argv, struct script *script, int *quiet, int *extended);
static int option_word(int argc, char **argv, int *index, struct script *script, int *quiet, int *extended);
static const char *option_argument(int argc, char **argv, int *index, const char *rest);
static void script_add(struct script *script, const char *text, size_t length);
static void script_add_file(struct script *script, const char *name);
static void usage(void);

/*
 * Runs sed.
 */
int
main(
	int argc,
	char **argv)
{
	struct sed_program program;
	struct script script;
	int quiet;
	int extended;
	int index;
	int compiled;
	int status;

	/* The options and the script. */
	memset(&script, 0, sizeof(script));
	index = read_options(argc, argv, &script, &quiet, &extended);
	if (script.pieces == 0) {
		/* Without -e or -f, the first operand is the script. */
		if (index >= argc)
			usage();
		script_add(&script, argv[index], strlen(argv[index]));
		index++;
	}

	/* The script, compiled. */
	memset(&program, 0, sizeof(program));
	program.extended = extended;
	compiled = sed_compile(script.text, &program);
	if (!compiled)
		return 1;
	if (program.quiet)
		quiet = 1;

	/* Succeeded: the status of running it over the files. */
	status = sed_execute(&program, argv + index, argc - index, quiet);
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
 * index of the first operand.
 */
static int
read_options(
	int argc,
	char **argv,
	struct script *script,
	int *quiet,
	int *extended)
{
	const char *word;
	int index;
	int more;

	/* No options yet. */
	*quiet = 0;
	*extended = 0;
	for (index = 1; index < argc; index++) {
		/* An operand, or - alone, ends the options. */
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;

		/* -- ends them too. */
		if (word[1] == '-' && word[2] == '\0') {
			index++;
			break;
		}

		/* The letters of the word. */
		more = option_word(argc, argv, &index, script, quiet, extended);
		if (!more)
			usage();
	}

	/* Succeeded: the first operand. */
	return index;
}

/*
 * Reads the letters of one option word; -e and -f take the rest of the
 * word or the next word.  Returns 0 for an unknown letter.
 */
static int
option_word(
	int argc,
	char **argv,
	int *index,
	struct script *script,
	int *quiet,
	int *extended)
{
	const char *letter;
	const char *argument;

	/* Each letter of the word. */
	for (letter = argv[*index] + 1; *letter != '\0'; letter++) {
		switch (*letter) {
		case 'n':
			*quiet = 1;
			break;
		case 'E':
		case 'r':
			*extended = 1;
			break;
		case 'e':
			/* -e script: a piece of the script. */
			argument = option_argument(argc, argv, index, letter + 1);
			script_add(script, argument, strlen(argument));
			return 1;
		case 'f':
			/* -f file: a piece read from a file. */
			argument = option_argument(argc, argv, index, letter + 1);
			script_add_file(script, argument);
			return 1;
		default:
			fprintf(stderr, "sed: unknown option -- '%c'\n", *letter);
			return 0;
		}
	}

	/* Succeeded. */
	return 1;
}

/*
 * Returns the argument of an option: the rest of its word, or the next
 * word.  A missing one is a usage error.
 */
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
	if (*index + 1 >= argc) {
		fprintf(stderr, "sed: option requires an argument\n");
		usage();
	}

	/* The next word is the argument. */
	(*index)++;

	/* Succeeded. */
	return argv[*index];
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

/* Reports the usage and ends sed. */
static void
usage(
	void)
{
	/* The two forms. */
	fprintf(stderr, "usage: sed [-nE] script [file...]\n"
		"       sed [-nE] [-e script]... [-f file]... [file...]\n");
	exit(1);
}
