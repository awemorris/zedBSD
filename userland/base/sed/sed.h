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

/* An address: a line, the last line, or the lines a regex matches. */
struct sed_address {
	int kind;
	unsigned long line;

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

/* The s command: the regex, the replacement and the flags. */
struct sed_substitute {
	regex_t *regex;
	char *replacement;
	int global;
	unsigned long occurrence;
	int print;
	struct sed_output *output;
};

/* One command of the script, with its addresses. */
struct sed_command {
	struct sed_address first;
	struct sed_address second;
	int negate;
	char name;

	/* Set while a range of two addresses is between them. */
	int in_range;

	/* a, i, c: the text; b, t, :: the label; r: the file name. */
	char *text;

	/* {: the index of the matching }; b, t: the index of the label. */
	size_t jump;

	/* s: the substitution; y: the map of every byte; w: the file. */
	struct sed_substitute substitute;
	unsigned char *map;
	struct sed_output *output;

	/* q: the exit status. */
	int exit_status;
};

/* A compiled script. */
struct sed_program {
	struct sed_command *commands;
	size_t count;
	size_t capacity;

	/* Set when the script began with #n, which is as -n. */
	int quiet;

	/* The files of w commands and flags. */
	struct sed_output *outputs;

	/* Set by -E: the regexes are extended ones. */
	int extended;
};

/* Compiles a script (compile.c).  Returns 0 after a message for an error. */
int sed_compile(const char *script, struct sed_program *program);

/* Runs a compiled script over the files (execute.c).  Returns the status. */
int sed_execute(struct sed_program *program, char **files, int count, int quiet);

/* Allocation that ends sed when there is no memory (main.c). */
void *sed_malloc(size_t size);
void *sed_realloc(void *memory, size_t size);
char *sed_strndup(const char *text, size_t length);

/* Reports an error and ends sed with status 1 (main.c). */
void sed_fatal(const char *message, const char *detail);

#endif
