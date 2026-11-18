/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The grep program: the options the command line sets, the patterns, and
 * what the two halves of grep offer each other.  main.c reads the command
 * line and walks the files (and the directories of -r); search.c reads one
 * input, matches its lines and writes what the options ask for.
 */

#ifndef KERN_USERLAND_BASE_GREP_GREP_H
#define KERN_USERLAND_BASE_GREP_GREP_H

#include <regex.h>
#include <stddef.h>

/* How patterns are read. */
#define GREP_MODE_BASIC		0	/* basic regular expressions */
#define GREP_MODE_EXTENDED	1	/* -E: extended regular expressions */
#define GREP_MODE_FIXED		2	/* -F: fixed strings */

/* What is written for the selected lines. */
#define GREP_OUTPUT_LINES	0	/* the lines (or their matches, -o) */
#define GREP_OUTPUT_COUNT	1	/* -c: how many */
#define GREP_OUTPUT_NAMES	2	/* -l: the names of files with any */
#define GREP_OUTPUT_UNMATCHED	3	/* -L: the names of files with none */
#define GREP_OUTPUT_QUIET	4	/* -q: nothing */

/* How a file that holds a NUL byte is treated. */
#define GREP_BINARY_REPORT	0	/* a message instead of its lines */
#define GREP_BINARY_TEXT	1	/* -a: as text */
#define GREP_BINARY_SKIP	2	/* -I: as if nothing matched */

/* Whether -r goes into directories, and whether it follows links there. */
#define GREP_RECURSE_NONE	0
#define GREP_RECURSE		1	/* -r: symbolic links only as operands */
#define GREP_RECURSE_FOLLOW	2	/* -R: every symbolic link */

/* Whether names go before what is written. */
#define GREP_NAMES_AUTO		(-1)	/* when there are several files */
#define GREP_NAMES_NEVER	0	/* -h */
#define GREP_NAMES_ALWAYS	1	/* -H */

/* A pattern: its text, and the compiled regex when it is one. */
struct grep_pattern {
	char *text;
	size_t length;
	regex_t regex;
	int empty;
};

/* A list of the globs of --include, --exclude or --exclude-dir. */
struct grep_globs {
	const char **items;
	size_t count;
	size_t capacity;
};

/*
 * Everything the command line asks for.
 *
 * It is filled in once, by main.c, before the first input is searched, and
 * only read after that.
 */
struct grep_options {
	int mode;
	int output;
	int ignore_case;
	int line_numbers;
	int byte_offset;
	int no_messages;
	int invert;
	int whole_line;
	int whole_word;
	int only_matching;
	int binary_files;
	int null_after_name;
	int null_data;
	int recursive;

	/* -H, -h or neither; and whether the operands are named, as follows. */
	int names;
	int show_names;

	/* Context: the lines before and after, and whether any was asked. */
	unsigned long before;
	unsigned long after;
	int context;
	const char *group_separator;

	/* -m: the most lines to select in a file; -1 for no limit. */
	long max_count;

	/* --label: the name standard input goes by. */
	const char *label;

	/* The globs that choose the files and directories of -r. */
	struct grep_globs include;
	struct grep_globs exclude;
	struct grep_globs exclude_dir;

	/* The patterns. */
	struct grep_pattern *patterns;
	size_t count;
	size_t capacity;
	int have_patterns;
};

/*
 * Searches one input, open on a descriptor, writing what the options ask
 * for, with the name before each line when show_names is set (search.c).
 * Returns 1 when a line was selected, 0 when none was, and -1 after a
 * message when the input could not be read.
 */
int grep_search(const struct grep_options *options, int descriptor, const char *name, int show_names);

/* Allocates or resizes memory, ending grep when there is none (main.c). */
void *grep_allocate(void *memory, size_t size);

#endif
