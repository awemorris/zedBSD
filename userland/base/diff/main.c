/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Compares two files (POSIX XCU diff).
 *
 *	diff [-c|-e|-f|-u|-C n|-U n] [-br] file1 file2
 *
 * The lines of the files are compared through a longest common
 * subsequence (lines.c) and the lines that differ are written in the
 * normal form (NaM, NdM, NcM with < and > lines), as a context diff (-c,
 * -C n), as a unified diff (-u, -U n), as an ed script (-e), or as the
 * same commands in forward order (-f).  -b compares lines with their
 * blanks folded.  - names standard input.
 *
 * Directories are compared through tree.c: the files of the same name
 * are compared, "Only in" names the others, subdirectories are compared
 * with -r and reported as "Common subdirectories" otherwise, and each pair
 * of differing files inside is introduced by a "diff options file1 file2"
 * line.  A directory and a file compare the file with the file of the
 * same name in the directory.  Files that are not text are reported as
 * differing without their lines.
 *
 * zedBSD also accepts -q (report only whether files differ) and
 * --metadata (compare owners, modes, times and hard links too), which the
 * installer uses to check a copied tree.
 *
 * diff exits with 0 when the files are the same, 1 when they differ, and
 * 2 on trouble.
 */

#include "userland/base/common/command.h"
#include "userland/base/diff/lines.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The output forms. */
#define DIFF_NORMAL 0
#define DIFF_CONTEXT 1
#define DIFF_UNIFIED 2
#define DIFF_ED 3
#define DIFF_FORWARD 4

/* The context lines of -c and -u. */
#define DIFF_DEFAULT_CONTEXT 3

/*
 * What the command line asks for.
 *
 * One instance lives for the run.  option_text holds the options as they
 * were given, for the line that introduces each pair inside directories.
 */
struct diff_options {
	int format;
	long context;
	int fold_blanks;
	int recursive;
	int brief;
	int metadata;
	int in_directories;
	char option_text[256];
};

/*
 * The run's options, for the text comparison that tree.c calls back.
 *
 * It is set once in main() before any comparison and read only after.
 */
static struct diff_options diff_run;

extern int diff_tree(const char *left, const char *right, int recursive, int metadata, int brief, int (*text)(const char *, const char *, const char *, const char *));

static int read_options(int argc, char **argv, struct diff_options *options);
static int read_context(const char *text, long *context);
static void add_option_text(struct diff_options *options, const char *text);
static int compare_standard_input(const char *left, const char *right);
static int text_diff(const char *left, const char *right, const char *left_label, const char *right_label);
static int compare_loaded(struct diff_file *first, struct diff_file *second, const char *left_path, const char *right_path);
static void write_normal(const struct diff_file *first, const struct diff_file *second, const struct diff_change *changes, long count);
static void write_hunks(const struct diff_file *first, const struct diff_file *second, const struct diff_change *changes, long count, int unified);
static void write_context_hunk(const struct diff_file *first, const struct diff_file *second, const struct diff_change *changes, long first_change, long last_change, long a_from, long a_to, long b_from, long b_to);
static void write_unified_hunk(const struct diff_file *first, const struct diff_file *second, const struct diff_change *changes, long first_change, long last_change, long a_from, long a_to, long b_from, long b_to);
static void write_ed(const struct diff_file *second, const struct diff_change *changes, long count, int forward);
static void write_ed_text(const struct diff_file *second, long start, long end);
static const char *copy_leaf_name(const char *path, char *buffer, size_t size);
static void write_header(const char *mark, const char *label, const char *path, int unified);
static void write_range(long start, long end, const char *separator);
static void write_unified_range(long start, long end);
static void write_line(const char *prefix, const struct diff_file *file, long line);
static void usage(void);

/*
 * Runs diff.
 */
int
main(
	int argc,
	char **argv)
{
	char joined[PATH_MAX + 1];
	char leaf_buffer[PATH_MAX + 1];
	struct stat left_status;
	struct stat right_status;
	const char *left;
	const char *right;
	const char *leaf;
	int first;
	int status;
	int left_found;
	int right_found;
	int left_directory;
	int right_directory;
	int compare;

	/* Reads the options; exactly two operands follow. */
	first = read_options(argc, argv, &diff_run);
	if (argc - first != 2)
		usage();
	left = argv[first];
	right = argv[first + 1];

	/* - is standard input, compared as text. */
	compare = strcmp(left, "-");
	if (compare != 0)
		compare = strcmp(right, "-");
	if (compare == 0) {
		status = compare_standard_input(left, right);
		fflush(stdout);
		return status;
	}

	/* A directory and a file: the file of the same name in the directory. */
	left_found = stat(left, &left_status);
	right_found = stat(right, &right_status);
	left_directory = 0;
	right_directory = 0;
	if (left_found == 0)
		left_directory = S_ISDIR(left_status.st_mode);
	if (right_found == 0)
		right_directory = S_ISDIR(right_status.st_mode);
	if (left_directory && !right_directory && right_found == 0) {
		leaf = copy_leaf_name(right, leaf_buffer, sizeof(leaf_buffer));
		snprintf(joined, sizeof(joined), "%s/%s", left, leaf);
		left = joined;
	} else if (right_directory && !left_directory && left_found == 0) {
		leaf = copy_leaf_name(left, leaf_buffer, sizeof(leaf_buffer));
		snprintf(joined, sizeof(joined), "%s/%s", right, leaf);
		right = joined;
	}

	/* Pairs inside two directories are introduced by a diff line. */
	if (left_directory && right_directory)
		diff_run.in_directories = 1;

	/* Compares the operands. */
	status = diff_tree(left, right, diff_run.recursive, diff_run.metadata, diff_run.brief, text_diff);

	/* A failed write is trouble too. */
	fflush(stdout);
	compare = ferror(stdout);
	if (compare != 0) {
		fprintf(stderr, "diff: write error\n");
		return 2;
	}

	/* Reports 0 for the same, 1 for different, 2 for trouble. */
	if (status >= 2)
		return 2;
	return status;
}

/*
 * Reads the options and returns the index of the first operand.  -C and
 * -U take a number attached or as the next argument.
 */
static int
read_options(
	int argc,
	char **argv,
	struct diff_options *options)
{
	const char *letter;
	const char *value;
	int index;
	int compare;
	int status;

	/* Normal output without options. */
	memset(options, 0, sizeof(*options));
	options->format = DIFF_NORMAL;
	options->context = DIFF_DEFAULT_CONTEXT;

	/* Reads arguments until the first operand; - alone is an operand. */
	for (index = 1; index < argc; index++) {
		if (argv[index][0] != '-' || argv[index][1] == '\0')
			break;

		/* -- ends the options. */
		compare = strcmp(argv[index], "--");
		if (compare == 0) {
			index++;
			break;
		}

		/* --metadata compares attributes too. */
		compare = strcmp(argv[index], "--metadata");
		if (compare == 0) {
			options->metadata = 1;
			add_option_text(options, argv[index]);
			continue;
		}

		/* The letters of the argument. */
		add_option_text(options, argv[index]);
		for (letter = argv[index] + 1; *letter != '\0'; letter++) {
			/* -C and -U take the rest of the argument or the next one. */
			if (*letter == 'C' || *letter == 'U') {
				value = letter + 1;
				if (*value == '\0') {
					if (index + 1 >= argc)
						usage();
					index++;
					value = argv[index];
					add_option_text(options, value);
				}

				/* Reads the context length. */
				status = read_context(value, &options->context);
				if (status != 0) {
					fprintf(stderr, "diff: invalid context length: '%s'\n", value);
					exit(2);
				}

				/* -C gives context and -U unified output. */
				options->format = DIFF_CONTEXT;
				if (*letter == 'U')
					options->format = DIFF_UNIFIED;
				break;
			}

			/* The other letters. */
			switch (*letter) {
			case 'b':
				options->fold_blanks = 1;
				break;
			case 'c':
				options->format = DIFF_CONTEXT;
				options->context = DIFF_DEFAULT_CONTEXT;
				break;
			case 'u':
				options->format = DIFF_UNIFIED;
				options->context = DIFF_DEFAULT_CONTEXT;
				break;
			case 'e':
				options->format = DIFF_ED;
				break;
			case 'f':
				options->format = DIFF_FORWARD;
				break;
			case 'r':
				options->recursive = 1;
				break;
			case 'q':
				options->brief = 1;
				break;
			default:
				fprintf(stderr, "diff: unknown option -%c\n", *letter);
				usage();
				break;
			}
		}
	}

	/* Reports where the operands start. */
	return index;
}

/* Parses a context length that is not negative. */
static int
read_context(
	const char *text,
	long *context)
{
	char *end;
	long value;

	/* Converts the whole text. */
	errno = 0;
	value = strtol(text, &end, 10);
	if (end == text || *end != '\0' || value < 0 || errno != 0)
		return -1;

	/* Succeeded: the length. */
	*context = value;
	return 0;
}

/* Appends one option argument to the text that repeats the options. */
static void
add_option_text(
	struct diff_options *options,
	const char *text)
{
	size_t used;
	size_t length;

	/* Separates the options by spaces, as they were given. */
	used = strlen(options->option_text);
	length = strlen(text);
	if (used + length + 2 >= sizeof(options->option_text))
		return;
	if (used > 0) {
		options->option_text[used] = ' ';
		used++;
	}

	/* Appends the option. */
	memcpy(options->option_text + used, text, length + 1);
}

/*
 * Gives the last component of a pathname, trailing slashes ignored, in a
 * buffer.
 */
static const char *
copy_leaf_name(
	const char *path,
	char *buffer,
	size_t size)
{
	size_t end;
	size_t start;
	size_t length;

	/* Drops trailing slashes and finds the start of the component. */
	end = strlen(path);
	while (end > 1 && path[end - 1] == '/')
		end--;
	start = end;
	while (start > 0 && path[start - 1] != '/')
		start--;

	/* Copies it. */
	length = end - start;
	if (length >= size)
		length = size - 1;
	memcpy(buffer, path + start, length);
	buffer[length] = '\0';
	return buffer;
}

/*
 * Compares operands when one of them is standard input: both are read as
 * text, and - is named "-".
 */
static int
compare_standard_input(
	const char *left,
	const char *right)
{
	struct diff_file first;
	struct diff_file second;
	const char *left_path;
	const char *right_path;
	int status;
	int compare;

	/* Standard input is read through its descriptor. */
	left_path = left;
	compare = strcmp(left, "-");
	if (compare == 0)
		left_path = "/dev/stdin";
	right_path = right;
	compare = strcmp(right, "-");
	if (compare == 0)
		right_path = "/dev/stdin";

	/* Loads both. */
	status = diff_load(left_path, left, &first);
	if (status != 0) {
		fprintf(stderr, "diff: %s: %s\n", left, strerror(errno));
		return 2;
	}

	/* Loads the second. */
	status = diff_load(right_path, right, &second);
	if (status != 0) {
		fprintf(stderr, "diff: %s: %s\n", right, strerror(errno));
		diff_release(&first);
		return 2;
	}

	/* Compares them. */
	status = compare_loaded(&first, &second, left_path, right_path);
	diff_release(&first);
	diff_release(&second);
	return status;
}

/*
 * Compares two regular files as text; tree.c calls this for a pair whose
 * bytes differ.  Returns 0 when they are the same under -b, 1 when they
 * differ, 2 on trouble.
 */
static int
text_diff(
	const char *left,
	const char *right,
	const char *left_label,
	const char *right_label)
{
	struct diff_file first;
	struct diff_file second;
	int status;

	/* Loads both files. */
	status = diff_load(left, left_label, &first);
	if (status != 0) {
		fprintf(stderr, "diff: %s: %s\n", left_label, strerror(errno));
		return 2;
	}

	/* Loads the second. */
	status = diff_load(right, right_label, &second);
	if (status != 0) {
		fprintf(stderr, "diff: %s: %s\n", right_label, strerror(errno));
		diff_release(&first);
		return 2;
	}

	/* Compares them. */
	status = compare_loaded(&first, &second, left, right);
	diff_release(&first);
	diff_release(&second);
	return status;
}

/*
 * Compares two loaded files and writes their differences in the chosen
 * form.  The paths date the headers of -c and -u.
 */
static int
compare_loaded(
	struct diff_file *first,
	struct diff_file *second,
	const char *left_path,
	const char *right_path)
{
	struct diff_change *changes;
	long count;
	int status;
	int unified;

	/* Numbers the lines and finds the changes. */
	status = diff_number_lines(first, second, diff_run.fold_blanks);
	if (status != 0) {
		fprintf(stderr, "diff: %s\n", strerror(errno));
		return 2;
	}

	/* Finds the changes. */
	status = diff_compare(first, second, &changes, &count);
	if (status != 0) {
		fprintf(stderr, "diff: %s\n", strerror(errno));
		return 2;
	}

	/* The same lines: nothing to write. */
	if (count == 0) {
		free(changes);
		return 0;
	}

	/* Inside directories, a line introduces the pair. */
	if (diff_run.in_directories) {
		if (diff_run.option_text[0] != '\0')
			printf("diff %s %s %s\n", diff_run.option_text, first->label, second->label);
		else
			printf("diff %s %s\n", first->label, second->label);
	}

	/* Writes the changes in the chosen form. */
	switch (diff_run.format) {
	case DIFF_CONTEXT:
	case DIFF_UNIFIED:
		unified = 0;
		if (diff_run.format == DIFF_UNIFIED)
			unified = 1;
		if (unified) {
			write_header("---", first->label, left_path, 1);
			write_header("+++", second->label, right_path, 1);
		} else {
			write_header("***", first->label, left_path, 0);
			write_header("---", second->label, right_path, 0);
		}

		/* Writes the hunks. */
		write_hunks(first, second, changes, count, unified);
		break;
	case DIFF_ED:
		write_ed(second, changes, count, 0);
		break;
	case DIFF_FORWARD:
		write_ed(second, changes, count, 1);
		break;
	default:
		write_normal(first, second, changes, count);
		break;
	}

	/* Succeeded: the files differ and the differences were written. */
	free(changes);
	return 1;
}

/* Writes the changes in the normal form. */
static void
write_normal(
	const struct diff_file *first,
	const struct diff_file *second,
	const struct diff_change *changes,
	long count)
{
	const struct diff_change *change;
	long index;
	long line;

	/* One command and its lines for each change. */
	for (index = 0; index < count; index++) {
		change = &changes[index];

		/* The command: a, d or c between the two ranges. */
		if (change->a_start == change->a_end) {
			printf("%ld", change->a_start);
			write_range(change->b_start, change->b_end, "a");
		} else if (change->b_start == change->b_end) {
			write_range(change->a_start, change->a_end, "");
			printf("d%ld\n", change->b_start);
		} else {
			write_range(change->a_start, change->a_end, "");
			write_range(change->b_start, change->b_end, "c");
		}

		/* The lines taken away, the separator, and the lines added. */
		for (line = change->a_start; line < change->a_end; line++)
			write_line("< ", first, line);
		if (change->a_start != change->a_end && change->b_start != change->b_end)
			printf("---\n");
		for (line = change->b_start; line < change->b_end; line++)
			write_line("> ", second, line);
	}
}

/*
 * Writes a range of the normal form: the first line, or the first and the
 * last separated by a comma, after a command letter when one is given; a
 * letter ends the command line.
 */
static void
write_range(
	long start,
	long end,
	const char *letter)
{
	/* The letter comes before the range of the second file. */
	if (letter[0] != '\0')
		printf("%s", letter);

	/* One line or several. */
	if (end - start == 1)
		printf("%ld", start + 1);
	else
		printf("%ld,%ld", start + 1, end);

	/* A letter ends the line. */
	if (letter[0] != '\0')
		putchar('\n');
}

/*
 * Groups the changes into hunks whose context overlaps and writes each as
 * a context or unified hunk.
 */
static void
write_hunks(
	const struct diff_file *first,
	const struct diff_file *second,
	const struct diff_change *changes,
	long count,
	int unified)
{
	long first_change;
	long last_change;
	long a_from;
	long a_to;
	long b_from;
	long b_to;
	long context;

	/* Takes the changes hunk by hunk. */
	context = diff_run.context;
	first_change = 0;
	while (first_change < count) {
		/* Joins the next changes whose context meets. */
		last_change = first_change;
		while (last_change + 1 < count && changes[last_change + 1].a_start - changes[last_change].a_end <= 2 * context)
			last_change++;

		/* The hunk covers the changes and their context. */
		a_from = changes[first_change].a_start - context;
		if (a_from < 0)
			a_from = 0;
		a_to = changes[last_change].a_end + context;
		if (a_to > first->count)
			a_to = first->count;
		b_from = changes[first_change].b_start - (changes[first_change].a_start - a_from);
		b_to = changes[last_change].b_end + (a_to - changes[last_change].a_end);
		if (b_from < 0)
			b_from = 0;
		if (b_to > second->count)
			b_to = second->count;

		/* Writes the hunk. */
		if (unified)
			write_unified_hunk(first, second, changes, first_change, last_change, a_from, a_to, b_from, b_to);
		else
			write_context_hunk(first, second, changes, first_change, last_change, a_from, a_to, b_from, b_to);
		first_change = last_change + 1;
	}
}

/* Writes one unified hunk: its ranges, then context, - and + lines. */
static void
write_unified_hunk(
	const struct diff_file *first,
	const struct diff_file *second,
	const struct diff_change *changes,
	long first_change,
	long last_change,
	long a_from,
	long a_to,
	long b_from,
	long b_to)
{
	const struct diff_change *change;
	long index;
	long line;
	long a_line;

	/* The ranges. */
	printf("@@ -");
	write_unified_range(a_from, a_to);
	printf(" +");
	write_unified_range(b_from, b_to);
	printf(" @@\n");

	/* Each change after the context before it. */
	a_line = a_from;
	for (index = first_change; index <= last_change; index++) {
		change = &changes[index];
		for (; a_line < change->a_start; a_line++)
			write_line(" ", first, a_line);
		for (line = change->a_start; line < change->a_end; line++)
			write_line("-", first, line);
		for (line = change->b_start; line < change->b_end; line++)
			write_line("+", second, line);
		a_line = change->a_end;
	}

	/* The context after the last change. */
	for (; a_line < a_to; a_line++)
		write_line(" ", first, a_line);
}

/*
 * Writes a unified range: the first line and the count, the count left out
 * when it is 1; an empty range starts at the line before it.
 */
static void
write_unified_range(
	long start,
	long end)
{
	/* One line: its number alone. */
	if (end - start == 1) {
		printf("%ld", start + 1);
		return;
	}

	/* No line: the line before, and 0. */
	if (end == start) {
		printf("%ld,0", start);
		return;
	}

	/* Several lines: the first and the count. */
	printf("%ld,%ld", start + 1, end - start);
}

/*
 * Writes one context hunk: the first file's range and lines, then the
 * second file's.  A side whose lines do not change in the hunk is written
 * as its range alone.
 */
static void
write_context_hunk(
	const struct diff_file *first,
	const struct diff_file *second,
	const struct diff_change *changes,
	long first_change,
	long last_change,
	long a_from,
	long a_to,
	long b_from,
	long b_to)
{
	const struct diff_change *change;
	const char *mark;
	long index;
	long line;
	long a_line;
	long b_line;
	int a_changes;
	int b_changes;

	/* Learns whether either side changes in the hunk. */
	a_changes = 0;
	b_changes = 0;
	for (index = first_change; index <= last_change; index++) {
		if (changes[index].a_start != changes[index].a_end)
			a_changes = 1;
		if (changes[index].b_start != changes[index].b_end)
			b_changes = 1;
	}

	/* The first file's range and, if it changes, its lines. */
	printf("***************\n*** ");
	if (a_to - a_from == 0)
		printf("%ld", a_from);
	else if (a_to - a_from == 1)
		printf("%ld", a_from + 1);
	else
		printf("%ld,%ld", a_from + 1, a_to);
	printf(" ****\n");
	if (a_changes) {
		a_line = a_from;
		for (index = first_change; index <= last_change; index++) {
			change = &changes[index];
			for (; a_line < change->a_start; a_line++)
				write_line("  ", first, a_line);
			mark = "- ";
			if (change->b_start != change->b_end)
				mark = "! ";
			for (line = change->a_start; line < change->a_end; line++)
				write_line(mark, first, line);
			a_line = change->a_end;
		}

		/* The context after the last change. */
		for (; a_line < a_to; a_line++)
			write_line("  ", first, a_line);
	}

	/* The second file's range and, if it changes, its lines. */
	printf("--- ");
	if (b_to - b_from == 0)
		printf("%ld", b_from);
	else if (b_to - b_from == 1)
		printf("%ld", b_from + 1);
	else
		printf("%ld,%ld", b_from + 1, b_to);
	printf(" ----\n");
	if (b_changes) {
		b_line = b_from;
		for (index = first_change; index <= last_change; index++) {
			change = &changes[index];
			for (; b_line < change->b_start; b_line++)
				write_line("  ", second, b_line);
			mark = "+ ";
			if (change->a_start != change->a_end)
				mark = "! ";
			for (line = change->b_start; line < change->b_end; line++)
				write_line(mark, second, line);
			b_line = change->b_end;
		}

		/* The context after the last change. */
		for (; b_line < b_to; b_line++)
			write_line("  ", second, b_line);
	}
}

/*
 * Writes an ed script: the changes last to first so that line numbers
 * stay valid (-e), or first to last with the letter before the numbers
 * (-f).
 */
static void
write_ed(
	const struct diff_file *second,
	const struct diff_change *changes,
	long count,
	int forward)
{
	const struct diff_change *change;
	long step;
	long index;
	long a_first;
	long a_last;
	int letter;

	/* The order of the changes. */
	index = count - 1;
	step = -1;
	if (forward) {
		index = 0;
		step = 1;
	}

	/* One command for each change. */
	for (; index >= 0 && index < count; index += step) {
		change = &changes[index];

		/* The command letter. */
		if (change->a_start == change->a_end)
			letter = 'a';
		else if (change->b_start == change->b_end)
			letter = 'd';
		else
			letter = 'c';

		/* The lines of the first file it applies to. */
		a_first = change->a_start + 1;
		a_last = change->a_end;
		if (letter == 'a') {
			a_first = change->a_start;
			a_last = change->a_start;
		}

		/* The command line: numbers then letter, or letter then numbers. */
		if (forward) {
			if (a_first == a_last)
				printf("%c%ld\n", letter, a_first);
			else
				printf("%c%ld %ld\n", letter, a_first, a_last);
		} else {
			if (a_first == a_last)
				printf("%ld%c\n", a_first, letter);
			else
				printf("%ld,%ld%c\n", a_first, a_last, letter);
		}

		/* The text to add, ended by a dot. */
		if (letter != 'd')
			write_ed_text(second, change->b_start, change->b_end);
	}
}

/*
 * Writes the text of an a or c command and the dot that ends it.  A line
 * that is a dot alone is written as two dots and then fixed with a
 * substitution, so that it does not end the text early.
 */
static void
write_ed_text(
	const struct diff_file *second,
	long start,
	long end)
{
	const char *text;
	size_t length;
	long line;

	/* Writes each line. */
	for (line = start; line < end; line++) {
		length = diff_line_length(second, line);
		text = second->data + second->starts[line];

		/* A lone dot: two dots, end the text, fix it, and go on adding. */
		if (length == 1 && text[0] == '.') {
			printf("..\n.\ns/.//\n");
			if (line + 1 < end)
				printf("a\n");
			continue;
		}

		/* Any other line as it is. */
		fwrite(text, 1, length, stdout);
		putchar('\n');
	}

	/* The dot that ends the text, unless a lone dot already ended it. */
	length = 0;
	if (end > start)
		length = diff_line_length(second, end - 1);
	if (end > start && length == 1 && second->data[second->starts[end - 1]] == '.')
		return;
	printf(".\n");
}

/*
 * Writes a header line of -c or -u: the mark, the name and the file's
 * modification time (date "+%a %b %e %T %Y" for -c; the date, the time
 * with nanoseconds and the zone for -u, after a tab).
 */
static void
write_header(
	const char *mark,
	const char *label,
	const char *path,
	int unified)
{
	struct stat status_of_file;
	struct tm *broken;
	char date[64];
	char zone[16];
	time_t when;
	long nanoseconds;
	int status;

	/* The modification time, or now for standard input. */
	when = time(NULL);
	nanoseconds = 0;
	status = stat(path, &status_of_file);
	if (status == 0) {
		when = status_of_file.st_mtime;
		nanoseconds = status_of_file.st_mtim.tv_nsec;
	}

	/* Breaks the time down in the local zone. */
	broken = localtime(&when);
	date[0] = '\0';
	zone[0] = '\0';

	/* -u: the ISO date, nanoseconds and zone. */
	if (unified) {
		if (broken != NULL) {
			strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", broken);
			strftime(zone, sizeof(zone), "%z", broken);
		}

		/* Writes the -u header. */
		printf("%s %s\t%s.%09ld %s\n", mark, label, date, nanoseconds, zone);
		return;
	}

	/* -c: the date as date writes it. */
	if (broken != NULL)
		strftime(date, sizeof(date), "%a %b %e %T %Y", broken);
	printf("%s %s %s\n", mark, label, date);
}

/*
 * Writes one line with a prefix.  A last line without a newline is
 * followed by the note that says so.
 */
static void
write_line(
	const char *prefix,
	const struct diff_file *file,
	long line)
{
	size_t length;

	/* The prefix, the line and its newline. */
	length = diff_line_length(file, line);
	fputs(prefix, stdout);
	fwrite(file->data + file->starts[line], 1, length, stdout);
	putchar('\n');

	/* The last line of a file that does not end in a newline. */
	if (file->missing_newline && line == file->count - 1)
		printf("\\ No newline at end of file\n");
}

/* Writes the usage message and exits with status 2. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: diff [-c|-e|-f|-u|-C n|-U n] [-br] file1 file2\n");
	exit(2);
}
