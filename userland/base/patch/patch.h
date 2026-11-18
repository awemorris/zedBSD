/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares what the parts of patch share: the lines of a file, the hunks
 * read from a patch, and the options.
 */

#ifndef USERLAND_BASE_PATCH_PATCH_H
#define USERLAND_BASE_PATCH_PATCH_H

#include <stddef.h>

/* The kinds of difference a patch holds. */
#define PATCH_UNKNOWN 0
#define PATCH_NORMAL 1
#define PATCH_CONTEXT 2
#define PATCH_UNIFIED 3
#define PATCH_ED 4

/*
 * Lines of text.
 *
 * Each line is held without its newline; last_unterminated says that the
 * last line had none.  The array owns its lines.
 */
struct patch_lines {
	char **items;
	long count;
	long capacity;
	int last_unterminated;
};

/*
 * One hunk: lines of the old file replaced by lines of the new one.
 *
 * old_start is the first line (1-based) of the old lines, or for an empty
 * old part the line they go after.  leading and trailing count the
 * context lines at both ends, which fuzz may leave unmatched.  For an ed
 * script, command holds the command line and new the text it adds.
 */
struct patch_hunk {
	long old_start;
	long new_start;
	struct patch_lines old;
	struct patch_lines new;
	long leading;
	long trailing;
	char *command;
	long raw_first;
	long raw_end;
};

/*
 * The differences for one file.
 *
 * The names are those of the headers and of an Index: line, as written
 * in the patch; the hunks are in order.
 */
struct patch_file {
	int format;
	char *old_name;
	char *new_name;
	char *index_name;
	struct patch_hunk *hunks;
	long hunk_count;
	long hunk_capacity;
};

/*
 * What the command line asks for.
 *
 * One instance lives for the run.  strip is -1 without -p.
 */
struct patch_options {
	int backup;
	int loose;
	int forward;
	int reverse;
	int format;
	const char *directory;
	const char *define;
	const char *input;
	const char *output;
	long strip;
	const char *reject;
	const char *file;
};

int patch_lines_add(struct patch_lines *lines, const char *text, size_t length);
void patch_lines_free(struct patch_lines *lines);
int patch_lines_load(const char *path, struct patch_lines *lines);
int patch_read(const struct patch_lines *patch, int forced_format, struct patch_file **files, long *count);
void patch_files_free(struct patch_file *files, long count);

#endif
