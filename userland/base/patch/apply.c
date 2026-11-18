/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Applies the hunks of one file's differences to its lines.
 *
 * Each hunk is looked for at the line it names, moved by how far the
 * hunks before it landed from theirs, then at lines ever further away
 * on both sides, never before the end of the hunk before it.  A hunk
 * that is not found whole is tried again with one, then two, of its
 * context lines at each end left unmatched (fuzz).  A hunk that still is
 * not found is rejected; with -N a hunk whose new lines are already there
 * is skipped instead.  -l compares lines with their blanks folded; -D
 * keeps both versions between #ifdef lines.
 *
 * An ed script is applied by running its commands, as diff -e writes
 * them, on the lines.
 */

#include "userland/base/patch/apply.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most context lines fuzz leaves unmatched at each end. */
#define PATCH_FUZZ_MAX 2

static void shared_context(const struct patch_hunk *hunk, const struct patch_lines *old, const struct patch_lines *new, long *leading, long *trailing);
static int find_hunk(const struct patch_options *options, const struct patch_lines *text, const struct patch_lines *old, long first, long last, long expected, long lowest, long *found);
static int lines_match(const struct patch_options *options, const struct patch_lines *text, long position, const struct patch_lines *old, long first, long last);
static int same_line(const struct patch_options *options, const char *left, const char *right);
static int add_range(struct patch_lines *out, const struct patch_lines *from, long first, long last);
static int add_text(struct patch_lines *out, const char *text);
static int add_defined(const struct patch_options *options, struct patch_lines *out, const struct patch_lines *text, long text_first, long text_last, const struct patch_lines *new, long new_first, long new_last);
static int ed_address(const char *text, long *first, long *last, const char **end);

/*
 * Applies the hunks of a file to its lines, building the result.  Each
 * hunk's outcome is stored in outcomes: 0 applied, 1 rejected, 2 skipped
 * as already applied.
 */
int
patch_apply(
	const struct patch_options *options,
	const struct patch_file *file,
	const struct patch_lines *text,
	struct patch_lines *result,
	int *outcomes)
{
	const struct patch_hunk *hunk;
	const struct patch_lines *old;
	const struct patch_lines *new;
	long index;
	long cursor;
	long offset;
	long expected;
	long found;
	long fuzz;
	long trim_top;
	long trim_bottom;
	long old_start;
	long leading;
	long trailing;
	long end;
	int status;
	int located;

	/* Copies the text hunk by hunk. */
	memset(result, 0, sizeof(*result));
	result->last_unterminated = text->last_unterminated;
	cursor = 0;
	offset = 0;
	for (index = 0; index < file->hunk_count; index++) {
		hunk = &file->hunks[index];
		old = &hunk->old;
		new = &hunk->new;
		old_start = hunk->old_start;
		if (options->reverse) {
			old = &hunk->new;
			new = &hunk->old;
			old_start = hunk->new_start;
		}

		/* The context at the ends, which both sides share. */
		shared_context(hunk, old, new, &leading, &trailing);

		/* Looks for the hunk, with more fuzz each time. */
		located = 0;
		trim_top = 0;
		trim_bottom = 0;
		found = 0;
		for (fuzz = 0; fuzz <= PATCH_FUZZ_MAX && !located; fuzz++) {
			trim_top = fuzz;
			if (trim_top > leading)
				trim_top = leading;
			trim_bottom = fuzz;
			if (trim_bottom > trailing)
				trim_bottom = trailing;
			if (fuzz > 0 && trim_top == 0 && trim_bottom == 0)
				break;

			/* The old lines name their first line, or the line an insertion follows. */
			expected = old_start - 1;
			if (old->count == 0)
				expected = old_start;
			expected += offset + trim_top;
			status = find_hunk(options, text, old, trim_top, old->count - trim_bottom, expected, cursor, &found);
			if (status == 0)
				located = 1;
		}

		/* A hunk not found is rejected, or skipped with -N when already applied. */
		if (!located) {
			outcomes[index] = PATCH_REJECTED;
			if (options->forward) {
				status = find_hunk(options, text, new, 0, new->count, old_start - 1 + offset, cursor, &found);
				if (status == 0)
					outcomes[index] = PATCH_SKIPPED;
			}

			/* The hunk is not applied. */
			continue;
		}

		/* The hunk is applied. */
		outcomes[index] = PATCH_APPLIED;

		/* Where the old lines end in the text. */
		end = found + (old->count - trim_top - trim_bottom);

		/*
		 * The text before the hunk and its leading context as the file
		 * has it, the new lines in place of the old, then the trailing
		 * context as the file has it.
		 */
		status = add_range(result, text, cursor, found + leading - trim_top);
		if (status != 0)
			return -1;
		if (options->define != NULL)
			status = add_defined(options, result, text, found + leading - trim_top, end - (trailing - trim_bottom), new, leading, new->count - trailing);
		else
			status = add_range(result, new, leading, new->count - trailing);
		if (status == 0)
			status = add_range(result, text, end - (trailing - trim_bottom), end);
		if (status != 0)
			return -1;

		/* The hunks after this one are expected as far off as it was. */
		cursor = end;
		offset = found - trim_top - (old_start - 1);
		if (old->count == 0)
			offset = found - old_start;
		if (cursor == text->count && trim_bottom == 0)
			result->last_unterminated = new->last_unterminated;
	}

	/* The text after the last hunk. */
	status = add_range(result, text, cursor, text->count);
	if (status != 0)
		return -1;

	/* Succeeded: the result is built. */
	return 0;
}

/*
 * Runs the commands of an ed script, as diff -e writes them, on the
 * lines: a, c and d with one or two line numbers, the bare a and the
 * s/.// that fix a line holding a lone dot.
 */
int
patch_apply_ed(
	const struct patch_file *file,
	const struct patch_lines *text,
	struct patch_lines *result)
{
	struct patch_lines work;
	struct patch_lines next;
	const struct patch_hunk *hunk;
	const char *command;
	const char *end;
	long index;
	long current;
	long first;
	long last;
	int status;
	int letter;
	int compare;
	int digit;
	char *fixed;

	/* Works on a copy of the lines. */
	memset(&work, 0, sizeof(work));
	status = add_range(&work, text, 0, text->count);
	if (status != 0)
		return -1;
	work.last_unterminated = text->last_unterminated;
	current = work.count;

	/* Runs each command. */
	for (index = 0; index < file->hunk_count; index++) {
		hunk = &file->hunks[index];
		command = hunk->command;

		/* s/.// drops the first dot of the current line. */
		compare = strcmp(command, "s/.//");
		if (compare == 0) {
			if (current >= 1 && current <= work.count) {
				fixed = work.items[current - 1];
				if (fixed[0] == '.')
					memmove(fixed, fixed + 1, strlen(fixed));
			}

			/* On to the next command. */
			continue;
		}

		/* The addresses: none means the current line. */
		first = current;
		last = current;
		end = command;
		digit = isdigit((unsigned char)command[0]);
		if (digit) {
			status = ed_address(command, &first, &last, &end);
			if (status != 0)
				return -1;
		}

		/* The command letter after them. */
		letter = *end;
		if (first < 0 || last > work.count || first > last)
			return -1;
		if (letter != 'a' && first < 1)
			return -1;

		/* Rebuilds the lines around the change. */
		memset(&next, 0, sizeof(next));
		if (letter == 'a') {
			status = add_range(&next, &work, 0, first);
			if (status == 0)
				status = add_range(&next, &hunk->new, 0, hunk->new.count);
			if (status == 0)
				status = add_range(&next, &work, first, work.count);
			current = first + hunk->new.count;
		} else if (letter == 'd' || letter == 'c') {
			status = add_range(&next, &work, 0, first - 1);
			if (status == 0 && letter == 'c')
				status = add_range(&next, &hunk->new, 0, hunk->new.count);
			if (status == 0)
				status = add_range(&next, &work, last, work.count);
			current = first - 1;
			if (letter == 'c')
				current += hunk->new.count;
		} else {
			return -1;
		}

		/* Stops on a lack of memory. */
		if (status != 0)
			return -1;

		/* The new lines replace the old. */
		next.last_unterminated = work.last_unterminated;
		patch_lines_free(&work);
		work = next;
	}

	/* Succeeded: the result. */
	*result = work;
	return 0;
}

/*
 * Counts the context lines at the ends of a hunk that both sides share.
 * A context hunk marks its sides apart, so the old side's context may
 * reach past a line the new side adds; only lines equal on both sides
 * count.
 */
static void
shared_context(
	const struct patch_hunk *hunk,
	const struct patch_lines *old,
	const struct patch_lines *new,
	long *leading,
	long *trailing)
{
	long shortest;
	long count;
	int compare;

	/* The context can reach no further than the shorter side. */
	shortest = old->count;
	if (new->count < shortest)
		shortest = new->count;

	/* Equal lines at the start, as far as the context goes. */
	count = 0;
	while (count < hunk->leading && count < shortest) {
		compare = strcmp(old->items[count], new->items[count]);
		if (compare != 0)
			break;
		count++;
	}

	/* The leading context. */
	*leading = count;

	/* Equal lines at the end, not reaching into the leading ones. */
	count = 0;
	while (count < hunk->trailing && *leading + count < shortest) {
		compare = strcmp(old->items[old->count - 1 - count], new->items[new->count - 1 - count]);
		if (compare != 0)
			break;
		count++;
	}

	/* The trailing context. */
	*trailing = count;
}

/*
 * Looks for old lines [first, last) in the text around an expected
 * position, not before lowest.  Stores where they start.
 */
static int
find_hunk(
	const struct patch_options *options,
	const struct patch_lines *text,
	const struct patch_lines *old,
	long first,
	long last,
	long expected,
	long lowest,
	long *found)
{
	long length;
	long highest;
	long distance;
	long position;
	int match;

	/* The positions the lines can start at. */
	length = last - first;
	highest = text->count - length;
	if (highest < lowest)
		return -1;
	if (expected < lowest)
		expected = lowest;
	if (expected > highest)
		expected = highest;

	/* Tries the expected line, then further and further away. */
	for (distance = 0; ; distance++) {
		if (expected + distance > highest && expected - distance < lowest)
			break;
		position = expected + distance;
		if (position <= highest) {
			match = lines_match(options, text, position, old, first, last);
			if (match) {
				*found = position;
				return 0;
			}
		}

		/* Then the line as far before it. */
		position = expected - distance;
		if (distance > 0 && position >= lowest) {
			match = lines_match(options, text, position, old, first, last);
			if (match) {
				*found = position;
				return 0;
			}
		}
	}

	/* Not found. */
	return -1;
}

/* Tells whether old lines [first, last) are in the text at a position. */
static int
lines_match(
	const struct patch_options *options,
	const struct patch_lines *text,
	long position,
	const struct patch_lines *old,
	long first,
	long last)
{
	long index;
	int same;

	/* Compares line by line. */
	for (index = first; index < last; index++) {
		same = same_line(options, text->items[position + index - first], old->items[index]);
		if (!same)
			return 0;
	}

	/* All of them match. */
	return 1;
}

/* Compares two lines, with their blanks folded under -l. */
static int
same_line(
	const struct patch_options *options,
	const char *left,
	const char *right)
{
	int compare;

	/* Exactly, without -l. */
	if (!options->loose) {
		compare = strcmp(left, right);
		if (compare == 0)
			return 1;
		return 0;
	}

	/* With -l, any run of blanks matches any other, and leading and trailing ones are ignored. */
	while (*left == ' ' || *left == '\t')
		left++;
	while (*right == ' ' || *right == '\t')
		right++;
	while (*left != '\0' && *right != '\0') {
		if ((*left == ' ' || *left == '\t') && (*right == ' ' || *right == '\t')) {
			while (*left == ' ' || *left == '\t')
				left++;
			while (*right == ' ' || *right == '\t')
				right++;
			continue;
		}

		/* Other characters must be the same. */
		if (*left != *right)
			return 0;
		left++;
		right++;
	}
	while (*left == ' ' || *left == '\t')
		left++;
	while (*right == ' ' || *right == '\t')
		right++;

	/* Both must be used up. */
	if (*left == '\0' && *right == '\0')
		return 1;
	return 0;
}

/* Adds lines [first, last) of one array to another. */
static int
add_range(
	struct patch_lines *out,
	const struct patch_lines *from,
	long first,
	long last)
{
	long index;
	int status;

	/* Copies each line. */
	for (index = first; index < last; index++) {
		status = patch_lines_add(out, from->items[index], strlen(from->items[index]));
		if (status != 0)
			return -1;
	}

	/* Succeeded: the lines are added. */
	return 0;
}

/* Adds one line of text. */
static int
add_text(
	struct patch_lines *out,
	const char *text)
{
	int status;

	/* Copies the line. */
	status = patch_lines_add(out, text, strlen(text));
	if (status != 0)
		return -1;
	return 0;
}

/*
 * Adds the changed lines of a hunk for -D: the old lines, as the file has
 * them, under #ifndef and the new ones under #ifdef, or #else between
 * them when both are there.
 */
static int
add_defined(
	const struct patch_options *options,
	struct patch_lines *out,
	const struct patch_lines *text,
	long text_first,
	long text_last,
	const struct patch_lines *new,
	long new_first,
	long new_last)
{
	char line[512];
	int status;

	/* Old lines only: #ifndef; new only: #ifdef; both: #ifndef, #else. */
	if (text_last > text_first) {
		snprintf(line, sizeof(line), "#ifndef %s", options->define);
		status = add_text(out, line);
		if (status == 0)
			status = add_range(out, text, text_first, text_last);
		if (status == 0 && new_last > new_first)
			status = add_text(out, "#else");
	} else {
		snprintf(line, sizeof(line), "#ifdef %s", options->define);
		status = add_text(out, line);
	}

	/* Then the new lines and the end of the construct. */
	if (status == 0)
		status = add_range(out, new, new_first, new_last);
	if (status == 0)
		status = add_text(out, "#endif");
	if (status != 0)
		return -1;

	/* Succeeded: both versions are added. */
	return 0;
}

/*
 * Reads an ed address "N" or "N,M"; stores the first and the last line
 * and where the command letter is.
 */
static int
ed_address(
	const char *text,
	long *first,
	long *last,
	const char **end)
{
	char *after;
	long value;

	/* The first number. */
	value = strtol(text, &after, 10);
	*first = value;
	*last = value;

	/* An optional second number. */
	if (*after == ',') {
		text = after + 1;
		value = strtol(text, &after, 10);
		if (after == text)
			return -1;
		*last = value;
	}

	/* Succeeded: the addresses. */
	*end = after;
	return 0;
}
