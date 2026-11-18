/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Prints files (POSIX XCU pr).
 *
 *	pr [+page] [-column] [-adfFmprt] [-e[char][gap]] [-h header]
 *	   [-i[char][gap]] [-l lines] [-n[char][width]] [-o offset] [-s[char]]
 *	   [-w width] [file...]
 *
 * Each file (standard input for - or none) is cut into pages of 66
 * lines: a header of two empty lines, "date name Page n" and two empty
 * lines, the text, and five empty lines.  The date is the file's
 * modification time, or the current time for standard input and for -m,
 * in the form of date "+%b %e %H:%M %Y".  -t leaves the header and the
 * trailer out, as does a page length of ten lines or less; -F ends a page
 * with a form feed instead of the empty lines.  An empty file prints
 * nothing.
 *
 * -column sets the text in that many columns, filled down (across with
 * -a) and balanced on a last page that is not full; a line too long for
 * its column is cut.  -m sets the files side by side instead.  Columns
 * are separated by spaces within -w width (72), or by the -s character (a
 * tab) within 512.  Inside a column a tab moves to the next multiple of
 * eight from the column's start.
 *
 * -d doubles the spacing, -e expands input tabs (the -e character) to
 * multiples of the gap, -i replaces runs of spaces that reach a multiple
 * of the gap with the -i character, -n numbers the lines in width digits
 * followed by the -n character, -o indents every line, +page starts the
 * output at a page, -p and -f pause for a terminal, and -r keeps quiet
 * about files that cannot be opened.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The page length without -l. */
#define PR_DEFAULT_LENGTH 66

/* The lines of the header and of the trailer. */
#define PR_MARGIN 5

/* The line width of columns without -w, without and with -s. */
#define PR_DEFAULT_WIDTH 72
#define PR_SEPARATED_WIDTH 512

/* The tab distance of -e, -i and of text inside columns. */
#define PR_DEFAULT_GAP 8

/* The number width of -n without one. */
#define PR_DEFAULT_NUMBER_WIDTH 5

/* The size of the buffer one output line is built in. */
#define PR_LINE_SIZE 8192

/* The most columns, and the most text one column holds. */
#define PR_COLUMNS_MAX 256
#define PR_CELL_SIZE 1024

/*
 * What the command line asks for.
 *
 * One instance lives for the run.  The characters and numbers of -e, -i,
 * -n and -s hold their defaults until an option sets them.
 */
struct pr_options {
	long first_page;
	int columns;
	int across;
	int double_space;
	int expand;
	int expand_char;
	int expand_gap;
	int form_feed;
	int pause_first;
	const char *header;
	int tabify;
	int tabify_char;
	int tabify_gap;
	long length;
	int merge;
	int number;
	int number_char;
	int number_width;
	long offset;
	int pause;
	int quiet;
	int separate;
	int separator;
	int omit;
	long width;
	int width_given;
};

/*
 * One input file.
 *
 * line_number counts the lines read so far, for -n; done says that the
 * file has no more lines.
 */
struct pr_input {
	const char *name;
	FILE *stream;
	char date[64];
	long line_number;
	int done;
};

/*
 * One line of a page as read: the text without its newline and its line
 * number in its file.  The buffer is reused for the next page.
 */
struct pr_line {
	char *text;
	size_t capacity;
	long number;
};

/*
 * The pages being printed.
 *
 * One instance lives for the run.  lines holds the lines of the current
 * page.  rows is the number of text rows of a page, per_column the
 * number of input lines a column holds, and text_width the columns of
 * text a column of a multi-column page holds.  prefix_width is the room
 * of the row number of -m -n.
 */
struct pr_layout {
	const struct pr_options *options;
	struct pr_line *lines;
	long rows;
	long per_column;
	long text_width;
	long prefix_width;
	int columns;
	int margins;
	long page;
	FILE *terminal;
	int paused;
};

/*
 * The cells of the row being written, one for each column.
 *
 * A row is built here and written before the next one; nothing is kept
 * from row to row.
 */
static char pr_cells[PR_COLUMNS_MAX][PR_CELL_SIZE];

static int read_options(int argc, char **argv, struct pr_options *options);
static int read_option_cluster(const char *cluster, const char *next, struct pr_options *options, int *used_next);
static int read_argument_option(int letter, const char *value, struct pr_options *options);
static const char *read_char_and_number(const char *text, int *character, int *number, int fallback);
static int open_input(const struct pr_options *options, const char *name, struct pr_input *input);
static void format_date(time_t when, char *date, size_t size);
static int read_line(struct pr_input *input, struct pr_line *line);
static int setup_layout(struct pr_layout *layout, const struct pr_options *options);
static void set_text_width(struct pr_layout *layout, int columns);
static int print_file(struct pr_layout *layout, struct pr_input *input);
static int print_merged(struct pr_layout *layout, struct pr_input *inputs, int count);
static long print_page_lines(struct pr_layout *layout, size_t count);
static long print_column_rows(struct pr_layout *layout, size_t count);
static void begin_page(struct pr_layout *layout, const char *date, const char *name);
static void end_page(struct pr_layout *layout, long rows_written);
static size_t format_cell(const struct pr_layout *layout, const char *text, long number, int numbered, char *out, size_t size);
static void join_cells(const struct pr_layout *layout, char cells[][PR_CELL_SIZE], const int *present, int count, char *out, size_t size);
static long write_output_line(const struct pr_layout *layout, const char *text);
static void pause_for_terminal(struct pr_layout *layout);
static void usage(void);

/*
 * Runs pr.
 */
int
main(
	int argc,
	char **argv)
{
	static char *standard_input[] = { "-", NULL };
	struct pr_options options;
	struct pr_layout layout;
	struct pr_input *inputs;
	char **names;
	int first;
	int count;
	int index;
	int opened;
	int failed;
	int status;

	/* Reads the options; the files follow them, standard input if none. */
	first = read_options(argc, argv, &options);
	names = argv + first;
	count = argc - first;
	if (count == 0) {
		names = standard_input;
		count = 1;
	}

	/* Works out the page layout. */
	status = setup_layout(&layout, &options);
	if (status != 0)
		return 1;

	/* Room for the inputs. */
	inputs = calloc((size_t)count, sizeof(*inputs));
	if (inputs == NULL) {
		fprintf(stderr, "pr: out of memory\n");
		return 1;
	}

	/* Opens the files; one that cannot be opened is skipped. */
	failed = 0;
	opened = 0;
	for (index = 0; index < count; index++) {
		status = open_input(&options, names[index], &inputs[opened]);
		if (status != 0) {
			failed = 1;
			continue;
		}

		/* Counts the file as open. */
		opened++;
	}

	/* Prints the files merged, or one after the other. */
	if (options.merge) {
		if (opened > 0) {
			status = print_merged(&layout, inputs, opened);
			if (status != 0)
				failed = 1;
		}
	} else {
		for (index = 0; index < opened; index++) {
			status = print_file(&layout, &inputs[index]);
			if (status != 0)
				failed = 1;
		}
	}

	/* A failed write is an error too. */
	fflush(stdout);
	status = ferror(stdout);
	if (status != 0) {
		fprintf(stderr, "pr: write error\n");
		return 1;
	}

	/* Reports whether any file could not be printed. */
	if (failed)
		return 1;

	/* Succeeded: every file was printed. */
	return 0;
}

/*
 * Reads the options and returns the index of the first file operand.
 * Several options take an argument attached to them, so the arguments are
 * read by hand.
 */
static int
read_options(
	int argc,
	char **argv,
	struct pr_options *options)
{
	const char *next;
	char *end;
	int index;
	int used_next;
	int status;
	int compare;

	/* The defaults. */
	memset(options, 0, sizeof(*options));
	options->first_page = 1;
	options->columns = 1;
	options->expand_char = '\t';
	options->expand_gap = PR_DEFAULT_GAP;
	options->tabify_char = '\t';
	options->tabify_gap = PR_DEFAULT_GAP;
	options->length = PR_DEFAULT_LENGTH;
	options->number_char = '\t';
	options->number_width = PR_DEFAULT_NUMBER_WIDTH;
	options->separator = '\t';

	/* Reads arguments until the first file. */
	for (index = 1; index < argc; index++) {
		/* +page starts the output at a page. */
		if (argv[index][0] == '+') {
			errno = 0;
			options->first_page = strtol(argv[index] + 1, &end, 10);
			if (end == argv[index] + 1 || *end != '\0' || options->first_page <= 0 || errno != 0) {
				fprintf(stderr, "pr: invalid page: '%s'\n", argv[index]);
				exit(1);
			}

			/* The page is read. */
			continue;
		}

		/* - alone is a file, and so is anything not starting with -. */
		if (argv[index][0] != '-' || argv[index][1] == '\0')
			break;

		/* -- ends the options. */
		compare = strcmp(argv[index], "--");
		if (compare == 0) {
			index++;
			break;
		}

		/* A cluster of options, which may take the next argument. */
		next = NULL;
		if (index + 1 < argc)
			next = argv[index + 1];
		used_next = 0;
		status = read_option_cluster(argv[index] + 1, next, options, &used_next);
		if (status != 0)
			usage();

		/* Skips an argument an option took. */
		index += used_next;
	}

	/* Reports where the files start. */
	return index;
}

/*
 * Reads one argument of options.  -h, -l, -o and -w take the rest of the
 * argument or the next one; -e, -i, -n and -s take what is attached.
 */
static int
read_option_cluster(
	const char *cluster,
	const char *next,
	struct pr_options *options,
	int *used_next)
{
	const char *cursor;
	const char *value;
	char *end;
	long number;
	int letter;
	int status;

	/* Takes each letter in turn. */
	cursor = cluster;
	while (*cursor != '\0') {
		letter = (unsigned char)*cursor;
		cursor++;

		/* A number is the column count. */
		if (letter >= '0' && letter <= '9') {
			number = strtol(cursor - 1, &end, 10);
			if (number <= 0 || number > PR_COLUMNS_MAX)
				return -1;
			options->columns = (int)number;
			cursor = end;
			continue;
		}

		/* -h, -l, -o and -w take the rest of the argument or the next one. */
		if (letter == 'h' || letter == 'l' || letter == 'o' || letter == 'w') {
			value = cursor;
			if (*value == '\0') {
				if (next == NULL)
					return -1;
				value = next;
				*used_next = 1;
			}

			/* The argument ends the cluster. */
			status = read_argument_option(letter, value, options);
			return status;
		}

		/* The other letters: attached values, or flags. */
		switch (letter) {
		case 'e':
			options->expand = 1;
			cursor = read_char_and_number(cursor, &options->expand_char, &options->expand_gap, PR_DEFAULT_GAP);
			break;
		case 'i':
			options->tabify = 1;
			cursor = read_char_and_number(cursor, &options->tabify_char, &options->tabify_gap, PR_DEFAULT_GAP);
			break;
		case 'n':
			options->number = 1;
			cursor = read_char_and_number(cursor, &options->number_char, &options->number_width, PR_DEFAULT_NUMBER_WIDTH);
			break;
		case 's':
			/* -s takes the one character attached, a tab by default. */
			options->separate = 1;
			if (*cursor != '\0') {
				options->separator = (unsigned char)*cursor;
				cursor++;
			}

			/* The separator is read. */
			break;
		case 'a':
			options->across = 1;
			break;
		case 'd':
			options->double_space = 1;
			break;
		case 'f':
			options->form_feed = 1;
			options->pause_first = 1;
			break;
		case 'F':
			options->form_feed = 1;
			break;
		case 'm':
			options->merge = 1;
			break;
		case 'p':
			options->pause = 1;
			break;
		case 'r':
			options->quiet = 1;
			break;
		case 't':
			options->omit = 1;
			break;
		default:
			fprintf(stderr, "pr: unknown option -%c\n", letter);
			return -1;
		}
	}

	/* Succeeded: every letter was known. */
	return 0;
}

/* Reads the argument of -h, -l, -o or -w. */
static int
read_argument_option(
	int letter,
	const char *value,
	struct pr_options *options)
{
	char *end;
	long number;

	/* -h takes text. */
	if (letter == 'h') {
		options->header = value;
		return 0;
	}

	/* The others take a number that is not negative. */
	errno = 0;
	number = strtol(value, &end, 10);
	if (end == value || *end != '\0' || number < 0 || errno != 0)
		return -1;

	/* Keeps it; a width must be positive. */
	if (letter == 'l') {
		options->length = number;
	} else if (letter == 'o') {
		options->offset = number;
	} else {
		if (number == 0)
			return -1;
		options->width = number;
		options->width_given = 1;
	}

	/* Succeeded: the option was read. */
	return 0;
}

/*
 * Reads the optional character and number attached to -e, -i or -n: a
 * character that is not a digit, then digits.  No number, or zero, keeps
 * the fallback.  Returns where the reading stopped.
 */
static const char *
read_char_and_number(
	const char *text,
	int *character,
	int *number,
	int fallback)
{
	const char *cursor;
	long value;

	/* A leading non-digit is the character. */
	cursor = text;
	if (*cursor != '\0' && (*cursor < '0' || *cursor > '9')) {
		*character = (unsigned char)*cursor;
		cursor++;
	}

	/* Digits are the number, capped at a sane size. */
	value = 0;
	while (*cursor >= '0' && *cursor <= '9') {
		value = value * 10 + (*cursor - '0');
		if (value > 1000)
			value = 1000;
		cursor++;
	}

	/* No number, or zero, is the fallback. */
	*number = (int)value;
	if (value == 0)
		*number = fallback;
	return cursor;
}

/*
 * Opens one input and dates it: the modification time of a file, the
 * current time for standard input.  Reports a failure unless -r.
 */
static int
open_input(
	const struct pr_options *options,
	const char *name,
	struct pr_input *input)
{
	struct stat status_of_file;
	time_t when;
	int status;
	int compare;

	/* Standard input has no name in the header and the current time. */
	memset(input, 0, sizeof(*input));
	compare = strcmp(name, "-");
	if (compare == 0) {
		input->name = "";
		input->stream = stdin;
		format_date(time(NULL), input->date, sizeof(input->date));
		return 0;
	}

	/* Opens the file. */
	input->name = name;
	input->stream = fopen(name, "r");
	if (input->stream == NULL) {
		if (!options->quiet)
			fprintf(stderr, "pr: %s: %s\n", name, strerror(errno));
		return -1;
	}

	/* A file is dated by its last modification. */
	when = time(NULL);
	status = fstat(fileno(input->stream), &status_of_file);
	if (status == 0)
		when = status_of_file.st_mtime;
	format_date(when, input->date, sizeof(input->date));

	/* Succeeded: the file is open. */
	return 0;
}

/* Formats a time as the header shows it. */
static void
format_date(
	time_t when,
	char *date,
	size_t size)
{
	struct tm *broken;

	/* The local time, as date "+%b %e %H:%M %Y" writes it. */
	date[0] = '\0';
	broken = localtime(&when);
	if (broken != NULL)
		strftime(date, size, "%b %e %H:%M %Y", broken);
}

/* Reads the next line of an input; returns 0 when there is none. */
static int
read_line(
	struct pr_input *input,
	struct pr_line *line)
{
	ssize_t got;

	/* A finished input has no more lines. */
	if (input->done)
		return 0;

	/* Reads one line. */
	got = getline(&line->text, &line->capacity, input->stream);
	if (got < 0) {
		input->done = 1;
		return 0;
	}

	/* Drops the newline. */
	if (got > 0 && line->text[got - 1] == '\n')
		line->text[got - 1] = '\0';

	/* Numbers it within its file. */
	input->line_number++;
	line->number = input->line_number;
	return 1;
}

/* Works out the rows of a page and the input lines of a column. */
static int
setup_layout(
	struct pr_layout *layout,
	const struct pr_options *options)
{
	size_t capacity;

	/* The header and the trailer need a page of more than ten lines. */
	memset(layout, 0, sizeof(*layout));
	layout->options = options;
	layout->margins = 1;
	if (options->omit || options->length <= 2 * PR_MARGIN)
		layout->margins = 0;

	/* The text rows of a page. */
	layout->rows = options->length;
	if (layout->margins)
		layout->rows -= 2 * PR_MARGIN;
	if (layout->rows <= 0)
		layout->rows = PR_DEFAULT_LENGTH;

	/* Double spacing gives each line two rows. */
	layout->per_column = layout->rows;
	if (options->double_space) {
		layout->per_column = layout->rows / 2;
		if (layout->per_column == 0)
			layout->per_column = 1;
	}

	/* The width of a column's text, checked for room. */
	layout->columns = options->columns;
	set_text_width(layout, options->columns);
	if (options->columns > 1 && layout->text_width < 1) {
		fprintf(stderr, "pr: page width too narrow\n");
		return -1;
	}

	/* Room for a page of lines. */
	capacity = (size_t)layout->per_column * (size_t)options->columns;
	layout->lines = calloc(capacity, sizeof(*layout->lines));
	if (layout->lines == NULL) {
		fprintf(stderr, "pr: out of memory\n");
		return -1;
	}

	/* Succeeded: the layout. */
	return 0;
}

/*
 * Sets the text width of a column: the line width, less the row number
 * of -m -n, shared by the columns with one separator between each two.
 */
static void
set_text_width(
	struct pr_layout *layout,
	int columns)
{
	const struct pr_options *options;
	long width;

	/* The line width: -w, or 72, or 512 with -s. */
	options = layout->options;
	width = PR_DEFAULT_WIDTH;
	if (options->separate)
		width = PR_SEPARATED_WIDTH;
	if (options->width_given)
		width = options->width;

	/* -m -n numbers each row before its columns. */
	layout->prefix_width = 0;
	if (options->merge && options->number) {
		layout->prefix_width = options->number_width + 1;
		if (options->number_char == '\t')
			layout->prefix_width = (options->number_width / PR_DEFAULT_GAP + 1) * PR_DEFAULT_GAP;
	}

	/* The share of each column. */
	layout->text_width = (width - layout->prefix_width - (columns - 1)) / columns;
}

/* Prints one file, page by page, numbering the pages from 1. */
static int
print_file(
	struct pr_layout *layout,
	struct pr_input *input)
{
	size_t capacity;
	size_t count;
	long rows;
	int got;
	int status;

	/* A page holds per_column lines in each column. */
	layout->page = 0;
	capacity = (size_t)layout->per_column * (size_t)layout->columns;

	/* Reads and prints a page of lines at a time. */
	for (;;) {
		count = 0;
		while (count < capacity) {
			got = read_line(input, &layout->lines[count]);
			if (!got)
				break;
			count++;
		}

		/* No line left: the file is done; an empty file prints nothing. */
		if (count == 0)
			break;

		/* Prints the page unless it comes before +page. */
		layout->page++;
		if (layout->page >= layout->options->first_page) {
			begin_page(layout, input->date, input->name);
			rows = print_page_lines(layout, count);
			end_page(layout, rows);
		}

		/* A page that is not full was the last. */
		if (count < capacity)
			break;
	}

	/* Tells a read error from the end of the file. */
	status = ferror(input->stream);
	if (input->stream != stdin)
		fclose(input->stream);
	if (status != 0) {
		fprintf(stderr, "pr: %s: read error\n", input->name);
		return -1;
	}

	/* Succeeded: the file was printed. */
	return 0;
}

/*
 * Prints the lines of one page, in one column or several.  Returns the
 * rows written, blank rows of -d included.
 */
static long
print_page_lines(
	struct pr_layout *layout,
	size_t count)
{
	char cell[PR_LINE_SIZE];
	char line[PR_LINE_SIZE];
	long rows;
	size_t index;
	int written;

	/* Several columns are laid out row by row. */
	if (layout->columns > 1) {
		rows = print_column_rows(layout, count);
		return rows;
	}

	/* One column: each line whole, after the offset and its number. */
	rows = 0;
	for (index = 0; index < count; index++) {
		format_cell(layout, layout->lines[index].text, layout->lines[index].number, layout->options->number, cell, sizeof(cell));
		written = snprintf(line, sizeof(line), "%*s%s", (int)layout->options->offset, "", cell);
		if (written < 0)
			line[0] = '\0';
		rows += write_output_line(layout, line);
	}

	/* Reports the rows. */
	return rows;
}

/*
 * Prints the lines of one page in columns: down each column, balanced on
 * a page that is not full, or across the rows with -a.  Returns the rows
 * written.
 */
static long
print_column_rows(
	struct pr_layout *layout,
	size_t count)
{
	char line[PR_LINE_SIZE];
	int present[PR_COLUMNS_MAX];
	size_t sizes[PR_COLUMNS_MAX];
	size_t starts[PR_COLUMNS_MAX];
	size_t capacity;
	size_t base;
	size_t extra;
	size_t row_count;
	size_t row;
	size_t index;
	int columns;
	int column;
	int written;
	long rows;

	/* Works out which lines each column holds. */
	columns = layout->columns;
	capacity = (size_t)layout->per_column * (size_t)columns;
	base = count / (size_t)columns;
	extra = count % (size_t)columns;
	for (column = 0; column < columns; column++) {
		if (count == capacity) {
			/* A full page: every column holds per_column lines. */
			sizes[column] = (size_t)layout->per_column;
			starts[column] = (size_t)column * (size_t)layout->per_column;
		} else {
			/* A last page: the first extra columns hold one line more. */
			sizes[column] = base;
			if ((size_t)column < extra)
				sizes[column]++;
			starts[column] = (size_t)column * base;
			if ((size_t)column < extra)
				starts[column] += (size_t)column;
			else
				starts[column] += extra;
		}
	}

	/* The rows: the longest column, or the lines across. */
	row_count = sizes[0];
	if (layout->options->across)
		row_count = (count + (size_t)columns - 1) / (size_t)columns;

	/* Writes each row. */
	rows = 0;
	for (row = 0; row < row_count; row++) {
		/* Fills the cells of the row. */
		for (column = 0; column < columns; column++) {
			present[column] = 0;
			pr_cells[column][0] = '\0';
			if (layout->options->across)
				index = row * (size_t)columns + (size_t)column;
			else if (row < sizes[column])
				index = starts[column] + row;
			else
				continue;
			if (index >= count)
				continue;
			format_cell(layout, layout->lines[index].text, layout->lines[index].number, layout->options->number, pr_cells[column], PR_CELL_SIZE);
			present[column] = 1;
		}

		/* Joins them after the offset and writes the row. */
		written = snprintf(line, sizeof(line), "%*s", (int)layout->options->offset, "");
		if (written < 0)
			written = 0;
		join_cells(layout, pr_cells, present, columns, line + written, sizeof(line) - (size_t)written);
		rows += write_output_line(layout, line);
	}

	/* Reports the rows. */
	return rows;
}


/* Prints the files side by side, one column each. */
static int
print_merged(
	struct pr_layout *layout,
	struct pr_input *inputs,
	int count)
{
	struct pr_line line;
	char joined[PR_LINE_SIZE];
	char row_text[PR_LINE_SIZE];
	char prefix[64];
	char date[64];
	int present[PR_COLUMNS_MAX];
	long rows;
	long number;
	int index;
	int got;
	int any;
	int printing;
	int written;

	/* One column for each file, as many as the cells hold. */
	if (count > PR_COLUMNS_MAX) {
		fprintf(stderr, "pr: too many files to merge\n");
		return -1;
	}

	/* One column for each file. */
	layout->columns = count;
	set_text_width(layout, count);
	if (layout->text_width < 1) {
		fprintf(stderr, "pr: page width too narrow\n");
		return -1;
	}

	/* A merged page is dated now and names no file. */
	format_date(time(NULL), date, sizeof(date));

	/* Builds rows until every file is done. */
	memset(&line, 0, sizeof(line));
	layout->page = 0;
	number = 0;
	rows = 0;
	printing = 0;
	for (;;) {
		/* One line from each file that still has one. */
		any = 0;
		for (index = 0; index < count; index++) {
			present[index] = 0;
			pr_cells[index][0] = '\0';
			got = read_line(&inputs[index], &line);
			if (!got)
				continue;
			format_cell(layout, line.text, 0, 0, pr_cells[index], PR_CELL_SIZE);
			present[index] = 1;
			any = 1;
		}

		/* A row without lines means every file is done. */
		if (!any)
			break;

		/* A row that does not fit on the page starts the next one. */
		if (layout->page == 0 || rows >= layout->rows) {
			if (printing)
				end_page(layout, rows);
			layout->page++;
			rows = 0;
			printing = 0;
			if (layout->page >= layout->options->first_page)
				printing = 1;
			if (printing)
				begin_page(layout, date, "");
		}

		/* Rows before +page are counted but not written. */
		number++;
		if (!printing) {
			rows++;
			if (layout->options->double_space)
				rows++;
			continue;
		}

		/* With -n the row number comes first, padded to its room. */
		prefix[0] = '\0';
		if (layout->options->number) {
			if (layout->options->number_char == '\t')
				snprintf(prefix, sizeof(prefix), "%*ld%*s", layout->options->number_width, number, (int)(layout->prefix_width - layout->options->number_width), "");
			else
				snprintf(prefix, sizeof(prefix), "%*ld%c", layout->options->number_width, number, layout->options->number_char);
		}

		/* The offset, the number and the columns make the row. */
		join_cells(layout, pr_cells, present, count, joined, sizeof(joined));
		written = snprintf(row_text, sizeof(row_text), "%*s%s%s", (int)layout->options->offset, "", prefix, joined);
		if (written < 0)
			row_text[0] = '\0';
		rows += write_output_line(layout, row_text);
	}

	/* Ends the last page. */
	if (printing)
		end_page(layout, rows);

	/* Succeeded: the files were printed. */
	free(line.text);
	return 0;
}

/* Starts a page: pauses for a terminal if asked, then writes the header. */
static void
begin_page(
	struct pr_layout *layout,
	const char *date,
	const char *name)
{
	const char *title;
	int terminal;

	/* -p pauses before each page, -f before the first, on a terminal. */
	terminal = isatty(STDOUT_FILENO);
	if (terminal) {
		if (layout->options->pause)
			pause_for_terminal(layout);
		else if (layout->options->pause_first && !layout->paused)
			pause_for_terminal(layout);
	}

	/* Without margins there is no header. */
	if (!layout->margins)
		return;

	/* The header names the file, or what -h gives. */
	title = name;
	if (layout->options->header != NULL)
		title = layout->options->header;
	printf("\n\n%s %s Page %ld\n\n\n", date, title, layout->page);
}

/*
 * Ends a page: a form feed with -F, or empty lines to fill the text rows
 * and the trailer.  Without margins nothing is added.
 */
static void
end_page(
	struct pr_layout *layout,
	long rows_written)
{
	long row;

	/* Without margins the page just ends. */
	if (!layout->margins)
		return;

	/* A form feed replaces the filling and the trailer. */
	if (layout->options->form_feed) {
		putchar('\f');
		return;
	}

	/* Fills the text rows, then writes the trailer. */
	for (row = rows_written; row < layout->rows; row++)
		putchar('\n');
	for (row = 0; row < PR_MARGIN; row++)
		putchar('\n');
}

/*
 * Formats one line for output: its number with -n, then its text with -e
 * tabs expanded.  In columns, a tab moves to the next multiple of eight
 * from the column's start and the text is cut at the column's width.
 * Returns the columns used.
 */
static size_t
format_cell(
	const struct pr_layout *layout,
	const char *text,
	long number,
	int numbered,
	char *out,
	size_t size)
{
	const struct pr_options *options;
	const char *cursor;
	size_t position;
	size_t length;
	size_t limit;
	size_t stop;
	int columned;
	int written;
	int byte;

	/* Columns limit and expand; one column copies. */
	options = layout->options;
	columned = 0;
	if (layout->columns > 1 || options->merge)
		columned = 1;
	limit = size - 1;
	if (columned && (size_t)layout->text_width < limit)
		limit = (size_t)layout->text_width;

	/* The number and its separator come first with -n. */
	length = 0;
	position = 0;
	if (numbered) {
		written = snprintf(out, size, "%*ld", options->number_width, number);
		if (written > 0)
			length = (size_t)written;
		if (length > limit)
			length = limit;
		position = length;

		/* In columns a tab separator becomes spaces. */
		if (columned && options->number_char == '\t') {
			stop = (position / PR_DEFAULT_GAP + 1) * PR_DEFAULT_GAP;
			while (position < stop && length < limit) {
				out[length] = ' ';
				length++;
				position++;
			}
		} else if (length < limit) {
			out[length] = (char)options->number_char;
			length++;
			position++;
		}
	}

	/* Copies the text, expanding tabs where asked. */
	for (cursor = text; *cursor != '\0' && length < limit; cursor++) {
		byte = (unsigned char)*cursor;

		/* An input tab of -e moves to the next multiple of its gap. */
		if (options->expand && byte == options->expand_char) {
			stop = (position / (size_t)options->expand_gap + 1) * (size_t)options->expand_gap;
			while (position < stop && length < limit) {
				out[length] = ' ';
				length++;
				position++;
			}

			/* The tab has been expanded. */
			continue;
		}

		/* In columns a tab moves to the next multiple of eight. */
		if (columned && byte == '\t') {
			stop = (position / PR_DEFAULT_GAP + 1) * PR_DEFAULT_GAP;
			while (position < stop && length < limit) {
				out[length] = ' ';
				length++;
				position++;
			}

			/* The tab has been expanded. */
			continue;
		}

		/* Any other byte is copied. */
		out[length] = (char)byte;
		length++;
		position++;
	}

	/* Terminates the cell. */
	out[length] = '\0';
	return position;
}

/*
 * Joins the cells of a row: each column starts one text width and a
 * separator after the one before, padded with spaces, or with -s after
 * the separator character.  The row ends after its last present cell.
 */
static void
join_cells(
	const struct pr_layout *layout,
	char cells[][PR_CELL_SIZE],
	const int *present,
	int count,
	char *out,
	size_t size)
{
	size_t length;
	size_t pitch;
	size_t target;
	size_t cell_length;
	int last;
	int index;

	/* Finds the last cell with a line. */
	last = -1;
	for (index = 0; index < count; index++) {
		if (present[index])
			last = index;
	}

	/* Joins the cells up to it. */
	length = 0;
	pitch = (size_t)layout->text_width + 1;
	for (index = 0; index <= last; index++) {
		/* Moves to the start of the column. */
		if (index > 0) {
			if (layout->options->separate) {
				if (length + 1 < size) {
					out[length] = (char)layout->options->separator;
					length++;
				}
			} else {
				target = (size_t)index * pitch;
				while (length < target && length + 1 < size) {
					out[length] = ' ';
					length++;
				}
			}
		}

		/* Appends the cell. */
		cell_length = strlen(cells[index]);
		if (length + cell_length + 1 > size)
			cell_length = size - length - 1;
		memcpy(out + length, cells[index], cell_length);
		length += cell_length;
	}

	/* Terminates the row. */
	out[length] = '\0';
}

/*
 * Writes one output line, with runs of spaces replaced by the -i
 * character where they reach a multiple of its gap, and a second, empty
 * line with -d.  Returns the rows written.
 */
static long
write_output_line(
	const struct pr_layout *layout,
	const char *text)
{
	const struct pr_options *options;
	const char *cursor;
	size_t column;
	size_t run_start;
	size_t stop;
	int byte;

	/* Without -i the line goes out as it is. */
	options = layout->options;
	if (!options->tabify) {
		fputs(text, stdout);
	} else {
		/* Keeps spaces waiting until a stop or another byte decides. */
		column = 0;
		run_start = 0;
		for (cursor = text; *cursor != '\0'; cursor++) {
			byte = (unsigned char)*cursor;
			if (byte == ' ') {
				column++;
				stop = (run_start / (size_t)options->tabify_gap + 1) * (size_t)options->tabify_gap;

				/* A run of two or more reaching a stop becomes the character. */
				if (column == stop) {
					if (column - run_start >= 2)
						putchar(options->tabify_char);
					else
						putchar(' ');
					run_start = column;
				}

				/* The space waits for the next byte. */
				continue;
			}

			/* Anything else first writes the spaces that wait. */
			while (run_start < column) {
				putchar(' ');
				run_start++;
			}

			/* Then the byte itself. */
			putchar(byte);

			/* A tab in the text moves to the next multiple of eight. */
			if (byte == '\t')
				column = (column / PR_DEFAULT_GAP + 1) * PR_DEFAULT_GAP;
			else
				column++;
			run_start = column;
		}

		/* Spaces at the end of the line are dropped. */
	}

	/* Ends the line, with an empty one after it for -d. */
	putchar('\n');
	if (options->double_space) {
		putchar('\n');
		return 2;
	}

	/* One row was written. */
	return 1;
}

/* Rings the bell and waits for a newline from the terminal. */
static void
pause_for_terminal(
	struct pr_layout *layout)
{
	int byte;

	/* Opens the terminal on the first pause. */
	fflush(stdout);
	if (layout->terminal == NULL)
		layout->terminal = fopen("/dev/tty", "r");
	layout->paused = 1;
	if (layout->terminal == NULL)
		return;

	/* Rings, then waits for the line. */
	fputc('\a', stderr);
	for (;;) {
		byte = fgetc(layout->terminal);
		if (byte == EOF || byte == '\n')
			break;
	}
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr,
		"usage: pr [+page] [-column] [-adfFmprt] [-e[char][gap]] [-h header]\n"
		"          [-i[char][gap]] [-l lines] [-n[char][width]] [-o offset]\n"
		"          [-s[char]] [-w width] [file...]\n");
	exit(1);
}
