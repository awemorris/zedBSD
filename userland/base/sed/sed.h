/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The stream editor (POSIX XCU sed): what the script compiles to, and what
 * the files of sed offer one another.
 *
 * A script compiles to an array of commands.  A { command knows the index
 * after its }, and b and t know the index of their label (the end of the
 * script when they have none), so that running the script is a walk along
 * the array.
 */

#ifndef KERN_USERLAND_BASE_SED_SED_H
#define KERN_USERLAND_BASE_SED_SED_H

#include <regex.h>
#include <stddef.h>
#include <stdio.h>

/* The kinds of address. */
#define SED_ADDRESS_NONE	0	/* no address */
#define SED_ADDRESS_LINE	1	/* a line number */
#define SED_ADDRESS_LAST	2	/* $, the last line */
#define SED_ADDRESS_REGEX	3	/* a regular expression */
#define SED_ADDRESS_ZERO	4	/* GNU 0 of 0,/re/: before the first line */
#define SED_ADDRESS_STEP	5	/* GNU first~step */
#define SED_ADDRESS_PLUS	6	/* GNU addr,+N: N lines more */
#define SED_ADDRESS_MULTIPLE	7	/* GNU addr,~N: up to a multiple of N */

/*
 * An address: a line, the last line, the lines a regex matches, or one of
 * GNU's forms.  line is the number of a line address, the first of
 * first~step, and the N of +N and ~N.
 */
struct sed_address {
	int kind;
	unsigned long line;
	unsigned long step;

	/* The regex; NULL for an empty one, which is the last regex used. */
	regex_t *regex;
};

/*
 * A file that output goes to: standard output, or the file of a w command
 * or a w flag, shared by every command that names it.  missing_newline is
 * set when the last line written lacked the newline its input lacked; the
 * newline is written before anything else is.
 */
struct sed_output {
	char *name;
	FILE *stream;
	int missing_newline;
	struct sed_output *next;
};

/*
 * A file that R reads a line at a time, shared by every R that names it,
 * so that each reads the line after the one the last read.  It is opened
 * when first read; a file that cannot be read gives no lines.
 */
struct sed_reader {
	char *name;
	FILE *stream;
	int opened;
	struct sed_reader *next;
};

/* The s command: the regex, the replacement and the flags. */
struct sed_substitute {
	regex_t *regex;
	char *replacement;
	int global;
	unsigned long occurrence;
	int print;
	struct sed_output *output;

	/* GNU's e flag: the result is run as a command, and its output kept. */
	int evaluate;
};

/* One command of the script, with its addresses. */
struct sed_command {
	struct sed_address first;
	struct sed_address second;
	int negate;
	char name;

	/*
	 * Set while a range of two addresses is between them; a range of +N
	 * or ~N ends at range_end.  A range from 0 starts set.
	 */
	int in_range;
	unsigned long range_end;

	/* a, i, c: the text; b, t, :: the label; r: the file name. */
	char *text;

	/* {: the index of the matching }; b, t: the index of the label. */
	size_t jump;

	/* s: the substitution; y: the map of every byte; w, W: the file. */
	struct sed_substitute substitute;
	unsigned char *map;
	struct sed_output *output;

	/* R: the file a line is read from. */
	struct sed_reader *reader;

	/* q, Q: the exit status; l: the line length, when number_given. */
	int exit_status;
	int number_given;
};

/* A compiled script. */
struct sed_program {
	struct sed_command *commands;
	size_t count;
	size_t capacity;

	/* Set when the script began with #n, which is as -n. */
	int quiet;

	/* The files of w commands and flags, and of R. */
	struct sed_output *outputs;
	struct sed_reader *readers;

	/* Set by -E: the regexes are extended ones. */
	int extended;

	/* Set by --sandbox: commands that run programs or touch files fail. */
	int sandbox;
};

/*
 * What the command line asks of a run, besides the script.
 *
 * main.c fills it in from the options before the script runs; execute.c
 * only reads it.
 */
struct sed_settings {
	/* -n: the pattern space is written only when the script says so. */
	int quiet;

	/* -s: each file is a stream of its own; -i implies it. */
	int separate;

	/*
	 * -i: each file is replaced by the output, keeping a backup named by
	 * the suffix when there is one.
	 */
	int in_place;
	const char *suffix;
	int follow_symlinks;

	/* -z: lines end with a NUL byte instead of a newline. */
	int null_data;

	/* -l: the width of the lines of l (0 for no folding). */
	unsigned long line_length;

	/* -u: input is read and output written a line at a time. */
	int unbuffered;

	/* --posix or POSIXLY_CORRECT: N at the end quits without writing. */
	int posix;
};

/* Compiles a script (compile.c).  Returns 0 after a message for an error. */
int sed_compile(const char *script, struct sed_program *program);

/* Runs a compiled script over the files (execute.c).  Returns the status. */
int sed_execute(struct sed_program *program, char **files, int count, const struct sed_settings *settings);

/* Allocation that ends sed when there is no memory (main.c). */
void *sed_malloc(size_t size);
void *sed_realloc(void *memory, size_t size);
char *sed_strndup(const char *text, size_t length);

/* Reports an error and ends sed with status 1 (main.c). */
void sed_fatal(const char *message, const char *detail);

#endif
