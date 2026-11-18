/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Searches one input for grep: reads its lines, decides which are selected,
 * and writes the lines, their matches (-o), the context around them (-A,
 * -B, -C), their count or the input's name.
 *
 * The input is read straight from its descriptor into a buffer that grows
 * to hold the longest line, so that a pipe is searched as its data comes.
 * An input that holds a NUL byte is binary: unless -a, its matching lines
 * are not written, and a message says the file matches instead (GNU grep
 * does the same; the message goes to standard error).
 *
 * A match of a pattern is found with the regex matcher (leftmost-longest)
 * or, with -F, by comparing bytes.  -x wants the match to be the whole
 * line.  -w wants it to be a whole word: when the leftmost match is not
 * one, a shorter match from the same place is tried, then a match further
 * on, as GNU grep does.
 */

#include "userland/base/grep/grep.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* How much is read from the input at a time. */
#define READ_SIZE 32768U

/* The separators of a line prefix: a selected line, and a context line. */
#define SEPARATOR_SELECTED	':'
#define SEPARATOR_CONTEXT	'-'

/*
 * The input being read.
 *
 * data[start] to data[end] is what has been read but not yet taken as a
 * line; offset is where data[start] is in the whole input, for -b.
 */
struct input {
	int descriptor;
	char *data;
	size_t start;
	size_t end;
	size_t capacity;
	unsigned long long offset;

	/* Set at the end of the input, and by a failed read (with errno). */
	int at_end;
	int error;

	/*
	 * The byte that ends a line.  A NUL byte read makes the input binary,
	 * unless it is the separator (-z).
	 */
	int separator;
	int binary;
};

/*
 * A line: its bytes with a NUL after them, its number and where it starts
 * in the input.  text_length is how much of it a regex sees, which stops at
 * a NUL byte inside the line.
 */
struct line {
	char *data;
	size_t length;
	size_t capacity;
	size_t text_length;
	unsigned long number;
	unsigned long long offset;
};

/*
 * The search of one input.
 *
 * The ring holds the lines before the next selected one that -B may yet
 * write: count of them, from first, in a ring of options->before lines.
 */
struct search {
	const struct grep_options *options;
	const char *name;
	int show_names;
	struct input input;
	struct line line;

	struct line *ring;
	size_t ring_first;
	size_t ring_count;

	/* The number of the last line written (0 for none), and -A's count. */
	unsigned long last_written;
	unsigned long after_left;

	/* The selected lines so far. */
	unsigned long count;
};

/*
 * Whether anything has been written by an earlier search.
 *
 * A group of context lines is separated from what went before even when
 * that was in another file, so the fact outlives one search.  It is set by
 * the first line written and never cleared.
 */
static int written_before;

static int run_search(struct search *search);
static int trailing_context(struct search *search);
static void finish_search(struct search *search);
static int line_selected(struct search *search);
static int find_match(const struct grep_options *options, const struct line *line, size_t start, size_t *match_start, size_t *match_end);
static int pattern_find(const struct grep_options *options, const struct grep_pattern *pattern, const struct line *line, size_t start, size_t *match_start, size_t *match_end);
static int whole_line_find(const struct grep_options *options, const struct grep_pattern *pattern, const struct line *line, size_t start, size_t *match_start, size_t *match_end);
static int whole_word_find(const struct grep_options *options, const struct grep_pattern *pattern, const struct line *line, size_t start, size_t *match_start, size_t *match_end);
static int plain_find(const struct grep_options *options, const struct grep_pattern *pattern, const struct line *line, size_t start, size_t limit, size_t *match_start, size_t *match_end);
static int regex_find(const struct grep_pattern *pattern, const struct line *line, size_t start, size_t limit, size_t *match_start, size_t *match_end);
static int fixed_find(const struct grep_options *options, const struct grep_pattern *pattern, const struct line *line, size_t start, size_t *match_start, size_t *match_end);
static int word_bounded(const struct line *line, size_t match_start, size_t match_end);
static int is_word_byte(int value);
static int same_text(const char *left, const char *right, size_t length, int ignore_case);
static int lower(int value);
static void write_selected(struct search *search);
static void write_matches(struct search *search);
static void write_line(struct search *search, const struct line *line, int separator);
static void write_prefix(struct search *search, unsigned long number, unsigned long long offset, int separator);
static void write_group_separator(struct search *search, unsigned long number);
static void remember_line(struct search *search);
static void write_remembered(struct search *search);
static void copy_line(struct line *to, const struct line *from);
static int read_line(struct input *input, struct line *line, int separator);
static int fill_input(struct input *input);

/*
 * Searches one input, writing what the options ask for.
 *
 * Returns 1 when a line was selected, 0 when none was, and -1 after a
 * message (unless -s) when the input could not be read.
 */
int
grep_search(
	const struct grep_options *options,
	int descriptor,
	const char *name,
	int show_names)
{
	struct search search;
	unsigned long index;
	int result;

	/* The search, with nothing read yet. */
	memset(&search, 0, sizeof(search));
	search.options = options;
	search.name = name;
	search.show_names = show_names;
	search.input.descriptor = descriptor;
	search.input.separator = '\n';
	if (options->null_data)
		search.input.separator = '\0';

	/* The ring for -B. */
	if (options->before > 0) {
		search.ring = grep_allocate(NULL,
		    options->before * sizeof(*search.ring));
		memset(search.ring, 0, options->before * sizeof(*search.ring));
	}

	/* The lines, then the count or the name. */
	result = run_search(&search);
	if (result >= 0)
		finish_search(&search);

	/* The buffers go. */
	free(search.input.data);
	free(search.line.data);
	for (index = 0; search.ring != NULL && index < options->before; index++)
		free(search.ring[index].data);
	free(search.ring);

	/* An input that could not be read. */
	if (result < 0)
		return -1;

	/* No line selected. */
	if (search.count == 0)
		return 0;

	/* Succeeded: a line was selected. */
	return 1;
}

/*
 * Reads the lines and writes the selected ones and their context.  Returns
 * -1 after a message when the input could not be read, and 0 otherwise.
 */
static int
run_search(
	struct search *search)
{
	const struct grep_options *options;
	int separator;
	int selected;
	int read;

	/* Lines end with a newline, or with a NUL for -z. */
	options = search->options;
	separator = '\n';
	if (options->null_data)
		separator = '\0';

	/* Each line, until -m has its lines. */
	for (;;) {
		/* -m: enough lines are selected; only the context is left. */
		if (options->max_count >= 0 &&
		    search->count >= (unsigned long)options->max_count) {
			read = trailing_context(search);
			return read;
		}

		/* The next line. */
		read = read_line(&search->input, &search->line, separator);
		if (read < 0)
			break;
		if (read == 0)
			return 0;

		/* -I: a binary input is as if nothing in it matched. */
		if (search->input.binary &&
		    options->binary_files == GREP_BINARY_SKIP) {
			search->count = 0;
			return 0;
		}

		/* A line not selected is context, after or before a selected one. */
		selected = line_selected(search);
		if (!selected) {
			if (search->after_left > 0) {
				write_line(search, &search->line, SEPARATOR_CONTEXT);
				search->after_left--;
			} else {
				remember_line(search);
			}

			/* On to the next line. */
			continue;
		}

		/* A selected line: -q and -l need only the first. */
		search->count++;
		if (options->output == GREP_OUTPUT_QUIET)
			return 0;
		if (options->output == GREP_OUTPUT_NAMES)
			return 0;
		if (options->output == GREP_OUTPUT_UNMATCHED)
			return 0;
		if (options->output == GREP_OUTPUT_COUNT)
			continue;

		/* A binary input's lines are not written: a message instead. */
		if (search->input.binary &&
		    options->binary_files == GREP_BINARY_REPORT) {
			fflush(stdout);
			fprintf(stderr, "grep: %s: binary file matches\n",
				search->name);
			return 0;
		}

		/* The line, or its matches, after the context before it. */
		write_selected(search);
	}

	/* The input could not be read. */
	if (!options->no_messages) {
		fflush(stdout);
		fprintf(stderr, "grep: %s: %s\n", search->name,
			strerror(search->input.error));
	}

	/* The failure. */
	return -1;
}

/*
 * Writes the context after the last line -m allows: the lines up to the
 * count of -A, stopping at a line that would have been selected.  Returns
 * -1 when the input could not be read, and 0 otherwise.
 */
static int
trailing_context(
	struct search *search)
{
	int separator;
	int selected;
	int read;

	/* Lines end with a newline, or with a NUL for -z. */
	separator = '\n';
	if (search->options->null_data)
		separator = '\0';

	/* Each line of the context. */
	while (search->after_left > 0 &&
	       search->options->output == GREP_OUTPUT_LINES) {
		/* The next line, which ends the context when it is selected. */
		read = read_line(&search->input, &search->line, separator);
		if (read < 0)
			return -1;
		if (read == 0)
			break;
		selected = line_selected(search);
		if (selected)
			break;

		/* A line of context. */
		write_line(search, &search->line, SEPARATOR_CONTEXT);
		search->after_left--;
	}

	/* Succeeded. */
	return 0;
}

/* Writes the count (-c) or the name (-l, -L) of the input searched. */
static void
finish_search(
	struct search *search)
{
	const struct grep_options *options;
	int end;

	/* A name ends with a newline, or a NUL for -Z. */
	options = search->options;
	end = '\n';
	if (options->null_after_name)
		end = '\0';

	/* -c: the count, after the name when names are written. */
	if (options->output == GREP_OUTPUT_COUNT) {
		if (search->show_names) {
			fputs(search->name, stdout);
			if (options->null_after_name)
				putchar('\0');
			else
				putchar(':');
		}

		/* The count itself. */
		printf("%lu\n", search->count);
		return;
	}

	/* -l: the name of an input with a selected line. */
	if (options->output == GREP_OUTPUT_NAMES && search->count > 0) {
		fputs(search->name, stdout);
		putchar(end);
		return;
	}

	/* -L: the name of an input without one. */
	if (options->output == GREP_OUTPUT_UNMATCHED && search->count == 0) {
		fputs(search->name, stdout);
		putchar(end);
	}
}

/* Reports whether the current line is selected: matched, or not with -v. */
static int
line_selected(
	struct search *search)
{
	size_t match_start;
	size_t match_end;
	int matched;

	/* A match anywhere. */
	matched = find_match(search->options, &search->line, 0, &match_start,
			     &match_end);

	/* -v turns the answer round. */
	if (search->options->invert && matched)
		return 0;
	if (search->options->invert)
		return 1;

	/* Succeeded: whether a pattern matched. */
	return matched;
}

/*
 * Finds the leftmost match of any pattern that starts at or after start,
 * the longest of those that start there.  Returns 0 when there is none.
 */
static int
find_match(
	const struct grep_options *options,
	const struct line *line,
	size_t start,
	size_t *match_start,
	size_t *match_end)
{
	size_t candidate_start;
	size_t candidate_end;
	size_t index;
	int found;
	int any;

	/* Each pattern's first match; the leftmost, then longest, wins. */
	any = 0;
	for (index = 0; index < options->count; index++) {
		found = pattern_find(options, &options->patterns[index], line,
				     start, &candidate_start, &candidate_end);
		if (!found)
			continue;

		/* A match further left, or as far left and longer. */
		if (!any || candidate_start < *match_start ||
		    (candidate_start == *match_start &&
		     candidate_end > *match_end)) {
			*match_start = candidate_start;
			*match_end = candidate_end;
			any = 1;
		}
	}

	/* Succeeded: whether a pattern matched. */
	return any;
}

/*
 * Finds one pattern's first match at or after start, as -x or -w shape
 * it.  Returns 0 when there is none.
 */
static int
pattern_find(
	const struct grep_options *options,
	const struct grep_pattern *pattern,
	const struct line *line,
	size_t start,
	size_t *match_start,
	size_t *match_end)
{
	int found;

	/* -x: the whole line or nothing (it overrides -w). */
	if (options->whole_line) {
		found = whole_line_find(options, pattern, line, start,
					match_start, match_end);
		return found;
	}

	/* -w: a whole word. */
	if (options->whole_word) {
		found = whole_word_find(options, pattern, line, start,
					match_start, match_end);
		return found;
	}

	/* Succeeded: the first match, as it comes. */
	found = plain_find(options, pattern, line, start, line->length,
			   match_start, match_end);
	return found;
}

/* Finds a match that is the whole line, for -x. */
static int
whole_line_find(
	const struct grep_options *options,
	const struct grep_pattern *pattern,
	const struct line *line,
	size_t start,
	size_t *match_start,
	size_t *match_end)
{
	int found;

	/* Only the search from the start of the line can find the line. */
	if (start != 0)
		return 0;

	/* The leftmost-longest match must start and end with the line. */
	found = plain_find(options, pattern, line, 0, line->length,
			   match_start, match_end);
	if (!found)
		return 0;
	if (*match_start != 0 || *match_end != line->length)
		return 0;

	/* Succeeded: the whole line. */
	return 1;
}

/*
 * Finds a match that is a whole word, for -w: no word byte just before it
 * or just after it.  A match that is not one gives way to a shorter match
 * from the same place, then to the next match further on.
 */
static int
whole_word_find(
	const struct grep_options *options,
	const struct grep_pattern *pattern,
	const struct line *line,
	size_t start,
	size_t *match_start,
	size_t *match_end)
{
	size_t shorter_start;
	size_t shorter_end;
	size_t position;
	int bounded;
	int found;

	/* Each match further on, until one is a word. */
	position = start;
	while (position <= line->length) {
		/* The next match. */
		found = plain_find(options, pattern, line, position,
				   line->length, match_start, match_end);
		if (!found)
			return 0;

		/* The match, and each shorter match from where it starts. */
		for (;;) {
			bounded = word_bounded(line, *match_start, *match_end);
			if (bounded)
				return 1;

			/* A fixed string has no shorter match. */
			if (options->mode == GREP_MODE_FIXED || pattern->empty)
				break;
			if (*match_end == *match_start)
				break;

			/* The longest match from the same place, one byte shorter. */
			found = regex_find(pattern, line, *match_start,
					   *match_end - 1U, &shorter_start,
					   &shorter_end);
			if (!found)
				break;
			if (shorter_start != *match_start)
				break;
			if (shorter_end == shorter_start)
				break;
			*match_end = shorter_end;
		}

		/* The next match starts after this one's start. */
		position = *match_start + 1U;
	}

	/* No match is a word. */
	return 0;
}

/*
 * Finds a pattern's first match that starts at or after start and ends by
 * limit.  Returns 0 when there is none.
 */
static int
plain_find(
	const struct grep_options *options,
	const struct grep_pattern *pattern,
	const struct line *line,
	size_t start,
	size_t limit,
	size_t *match_start,
	size_t *match_end)
{
	int found;

	/* An empty pattern matches, emptily, where the search starts. */
	if (pattern->empty) {
		*match_start = start;
		*match_end = start;
		return 1;
	}

	/* A fixed string. */
	if (options->mode == GREP_MODE_FIXED) {
		found = fixed_find(options, pattern, line, start, match_start,
				   match_end);
		return found;
	}

	/* Succeeded: whether the regex matches. */
	found = regex_find(pattern, line, start, limit, match_start,
			   match_end);
	return found;
}

/*
 * Finds a regex's leftmost-longest match in the bytes from start to limit.
 * ^ matches at start only when it is the start of the line, and $ at limit
 * only when it is the end of the line.
 */
static int
regex_find(
	const struct grep_pattern *pattern,
	const struct line *line,
	size_t start,
	size_t limit,
	size_t *match_start,
	size_t *match_end)
{
	regmatch_t match;
	int flags;
	int result;

	/* The regex sees the line only up to a NUL byte in it. */
	if (limit > line->text_length)
		limit = line->text_length;
	if (start > limit)
		return 0;

	/* Whether the start is the start of the line. */
	flags = 0;
	if (start > 0)
		flags |= REG_NOTBOL;

	/*
	 * A part that runs to the end of the line is matched as the string
	 * from start, which the NUL after the line ends; only a part that ends
	 * early (a shorter match for -w) needs REG_STARTEND, which costs a copy
	 * of the part in this libc.
	 */
	if (limit == line->text_length) {
		result = regexec(&pattern->regex, line->data + start, 1, &match,
				 flags);
		if (result != 0)
			return 0;
		*match_start = start + (size_t)match.rm_so;
		*match_end = start + (size_t)match.rm_eo;
		return 1;
	}

	/* The part of the line, whose end is not the line's. */
	match.rm_so = (regoff_t)start;
	match.rm_eo = (regoff_t)limit;
	flags |= REG_STARTEND | REG_NOTEOL;
	result = regexec(&pattern->regex, line->data, 1, &match, flags);
	if (result != 0)
		return 0;

	/* Succeeded: where it is in the line. */
	*match_start = (size_t)match.rm_so;
	*match_end = (size_t)match.rm_eo;
	return 1;
}

/* Finds a fixed string (-F) at or after start. */
static int
fixed_find(
	const struct grep_options *options,
	const struct grep_pattern *pattern,
	const struct line *line,
	size_t start,
	size_t *match_start,
	size_t *match_end)
{
	size_t position;
	int same;

	/* A string longer than what is left is nowhere. */
	if (pattern->length > line->length)
		return 0;

	/* Each place it could start. */
	for (position = start; position + pattern->length <= line->length;
	     position++) {
		same = same_text(line->data + position, pattern->text,
				 pattern->length, options->ignore_case);
		if (same) {
			*match_start = position;
			*match_end = position + pattern->length;
			return 1;
		}
	}

	/* Not found. */
	return 0;
}

/* Reports whether no word byte touches a match on either side. */
static int
word_bounded(
	const struct line *line,
	size_t match_start,
	size_t match_end)
{
	int word;

	/* The byte before the match. */
	if (match_start > 0) {
		word = is_word_byte((unsigned char)line->data[match_start - 1U]);
		if (word)
			return 0;
	}

	/* The byte after it. */
	if (match_end < line->length) {
		word = is_word_byte((unsigned char)line->data[match_end]);
		if (word)
			return 0;
	}

	/* Succeeded: a whole word. */
	return 1;
}

/* Reports whether a byte is part of a word: a letter, a digit or _. */
static int
is_word_byte(
	int value)
{
	/* The ASCII letters and digits, and the underscore. */
	if (value >= 'a' && value <= 'z')
		return 1;
	if (value >= 'A' && value <= 'Z')
		return 1;
	if (value >= '0' && value <= '9')
		return 1;
	if (value == '_')
		return 1;

	/* Anything else ends a word. */
	return 0;
}

/* Compares length bytes, ignoring case when asked. */
static int
same_text(
	const char *left,
	const char *right,
	size_t length,
	int ignore_case)
{
	size_t index;
	int a;
	int b;

	/* Each byte in turn. */
	for (index = 0; index < length; index++) {
		/* Each byte, folded when case is ignored. */
		a = (unsigned char)left[index];
		b = (unsigned char)right[index];
		if (ignore_case) {
			a = lower(a);
			b = lower(b);
		}

		/* Bytes that differ. */
		if (a != b)
			return 0;
	}

	/* Succeeded: the same. */
	return 1;
}

/* Folds an ASCII letter to lower case. */
static int
lower(
	int value)
{
	/* A to Z. */
	if (value >= 'A' && value <= 'Z')
		return value - 'A' + 'a';
	return value;
}

/*
 * Writes a selected line: the context remembered before it, then the line
 * or (-o) its matches; -A's count starts again after it.
 */
static void
write_selected(
	struct search *search)
{
	/* The lines before it that -B keeps. */
	write_remembered(search);

	/* -o writes only the matches, and nothing for -v. */
	if (search->options->only_matching) {
		if (!search->options->invert)
			write_matches(search);
	} else {
		write_line(search, &search->line, SEPARATOR_SELECTED);
	}

	/* The lines after it that -A writes. */
	search->after_left = search->options->after;
}

/*
 * Writes each match of the line on a line of its own (-o).  Empty matches
 * are not written.
 */
static void
write_matches(
	struct search *search)
{
	const struct line *line;
	size_t match_start;
	size_t match_end;
	size_t position;
	int found;

	/* Each match, from the start of the line. */
	line = &search->line;
	position = 0;
	while (position <= line->length) {
		found = find_match(search->options, line, position, &match_start,
				   &match_end);
		if (!found)
			break;

		/* An empty match is passed over. */
		if (match_end == match_start) {
			position = match_start + 1U;
			continue;
		}

		/* The match, after the group separator the context asks for. */
		write_group_separator(search, line->number);
		write_prefix(search, line->number, line->offset + match_start,
			     SEPARATOR_SELECTED);
		fwrite(line->data + match_start, 1, match_end - match_start,
		       stdout);
		if (search->options->null_data)
			putchar('\0');
		else
			putchar('\n');
		search->last_written = line->number;
		written_before = 1;
		position = match_end;
	}
}

/*
 * Writes a line after its prefix; the separator in the prefix tells a
 * selected line from a context line.
 */
static void
write_line(
	struct search *search,
	const struct line *line,
	int separator)
{
	/* A separator between groups, the prefix and the line. */
	write_group_separator(search, line->number);
	write_prefix(search, line->number, line->offset, separator);
	fwrite(line->data, 1, line->length, stdout);

	/* The end of the line: a newline, or a NUL for -z. */
	if (search->options->null_data)
		putchar('\0');
	else
		putchar('\n');

	/* Succeeded: the line is the last one written. */
	search->last_written = line->number;
	written_before = 1;
}

/* Writes the prefix of a line: the name, the number and the offset asked. */
static void
write_prefix(
	struct search *search,
	unsigned long number,
	unsigned long long offset,
	int separator)
{
	const struct grep_options *options;

	/* The name, ended by a NUL with -Z. */
	options = search->options;
	if (search->show_names) {
		fputs(search->name, stdout);
		if (options->null_after_name)
			putchar('\0');
		else
			putchar(separator);
	}

	/* The line number (-n). */
	if (options->line_numbers)
		printf("%lu%c", number, separator);

	/* The byte offset (-b). */
	if (options->byte_offset)
		printf("%llu%c", offset, separator);
}

/*
 * Writes the group separator before a line that does not follow the last
 * line written, when context was asked for and something was written.
 */
static void
write_group_separator(
	struct search *search,
	unsigned long number)
{
	/* Only context groups are separated, and only when asked. */
	if (!search->options->context)
		return;
	if (search->options->group_separator == NULL)
		return;
	if (!written_before)
		return;

	/* A line right after the last one written is in the same group. */
	if (search->last_written != 0 && number <= search->last_written + 1UL)
		return;

	/* Succeeded: the separator. */
	puts(search->options->group_separator);
}

/* Keeps a line that -B may write before the next selected line. */
static void
remember_line(
	struct search *search)
{
	size_t size;
	size_t slot;

	/* Without -B, or when lines are not written, nothing is kept. */
	size = search->options->before;
	if (size == 0)
		return;
	if (search->options->output != GREP_OUTPUT_LINES)
		return;

	/* A full ring drops its oldest line. */
	if (search->ring_count == size) {
		search->ring_first = (search->ring_first + 1U) % size;
		search->ring_count--;
	}

	/* Succeeded: the line in the next slot. */
	slot = (search->ring_first + search->ring_count) % size;
	copy_line(&search->ring[slot], &search->line);
	search->ring_count++;
}

/* Writes the lines -B kept, as context, and empties the ring. */
static void
write_remembered(
	struct search *search)
{
	size_t size;
	size_t slot;

	/* Each kept line, oldest first. */
	size = search->options->before;
	while (search->ring_count > 0) {
		slot = search->ring_first;
		write_line(search, &search->ring[slot], SEPARATOR_CONTEXT);
		search->ring_first = (search->ring_first + 1U) % size;
		search->ring_count--;
	}
}

/* Copies a line, growing the copy's buffer as needed. */
static void
copy_line(
	struct line *to,
	const struct line *from)
{
	/* Room for the bytes and a NUL. */
	if (to->capacity < from->length + 1U) {
		to->capacity = from->length + 1U;
		to->data = grep_allocate(to->data, to->capacity);
	}

	/* The bytes, and what is known of the line. */
	memcpy(to->data, from->data, from->length + 1U);
	to->length = from->length;
	to->text_length = from->text_length;
	to->number = from->number;
	to->offset = from->offset;
}

/*
 * Reads the next line into line, without its separator.  A last line
 * without a separator is still a line.  Returns 1 for a line, 0 at the end
 * of the input and -1 when it could not be read.
 */
static int
read_line(
	struct input *input,
	struct line *line,
	int separator)
{
	char *found;
	size_t length;
	size_t taken;
	int filled;

	/* The bytes up to a separator, reading more until there is one. */
	for (;;) {
		found = memchr(input->data + input->start, separator,
			       input->end - input->start);
		if (found != NULL)
			break;
		if (input->at_end)
			break;
		filled = fill_input(input);
		if (filled < 0)
			return -1;
	}

	/* The end of the input. */
	length = input->end - input->start;
	if (found != NULL)
		length = (size_t)(found - (input->data + input->start));
	if (found == NULL && length == 0)
		return 0;

	/* The line, with a NUL after it. */
	if (line->capacity < length + 1U) {
		line->capacity = length + 1U;
		line->data = grep_allocate(line->data, line->capacity);
	}

	/* The bytes, and what is known of the line. */
	memcpy(line->data, input->data + input->start, length);
	line->data[length] = '\0';
	line->length = length;
	line->text_length = strlen(line->data);
	line->number++;
	line->offset = input->offset;

	/* Succeeded: past the line and its separator. */
	taken = length;
	if (found != NULL)
		taken++;
	input->start += taken;
	input->offset += taken;
	return 1;
}

/*
 * Reads more of the input into the buffer, after what is left of it.
 * Returns 0, with at_end set when nothing more came, or -1 with the error.
 */
static int
fill_input(
	struct input *input)
{
	ssize_t count;
	char *nul;

	/* What is left moves to the front. */
	if (input->start > 0) {
		memmove(input->data, input->data + input->start,
			input->end - input->start);
		input->end -= input->start;
		input->start = 0;
	}

	/* Room for a read. */
	if (input->capacity - input->end < READ_SIZE) {
		input->capacity = input->capacity * 2U + READ_SIZE;
		input->data = grep_allocate(input->data, input->capacity);
	}

	/* One read; a signal is not the end. */
	for (;;) {
		count = read(input->descriptor, input->data + input->end,
			     READ_SIZE);
		if (count >= 0)
			break;
		if (errno != EINTR) {
			input->error = errno;
			return -1;
		}
	}

	/* Nothing more: the end. */
	if (count == 0) {
		input->at_end = 1;
		return 0;
	}

	/* A NUL byte makes the input binary, unless NUL ends the lines. */
	nul = NULL;
	if (input->separator != '\0')
		nul = memchr(input->data + input->end, '\0', (size_t)count);
	if (nul != NULL)
		input->binary = 1;

	/* Succeeded: the bytes are in the buffer. */
	input->end += (size_t)count;
	return 0;
}
