/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the line comparison of diff: loading two files as lines and
 * finding the lines they have in common.
 */

#ifndef USERLAND_BASE_DIFF_LINES_H
#define USERLAND_BASE_DIFF_LINES_H

#include <stddef.h>

/*
 * One file as lines.
 *
 * data holds the whole file; starts[i] is where line i starts and
 * starts[count] where the data ends.  ids[i] numbers the line so that two
 * lines that compare equal (under -b, after blanks are folded) share a
 * number.  missing_newline says the last line has no newline.
 */
struct diff_file {
	const char *label;
	char *data;
	size_t size;
	size_t *starts;
	long *ids;
	long count;
	int missing_newline;
};

/*
 * One block of lines that differ: lines [a_start, a_end) of the first file
 * are replaced by lines [b_start, b_end) of the second.  Either range may
 * be empty.
 */
struct diff_change {
	long a_start;
	long a_end;
	long b_start;
	long b_end;
};

int diff_load(const char *path, const char *label, struct diff_file *file);
void diff_release(struct diff_file *file);
int diff_number_lines(struct diff_file *first, struct diff_file *second, int fold_blanks);
int diff_compare(const struct diff_file *first, const struct diff_file *second, struct diff_change **changes, long *count);
size_t diff_line_length(const struct diff_file *file, long line);

#endif
