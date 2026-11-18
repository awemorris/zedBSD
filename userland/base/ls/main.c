/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Lists directory contents (POSIX XCU ls, laid out as GNU ls lays it out).
 *
 *	ls [-1AaCcdFfgHhikLlmNnopqRrSstux] [-T cols] [-w cols] [file...]
 *
 * ws001-p041: the options of XCU that were missing: -A (the dot names but
 * . and ..), -c and -u (the status change or the access time for -t and
 * -l; alone they sort by it, as GNU ls does), -f (the directory's order,
 * -a), -g and -o (the long format without the owner or the group), -H
 * (the symbolic links named on the command line followed), -k (blocks of
 * 1024 bytes), -p (a slash after a directory), -s (the blocks a file takes
 * before its name, and a total) and -S (the largest first).  Blocks are
 * of 512 bytes unless -k, as XCU says (the total of -l too).
 *
 * With no operand the current directory is listed.  Operands that are not
 * directories (all of them with -d) are listed first as one sorted group,
 * then each directory's contents, with its name as a header when there
 * are several operands.
 *
 * Written to a terminal, the names go in columns filled down the page
 * (-C) as wide as the terminal, quoted as a shell would need them;
 * written anywhere else, one to a line (-1) as they are.  -x fills the
 * columns across, -m separates the names with commas and -l writes the
 * long format; the last of these on the command line wins.
 *
 * Names are taken as UTF-8 in any locale, unlike GNU ls, which escapes
 * every byte past ASCII in the C locale.
 */

#include "userland/base/common/command.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <locale.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

/* The longest path ls builds by joining a directory and a name. */
#define LS_PATH_CAPACITY 1024U

/* How deep -R goes before it reports a loop. */
#define LS_RECURSION_LIMIT 64

/* The width filled when neither -w, the terminal nor COLUMNS gives one. */
#define LS_DEFAULT_LINE_WIDTH 80U

/* The distance between tab stops when neither -T nor TABSIZE gives one. */
#define LS_DEFAULT_TAB_SIZE 8U

/* The narrowest column: one character and the two spaces after it. */
#define LS_MIN_COLUMN_WIDTH 3U

/* The age past which the long format shows the year instead of the time: six months. */
#define LS_RECENT_SECONDS 15552000

/* The status ls ends with when an option's value is not valid, as GNU ls. */
#define LS_STATUS_INVALID_VALUE 2

/* The characters -F marks names with, which must be quoted in a name so a mark reads as one. */
#define LS_CLASSIFY_QUOTED "*=>@|"

/* The character that ends a directory header, which must be quoted in the directory's name. */
#define LS_HEADER_QUOTED ":"

/*
 * How the names of a listing are laid out.
 */
enum ls_format {
	LS_FORMAT_ONE_PER_LINE,	/* -1: one name to a line */
	LS_FORMAT_COLUMNS,	/* -C: columns filled down */
	LS_FORMAT_ACROSS,	/* -x: columns filled across */
	LS_FORMAT_COMMAS,	/* -m: names separated by commas */
	LS_FORMAT_LONG		/* -l: the long format */
};

/*
 * How a name is written.
 */
enum ls_quoting {
	LS_QUOTING_LITERAL,	/* as it is */
	LS_QUOTING_SHELL	/* quoted as a shell needs it, with $'\ooo' for what cannot be shown */
};

/*
 * What one character of a name asks of the shell quoting.
 */
enum ls_shell_class {
	LS_SHELL_PLAIN,		/* letters, digits and % + , - . / : ] _ */
	LS_SHELL_SPACE,		/* a space: quoted, and still fine inside double quotes */
	LS_SHELL_SPECIAL,	/* a character the shell acts on, such as $ or * */
	LS_SHELL_LEADING,	/* # and ~, which the shell acts on at the start of a word */
	LS_SHELL_ALONE,		/* { and }, which the shell acts on as a whole word */
	LS_SHELL_BARE,		/* # and ~ later in a word, { and } in a longer one: left alone, but not put in double quotes */
	LS_SHELL_APOSTROPHE,	/* ', which single quotes cannot hold */
	LS_SHELL_CONTROL,	/* a control character with a C escape, such as \t */
	LS_SHELL_OTHER		/* anything else: printable or not by the locale */
};

/*
 * Which time -t sorts by and -l shows.
 */
enum ls_time_field {
	LS_TIME_MODIFIED,	/* the default: the last change of the contents */
	LS_TIME_CHANGED,	/* -c: the last change of the status */
	LS_TIME_ACCESSED	/* -u: the last access */
};

/*
 * The options of one run of ls.  main() fills it from the command line
 * and the environment, and everything else only reads it.
 */
struct options {
	int all;		/* -a, -A, -f: names starting with a dot too */
	int dots;		/* -a, -f: . and .. too */
	int unsorted;		/* -f: the directory's own order */
	int time_field;		/* an ls_time_field: -c or -u */
	int size_sort;		/* -S: the largest first */
	int no_owner;		/* -g: the long format without the owner */
	int no_group;		/* -o: the long format without the group */
	int follow_operands;	/* -H: the symbolic links named on the command line followed */
	int kilobytes;		/* -k: blocks of 1024 bytes */
	int slash;		/* -p: a slash after a directory's name */
	int blocks;		/* -s: the blocks a file takes, before its name */
	int directory;		/* -d: a directory operand as itself */
	int classify;		/* -F: a mark after the name for the type */
	int human;		/* -h: sizes with a unit */
	int numeric;		/* -n: the long format with the owner's and the group's numbers */
	int recursive;		/* -R: the subdirectories too */
	int reverse;		/* -r: the order reversed */
	int time_sort;		/* -t: newest first */
	int inode;		/* -i: the file serial number first */
	int follow;		/* -L: what a symbolic link points to */
	int format;		/* an ls_format: -1, -C, -x, -m or -l, the last given */
	int format_given;	/* 1 when the command line chose the format */
	int literal;		/* -N: names never quoted */
	int quoting;		/* an ls_quoting */
	int hide_control;	/* -q, or a terminal: a character that cannot be shown written as ? */
	int align_quotes;	/* in a listing with a quoted name, the others get a space before them */
	int width_given;	/* 1 when -w gave the width */
	size_t line_width;	/* the width -C, -x and -m fill, 0 for no limit */
	int tab_given;		/* 1 when -T gave the tab size */
	size_t tab_size;	/* the distance between tab stops, 0 for spaces only */
};

/*
 * One name to list, with its status and the text it is written as.
 *
 * The name is allocated by the entry, except for the one list_operand()
 * builds around its operand.  The shown text is filled by prepare_names()
 * just before the entry is written and freed with the entry.
 */
struct entry {
	char *name;
	struct stat status;
	int status_valid;	/* 0 when the status could not be read */
	char *shown;		/* the name as written: quoted, or with ? for what cannot be shown */
	size_t shown_width;	/* the columns of the terminal the shown name takes */
	int quoted;		/* 1 when the shown name is enclosed in quotes */
};

/*
 * The widths of the columns of the long format, the widest value of each
 * among the entries listed together.
 */
struct long_widths {
	size_t links;
	size_t user;
	size_t group;
	size_t size;
};

/*
 * What the names of one listing share: the digits of the widest serial
 * number (-i), and whether any name is quoted, so that the others get a
 * space before them and the names line up.
 */
struct name_layout {
	size_t inode_width;
	size_t blocks_width;
	int some_quoted;
};

/*
 * One candidate number of columns for -C and -x, while the names are
 * measured: the width of each column so far, the length of the line they
 * add up to, and whether that line still fits.
 */
struct column_fit {
	int fits;
	size_t line_length;
	size_t *widths;
};

/*
 * A string being built with room reserved for the longest result, so
 * that appending never fails.
 */
struct text_writer {
	char *text;
	size_t length;
};

static int parse_options(int argc, char **argv, struct options *options, char ***operands, int *operand_count);
static void settle_layout(struct options *options);
static int parse_count(const char *text, size_t *value);
static int finish_output(int failed);
static int list_operands(int count, char **names, const struct options *options);
static int operand_status(const char *name, const struct options *options, int command_line, struct stat *status);
static int list_operand(const char *path, const struct options *options, int header);
static int list_directory(const char *path, const struct options *options, int header, int depth);
static int list_subdirectories(const char *path, const struct entry *items, size_t count, const struct options *options, int depth);
static int load(const char *path, const struct options *options, struct entry **result, size_t *result_count);
static int read_entries(DIR *directory, const char *path, const struct options *options, struct entry **items, size_t *count);
static char *copy_string(const char *text);
static int join_path(const char *directory, const char *name, char *out, size_t capacity);
static void sort_entries(struct entry *items, size_t count, const struct options *options);
static int compare(const struct entry *left, const struct entry *right, const struct options *options);
static int print_entries(const char *path, struct entry *items, size_t count, struct entry *measured_too, size_t measured_count, const struct options *options, int total);
static int prepare_names(struct entry *items, size_t count, const struct options *options, struct name_layout *layout);
static int follows_operand_links(const struct options *options);
static int print_long_entries(const char *path, struct entry *items, size_t count, const struct entry *measured_too, size_t measured_count, const struct options *options, const struct name_layout *layout, int total);
static void print_one_per_line(const struct entry *items, size_t count, const struct options *options, const struct name_layout *layout);
static int print_columns(const struct entry *items, size_t count, const struct options *options, const struct name_layout *layout, int down);
static int fit_columns(const struct entry *items, size_t count, const struct options *options, const struct name_layout *layout, int down, struct column_fit **result, size_t *result_columns);
static void print_down(const struct entry *items, size_t count, const struct options *options, const struct name_layout *layout, const struct column_fit *fit, size_t columns);
static void print_across(const struct entry *items, size_t count, const struct options *options, const struct name_layout *layout, const struct column_fit *fit, size_t columns);
static void print_separated(const struct entry *items, size_t count, const struct options *options, const struct name_layout *layout, char separator);
static void indent(size_t from, size_t to, const struct options *options);
static size_t name_length(const struct entry *item, const struct options *options, const struct name_layout *layout);
static void print_name(const struct entry *item, const struct options *options, const struct name_layout *layout);
static int name_padded(const struct entry *item, const struct options *options, const struct name_layout *layout);
static void inode_text(const struct entry *item, char out[24]);
static void print_header(const char *path, const struct options *options);
static int quote_name(const char *name, const struct options *options, const char *quoted_too, char **shown, size_t *width, int *quoted);
static int quote_shell(const char *name, size_t length, const char *quoted_too, struct text_writer *writer);
static int shell_class(const char *name, size_t length, size_t at);
static void shell_escape_quote(const char *name, size_t length, struct text_writer *writer);
static void hide_unprintable(const char *name, size_t length, struct text_writer *writer, size_t *width);
static size_t character_length(const char *text, size_t length, size_t at, int *printable);
static size_t display_width(const char *text, size_t length);
static void write_bytes(struct text_writer *writer, const char *bytes, size_t length);
static void write_char(struct text_writer *writer, char character);
static void measure_long(const struct entry *items, size_t count, const struct options *options, struct long_widths *widths);
static void human_size(off_t value, char out[16]);
static const char *uid_name(uid_t id, int numeric, char out[24]);
static const char *gid_name(gid_t id, int numeric, char out[24]);
static int print_long(const char *directory, const struct entry *item, const struct options *options, const struct long_widths *widths, const struct name_layout *layout);
static void print_link_target(const char *path, const struct options *options);
static void mode_text(mode_t mode, char out[11]);
static char type_char(mode_t mode);
static void ls_time(time_t value, char out[32]);
static int days_in_year(long long year);
static int days_in_month(int month, long long year);
static char type_mark(mode_t mode);
static char entry_mark(const struct entry *item, const struct options *options);
static void blocks_text(const struct entry *item, const struct options *options, char out[24]);
static unsigned long long blocks_in_units(unsigned long long blocks, const struct options *options);
static struct timespec entry_time(const struct entry *item, const struct options *options);
static int compare_times(const struct timespec *left, const struct timespec *right);
static void print_total(const struct entry *items, size_t count, const struct options *options);
static void free_entries(struct entry *items, size_t count);

/*
 * The long forms of the options, which GNU ls also takes.
 *
 * command_options_next() reads it for every --name on the command line;
 * each entry reports the letter of the short form, so both forms share one
 * case of parse_options().  It is constant for the whole run.
 */
static const struct command_long_option ls_long_options[] = {
	{"all", COMMAND_VALUE_NONE, 'a'},
	{"classify", COMMAND_VALUE_NONE, 'F'},
	{"dereference", COMMAND_VALUE_NONE, 'L'},
	{"directory", COMMAND_VALUE_NONE, 'd'},
	{"hide-control-chars", COMMAND_VALUE_NONE, 'q'},
	{"human-readable", COMMAND_VALUE_NONE, 'h'},
	{"inode", COMMAND_VALUE_NONE, 'i'},
	{"literal", COMMAND_VALUE_NONE, 'N'},
	{"recursive", COMMAND_VALUE_NONE, 'R'},
	{"reverse", COMMAND_VALUE_NONE, 'r'},
	{"tabsize", COMMAND_VALUE_REQUIRED, 'T'},
	{"width", COMMAND_VALUE_REQUIRED, 'w'},
	{NULL, 0, 0}
};

/*
 * Runs ls: lists the operands, or the current directory without one.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	char **operands;
	int operand_count;
	int failed;
	int listed;

	/*
	 * Names are UTF-8 whatever the locale says, because the system supports no
	 * other encoding (the user's decision of 2026-09-29): a printable
	 * character is written as it is and measured by its width.  Without a
	 * UTF-8 locale in the C library the names fall back to single bytes.
	 */
	(void)setlocale(LC_CTYPE, "C.UTF-8");

	/* The options from the command line; a bad one ends the run. */
	failed = parse_options(argc, argv, &options, &operands, &operand_count);
	if (failed != 0)
		return failed;

	/* What the command line left to the output and the environment. */
	settle_layout(&options);

	/* No operand lists the current directory; otherwise files first as one sorted group, then each directory. */
	if (operand_count == 0) {
		listed = list_operand(".", &options, options.recursive);
		failed = 1;
		if (listed)
			failed = 0;
	} else {
		failed = list_operands(operand_count, operands, &options);
	}

	/* A failed write fails the run too. */
	failed = finish_output(failed);

	/* Reports a failure, which the status already says. */
	if (failed != 0)
		return failed;

	/* Succeeded. */
	return 0;
}

/*
 * Reads the options from the command line in the GNU order (options may
 * follow operands unless POSIXLY_CORRECT is set).  Returns 0 with the
 * operands, or the status ls ends with after a message.
 */
static int
parse_options(
	int argc,
	char **argv,
	struct options *options,
	char ***operands,
	int *operand_count)
{
	struct command_options scan;
	int code;
	int parsed;

	/* Nothing chosen yet. */
	memset(options, 0, sizeof(*options));
	options->format = LS_FORMAT_ONE_PER_LINE;
	options->line_width = LS_DEFAULT_LINE_WIDTH;
	options->tab_size = LS_DEFAULT_TAB_SIZE;

	/* The scan of the command line. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "ls";
	scan.letters = "1AaCcdFfgHhikLlmnNopqRrSstuxT:w:";
	scan.names = ls_long_options;
	command_options_start(&scan);

	/* Each option in turn. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;

		/* Chooses what the option sets. */
		switch (code) {
		case 'a':
			options->all = 1;
			options->dots = 1;
			break;
		case 'A':
			/* The dot names, but . and .. (the later of -a and -A wins, as GNU ls takes them). */
			options->all = 1;
			options->dots = 0;
			break;
		case 'f':
			/* The directory's own order, every name. */
			options->all = 1;
			options->dots = 1;
			options->unsorted = 1;
			break;
		case 'c':
			options->time_field = LS_TIME_CHANGED;
			break;
		case 'u':
			options->time_field = LS_TIME_ACCESSED;
			break;
		case 'S':
			options->size_sort = 1;
			options->time_sort = 0;
			break;
		case 'g':
			/* The long format without the owner. */
			options->no_owner = 1;
			options->format = LS_FORMAT_LONG;
			options->format_given = 1;
			break;
		case 'o':
			/* The long format without the group. */
			options->no_group = 1;
			options->format = LS_FORMAT_LONG;
			options->format_given = 1;
			break;
		case 'H':
			options->follow_operands = 1;
			break;
		case 'k':
			options->kilobytes = 1;
			break;
		case 'p':
			options->slash = 1;
			break;
		case 's':
			options->blocks = 1;
			break;
		case 'd':
			options->directory = 1;
			break;
		case 'F':
			options->classify = 1;
			break;
		case 'h':
			options->human = 1;
			break;
		case 'i':
			options->inode = 1;
			break;
		case 'L':
			options->follow = 1;
			break;
		case 'N':
			options->literal = 1;
			break;
		case 'q':
			options->hide_control = 1;
			break;
		case 'R':
			options->recursive = 1;
			break;
		case 'r':
			options->reverse = 1;
			break;
		case 't':
			/* The later of -t and -S wins. */
			options->time_sort = 1;
			options->size_sort = 0;
			break;
		case 'l':
			options->format = LS_FORMAT_LONG;
			options->format_given = 1;
			break;
		case 'n':
			/* POSIX: -l with the numbers of the owner and the group. */
			options->numeric = 1;
			options->format = LS_FORMAT_LONG;
			options->format_given = 1;
			break;
		case '1':
			options->format = LS_FORMAT_ONE_PER_LINE;
			options->format_given = 1;
			break;
		case 'C':
			options->format = LS_FORMAT_COLUMNS;
			options->format_given = 1;
			break;
		case 'x':
			options->format = LS_FORMAT_ACROSS;
			options->format_given = 1;
			break;
		case 'm':
			options->format = LS_FORMAT_COMMAS;
			options->format_given = 1;
			break;
		case 'w':
			/* The width must be a count. */
			parsed = parse_count(scan.value, &options->line_width);
			if (parsed < 0) {
				fprintf(stderr, "ls: invalid line width: '%s'\n", scan.value);
				return LS_STATUS_INVALID_VALUE;
			}

			/* A width too large to hold is no limit, as GNU ls takes it. */
			if (parsed > 0)
				options->line_width = 0;

			/* The width is settled. */
			options->width_given = 1;
			break;
		case 'T':
			/* The tab size must be a count. */
			parsed = parse_count(scan.value, &options->tab_size);
			if (parsed < 0) {
				fprintf(stderr, "ls: invalid tab size: '%s'\n", scan.value);
				return LS_STATUS_INVALID_VALUE;
			}

			/* A tab size too large to hold is refused, as GNU ls refuses it. */
			if (parsed > 0) {
				fprintf(stderr, "ls: invalid tab size: '%s': %s\n", scan.value, strerror(EOVERFLOW));
				return LS_STATUS_INVALID_VALUE;
			}

			/* The tab size is settled. */
			options->tab_given = 1;
			break;
		default:
			fprintf(stderr, "usage: ls [-1AaCcdFfgHhikLlmNnopqRrSstux] [-T cols] [-w cols] [file...]\n");
			return 1;
		}
	}

	/* -c and -u without the long format sort by their time, as GNU ls does (with it, only -t sorts by time); -S keeps its order. */
	if (options->time_field != LS_TIME_MODIFIED && options->format != LS_FORMAT_LONG && !options->size_sort)
		options->time_sort = 1;

	/* Succeeded: the operands the scan set aside. */
	*operands = scan.operands;
	*operand_count = scan.operand_count;
	return 0;
}

/*
 * Settles what the command line left to the output and the environment:
 * the format, the quoting, the width and the tab size, as GNU ls does.
 */
static void
settle_layout(
	struct options *options)
{
	struct winsize window;
	const char *variable;
	int terminal;
	int error;
	int parsed;
	int width_known;
	int uses_width;

	/* A terminal gets columns unless the command line chose the format. */
	terminal = isatty(STDOUT_FILENO);
	if (!options->format_given && terminal)
		options->format = LS_FORMAT_COLUMNS;

	/* A terminal gets ? for what it cannot show, as -q asks. */
	if (terminal)
		options->hide_control = 1;

	/* A terminal gets names quoted as a shell needs them, unless -N. */
	options->quoting = LS_QUOTING_LITERAL;
	if (terminal && !options->literal)
		options->quoting = LS_QUOTING_SHELL;

	/* Only the formats that fill a line look for its width and the tab size. */
	uses_width = 0;
	if (options->format == LS_FORMAT_COLUMNS) {
		uses_width = 1;
	} else if (options->format == LS_FORMAT_ACROSS) {
		uses_width = 1;
	} else if (options->format == LS_FORMAT_COMMAS) {
		uses_width = 1;
	}

	/* The width: -w, then the terminal's, then COLUMNS, then 80. */
	width_known = options->width_given;
	if (uses_width && !width_known && terminal) {
		error = ioctl(STDOUT_FILENO, TIOCGWINSZ, &window);
		if (error == 0 && window.ws_col > 0) {
			options->line_width = window.ws_col;
			width_known = 1;
		}
	}

	/* COLUMNS, when it is set and not empty; a value too large is no limit. */
	variable = NULL;
	if (uses_width && !width_known)
		variable = getenv("COLUMNS");
	if (variable != NULL && variable[0] != '\0') {
		parsed = parse_count(variable, &options->line_width);
		if (parsed > 0) {
			/* Too large to hold: no limit. */
			options->line_width = 0;
		} else if (parsed < 0) {
			/* Not a count: said, and the default kept. */
			fprintf(stderr, "ls: ignoring invalid width in environment variable COLUMNS: '%s'\n", variable);
			options->line_width = LS_DEFAULT_LINE_WIDTH;
		}
	}

	/* TABSIZE, unless -T gave the tab size. */
	variable = NULL;
	if (uses_width && !options->tab_given)
		variable = getenv("TABSIZE");
	if (variable != NULL) {
		parsed = parse_count(variable, &options->tab_size);
		if (parsed != 0) {
			fprintf(stderr, "ls: ignoring invalid tab size in environment variable TABSIZE: '%s'\n", variable);
			options->tab_size = LS_DEFAULT_TAB_SIZE;
		}
	}

	/*
	 * Names line up behind a quote only where they are lined up at all:
	 * in columns that have a width and in the long format.
	 */
	options->align_quotes = 0;
	if (options->quoting == LS_QUOTING_SHELL) {
		if (options->format == LS_FORMAT_LONG) {
			options->align_quotes = 1;
		} else if (options->format == LS_FORMAT_COLUMNS && options->line_width != 0) {
			options->align_quotes = 1;
		} else if (options->format == LS_FORMAT_ACROSS && options->line_width != 0) {
			options->align_quotes = 1;
		}
	}
}

/*
 * Reads a count as GNU ls reads -w and -T: decimal, octal with a leading
 * 0 or hexadecimal with 0x, and nothing after the digits.  Returns 0, 1
 * when the count is too large to hold, and -1 when it is not a count.
 */
static int
parse_count(
	const char *text,
	size_t *value)
{
	const char *start;
	char *end;
	unsigned long long count;

	/* Blanks may come first, as strtoull takes them. */
	start = text;
	while (*start == ' ' ||
	       (*start >= '\t' &&
		*start <= '\r'))
		start++;

	/* A sign or nothing at all is not a count. */
	if (*start == '-' || *start == '\0')
		return -1;

	/* The digits, which must be all there is. */
	errno = 0;
	count = strtoull(start, &end, 0);
	if (end == start || *end != '\0')
		return -1;

	/* A count past what strtoull holds, or past what a size holds. */
	if (errno == ERANGE)
		return 1;
	if (count > (unsigned long long)(size_t)-1)
		return 1;

	/* Succeeded: the count. */
	*value = (size_t)count;
	return 0;
}

/*
 * Writes out what is still buffered, so that a failed write is reported
 * instead of a partial listing; returns the status ls ends with.
 */
static int
finish_output(
	int failed)
{
	int error;
	int stream_error;

	/* A failed write, now or earlier, is status 1. */
	error = fflush(stdout);
	if (error != 0)
		return 1;
	stream_error = ferror(stdout);
	if (stream_error)
		return 1;

	/* Reports the listing's own failure. */
	if (failed != 0)
		return failed;

	/* Succeeded. */
	return 0;
}

/*
 * Lists the operands as POSIX orders them: the missing ones reported first,
 * then every file (or directory with -d) as one sorted group, then each
 * directory in sorted order with its name as a header when there are
 * several operands.  Returns 0, 2 when an operand could not be accessed
 * (as GNU ls does), and 1 for any other failure.
 */
static int
list_operands(
	int count,
	char **names,
	const struct options *options)
{
	struct entry *files;
	struct entry *directories;
	struct entry *entry;
	struct stat status;
	size_t file_count;
	size_t directory_count;
	size_t index;
	int error;
	int failed;
	int directory;
	int header;
	int listed;

	/* Room for every operand among the files. */
	files = calloc((size_t)count, sizeof(*files));
	if (files == NULL) {
		fprintf(stderr, "ls: out of memory\n");
		return 1;
	}

	/* And among the directories. */
	directories = calloc((size_t)count, sizeof(*directories));
	if (directories == NULL) {
		fprintf(stderr, "ls: out of memory\n");
		free(files);
		return 1;
	}

	/* Each operand into its group; a missing one is reported now. */
	failed = 0;
	file_count = 0;
	directory_count = 0;
	for (index = 0; index < (size_t)count; index++) {
		error = operand_status(names[index], options, 1, &status);
		if (error != 0) {
			command_error("ls", names[index]);
			failed = 2;
			continue;
		}

		/* A directory is listed by its contents, unless -d. */
		directory = S_ISDIR(status.st_mode);
		if (options->directory)
			directory = 0;
		if (directory) {
			entry = &directories[directory_count];
			directory_count++;
		} else {
			entry = &files[file_count];
			file_count++;
		}

		/* The entry's name; without memory the operand is left out. */
		entry->name = copy_string(names[index]);
		if (entry->name == NULL) {
			/* The place in the group is given back. */
			if (directory) {
				directory_count--;
			} else {
				file_count--;
			}

			/* The failure is said, and the next operand taken. */
			fprintf(stderr, "ls: out of memory\n");
			failed = 1;
			continue;
		}

		/* Its status, read already. */
		entry->status = status;
		entry->status_valid = 1;
	}

	/*
	 * The files, sorted together; in the long format without a total.
	 * The directories are measured with them, as GNU ls reads every
	 * operand before it sets the directories aside.
	 */
	sort_entries(files, file_count, options);
	if (file_count > 0) {
		listed = print_entries("", files, file_count, directories, directory_count, options, 0);
		if (!listed && failed == 0)
			failed = 1;
	}

	/* The directories, sorted, each under its name among several operands or with -R. */
	sort_entries(directories, directory_count, options);
	header = 0;
	if (count > 1) {
		header = 1;
	} else if (options->recursive) {
		header = 1;
	}

	/* Each directory, after a blank line when something came before it. */
	for (index = 0; index < directory_count; index++) {
		if (file_count > 0 || index > 0)
			putchar('\n');

		/* Its listing. */
		listed = list_directory(directories[index].name, options, header, 0);
		if (!listed && failed == 0)
			failed = 1;
	}

	/* The groups are done with. */
	free_entries(files, file_count);
	free_entries(directories, directory_count);

	/* Reports whether any operand failed. */
	if (failed != 0)
		return failed;

	/* Succeeded. */
	return 0;
}

/*
 * Reads the status of a path: of what a symbolic link points to with -L,
 * and of the link itself otherwise; with -L a dangling link is still
 * listed as a link.  A link named on the command line (command_line) that
 * points to a directory is followed too, unless -d, -F or -l, as POSIX
 * and GNU ls have it.  Returns 0 or -1.
 */
static int
operand_status(
	const char *name,
	const struct options *options,
	int command_line,
	struct stat *status)
{
	int error;
	int follows;
	int directory;

	/* -L, and -H for a name on the command line, follow the link when it leads somewhere. */
	if (options->follow || (command_line && options->follow_operands)) {
		error = stat(name, status);
		if (error == 0)
			return 0;
	}

	/* An operand is followed when it leads to a directory. */
	follows = 0;
	if (command_line && !options->follow)
		follows = follows_operand_links(options);
	if (follows) {
		error = stat(name, status);
		if (error == 0) {
			directory = S_ISDIR(status->st_mode);
			if (directory)
				return 0;
		} else if (errno != ENOENT && errno != ELOOP) {
			/* Only a link leading nowhere is listed as itself; any other failure is reported. */
			return -1;
		}
	}

	/* The entry itself. */
	error = lstat(name, status);
	if (error != 0)
		return -1;

	/* Succeeded. */
	return 0;
}

/*
 * Returns 1 when a symbolic link named on the command line that points to
 * a directory is listed as the directory: unless -d, -F or -l asks about
 * the link itself.
 */
static int
follows_operand_links(
	const struct options *options)
{
	/* The options that list the link itself. */
	if (options->directory)
		return 0;
	if (options->classify)
		return 0;
	if (options->format == LS_FORMAT_LONG)
		return 0;

	/* Succeeded: the link is followed. */
	return 1;
}

/*
 * Lists the one implicit operand (the current directory): its contents,
 * or itself with -d.  Returns 1, or 0 after a message.
 */
static int
list_operand(
	const char *path,
	const struct options *options,
	int header)
{
	struct stat status;
	struct entry item;
	int error;
	int directory;
	int listed;

	/* The operand's own status. */
	error = lstat(path, &status);
	if (error != 0) {
		command_error("ls", path);
		return 0;
	}

	/* A directory is listed by its contents, unless -d. */
	directory = S_ISDIR(status.st_mode);
	if (directory && !options->directory) {
		listed = list_directory(path, options, header, 0);
		if (!listed)
			return 0;

		/* Succeeded: the contents are listed. */
		return 1;
	}

	/* Anything else is listed as itself, a group of one without a total. */
	memset(&item, 0, sizeof(item));
	item.name = (char *)path;
	item.status = status;
	item.status_valid = 1;
	listed = print_entries("", &item, 1, NULL, 0, options, 0);
	free(item.shown);

	/* Reports an entry that could not be listed. */
	if (!listed)
		return 0;

	/* Succeeded. */
	return 1;
}

/*
 * Lists the contents of a directory, with its name as a header when asked,
 * and with -R the subdirectories after it.  Returns 1, or 0 when anything
 * could not be listed.
 */
static int
list_directory(
	const char *path,
	const struct options *options,
	int header,
	int depth)
{
	struct entry *items;
	size_t count;
	int loaded;
	int printed;
	int ok;

	/* -R through a loop of links ends somewhere. */
	if (depth > LS_RECURSION_LIMIT) {
		errno = ELOOP;
		command_error("ls", path);
		return 0;
	}

	/* The entries, sorted. */
	loaded = load(path, options, &items, &count);
	if (!loaded) {
		command_error("ls", path);
		return 0;
	}

	/* The header, when asked for. */
	if (header)
		print_header(path, options);

	/* The entries, with the total of their blocks. */
	ok = 1;
	printed = print_entries(path, items, count, NULL, 0, options, 1);
	if (!printed)
		ok = 0;

	/* -R: each subdirectory after this one. */
	if (options->recursive) {
		printed = list_subdirectories(path, items, count, options, depth);
		if (!printed)
			ok = 0;
	}

	/* The entries are done with. */
	free_entries(items, count);

	/* Reports what could not be listed. */
	if (!ok)
		return 0;

	/* Succeeded. */
	return 1;
}

/* Lists each subdirectory among a directory's entries (-R); returns 1, or 0 on a failure. */
static int
list_subdirectories(
	const char *path,
	const struct entry *items,
	size_t count,
	const struct options *options,
	int depth)
{
	char child[LS_PATH_CAPACITY];
	size_t index;
	int directory;
	int dot;
	int dot_dot;
	int joined;
	int listed;
	int ok;

	/* Each entry that is a directory, other than . and .. */
	ok = 1;
	for (index = 0; index < count; index++) {
		if (!items[index].status_valid)
			continue;
		directory = S_ISDIR(items[index].status.st_mode);
		if (!directory)
			continue;
		dot = strcmp(items[index].name, ".");
		dot_dot = strcmp(items[index].name, "..");
		if (dot == 0 || dot_dot == 0)
			continue;

		/* The path of the subdirectory. */
		joined = join_path(path, items[index].name, child, sizeof(child));
		if (!joined) {
			command_error("ls", items[index].name);
			ok = 0;
			continue;
		}

		/* A blank line, then the subdirectory under its own header. */
		putchar('\n');
		listed = list_directory(child, options, 1, depth + 1);
		if (!listed)
			ok = 0;
	}

	/* Reports what could not be listed. */
	if (!ok)
		return 0;

	/* Succeeded. */
	return 1;
}

/*
 * Reads the entries of a directory with their status, sorted.  Returns 1
 * with an array the caller frees with free_entries(), or 0 with errno set
 * and nothing to free.
 */
static int
load(
	const char *path,
	const struct options *options,
	struct entry **result,
	size_t *result_count)
{
	struct entry *items;
	DIR *directory;
	size_t count;
	int read;
	int closed;
	int saved;

	/* The directory. */
	directory = opendir(path);
	if (directory == NULL)
		return 0;

	/* Its entries; a partial list is not published after a read error. */
	items = NULL;
	count = 0;
	read = read_entries(directory, path, options, &items, &count);
	if (!read) {
		saved = errno;
		(void)closedir(directory);
		free_entries(items, count);
		errno = saved;
		return 0;
	}

	/* A failure to close is a failure of the directory too. */
	closed = closedir(directory);
	if (closed != 0) {
		saved = errno;
		free_entries(items, count);
		errno = saved;
		return 0;
	}

	/* Succeeded: the entries, sorted. */
	sort_entries(items, count, options);
	*result = items;
	*result_count = count;
	return 1;
}

/*
 * Reads the entries of an open directory into a growing array, with -a
 * starting with . and .. and otherwise leaving out the names starting with
 * a dot.  Returns 1, or 0 with errno set; either way *items and *count
 * describe what was read.
 */
static int
read_entries(
	DIR *directory,
	const char *path,
	const struct options *options,
	struct entry **items,
	size_t *count)
{
	static const char *const dots[] = {".", ".."};
	char child[LS_PATH_CAPACITY];
	struct dirent *found;
	struct entry *larger;
	const char *name;
	size_t capacity;
	unsigned dot;
	int dot_name;
	int dot_dot_name;
	int joined;
	int error;

	/* Each name until the end of the directory. */
	capacity = 0;
	dot = 0;
	for (;;) {
		if (options->dots && !options->unsorted && dot < 2U) {
			/* -a lists . and .. first, whatever order the directory has them in. */
			name = dots[dot];
			dot++;
		} else {
			/* errno tells a read error from the end of the directory. */
			errno = 0;
			found = readdir(directory);
			if (found == NULL && errno != 0)
				return 0;
			if (found == NULL)
				break;
			name = found->d_name;

			/* Without -a a name starting with a dot is hidden. */
			if (!options->all && name[0] == '.')
				continue;

			/* With -a, . and .. have been listed already (with -f they come in the directory's order); -A leaves them out. */
			dot_name = strcmp(name, ".");
			dot_dot_name = strcmp(name, "..");
			if (options->all &&
			    !(options->dots && options->unsorted) &&
			    (dot_name == 0 ||
			     dot_dot_name == 0))
				continue;
		}

		/* The array doubles when it is full. */
		if (*count == capacity) {
			capacity = 16U;
			if (*count != 0)
				capacity = *count * 2U;
			larger = realloc(*items, capacity * sizeof(**items));
			if (larger == NULL)
				return 0;
			*items = larger;
		}

		/* The name; the text it is written as comes later. */
		(*items)[*count].shown = NULL;
		(*items)[*count].name = copy_string(name);
		if ((*items)[*count].name == NULL)
			return 0;

		/* Its status, which a path too long for the buffer leaves unread. */
		(*items)[*count].status_valid = 0;
		joined = join_path(path, name, child, sizeof(child));
		if (joined) {
			error = operand_status(child, options, 0, &(*items)[*count].status);
			if (error == 0)
				(*items)[*count].status_valid = 1;
		}

		/* The entry is in the array. */
		(*count)++;
	}

	/* Succeeded: the whole directory. */
	return 1;
}

/* Returns an allocated copy of a string, or NULL without memory. */
static char *
copy_string(
	const char *text)
{
	size_t size;
	char *copy;

	/* Room for the text and its terminating null. */
	size = strlen(text) + 1U;
	copy = malloc(size);
	if (copy == NULL)
		return NULL;

	/* Succeeded: the copy. */
	memcpy(copy, text, size);
	return copy;
}

/*
 * Writes directory/name into a buffer, with a slash between them unless
 * the directory is empty or ends in one.  Returns 1, or 0 with errno
 * ENAMETOOLONG when it does not fit.
 */
static int
join_path(
	const char *directory,
	const char *name,
	char *out,
	size_t capacity)
{
	size_t directory_length;
	size_t name_length;
	size_t slash;

	/* A slash is needed after a directory that does not end in one. */
	directory_length = strlen(directory);
	name_length = strlen(name);
	slash = 0;
	if (directory_length > 0 && directory[directory_length - 1U] != '/')
		slash = 1;

	/* The path must fit with its terminating null. */
	if (directory_length + slash + name_length + 1U > capacity) {
		errno = ENAMETOOLONG;
		return 0;
	}

	/* Succeeded: the directory, the slash and the name. */
	memcpy(out, directory, directory_length);
	if (slash)
		out[directory_length] = '/';
	memcpy(out + directory_length + slash, name, name_length + 1U);
	return 1;
}

/* Sorts entries in the order compare() gives, keeping equal ones in place. */
static void
sort_entries(
	struct entry *items,
	size_t count,
	const struct options *options)
{
	struct entry value;
	size_t index;
	size_t at;
	int order;

	/* -f keeps the directory's own order. */
	if (options->unsorted)
		return;

	/* An insertion sort: each entry moves back past the ones that follow it in order. */
	for (index = 1; index < count; index++) {
		value = items[index];
		at = index;
		while (at > 0) {
			order = compare(&items[at - 1U], &value, options);
			if (order <= 0)
				break;
			items[at] = items[at - 1U];
			at--;
		}

		/* The entry in its place. */
		items[at] = value;
	}
}

/*
 * Orders two entries: by name, with -S the largest first and with -t the
 * newest first (the time -c or -u chose), then by name; -r reverses the
 * order.  Returns less than, equal to or greater than 0.
 */
static int
compare(
	const struct entry *left,
	const struct entry *right,
	const struct options *options)
{
	struct timespec left_time;
	struct timespec right_time;
	int order;
	int known;

	/* By name unless a key below decides; the keys need both entries' status. */
	order = 0;
	known = 0;
	if (left->status_valid && right->status_valid)
		known = 1;

	/* -S, when both sizes are known: larger first (the later of -S and -t was kept). */
	if (options->size_sort && known) {
		if (left->status.st_size > right->status.st_size)
			order = -1;
		else if (left->status.st_size < right->status.st_size)
			order = 1;
	} else if (options->time_sort && known) {
		/* -t: newer first. */
		left_time = entry_time(left, options);
		right_time = entry_time(right, options);
		order = -compare_times(&left_time, &right_time);
	}

	/* Then by name. */
	if (order == 0)
		order = strcmp(left->name, right->name);

	/* -r turns it around. */
	if (options->reverse)
		return -order;

	/* Succeeded: the order. */
	return order;
}

/*
 * Writes a group of entries in the chosen format; in the long format a
 * directory's entries (total set) come after the total of their blocks.
 * The entries measured_too are not written but measured with the group:
 * they widen the columns of the long format and the serial numbers and
 * can make its names line up behind a space.  Returns 1, or 0 when an
 * entry could not be listed.
 */
static int
print_entries(
	const char *path,
	struct entry *items,
	size_t count,
	struct entry *measured_too,
	size_t measured_count,
	const struct options *options,
	int total)
{
	struct name_layout layout;
	struct name_layout measured_layout;
	int prepared;
	int printed;

	/* The names as they are written, and what the listing shares. */
	prepared = prepare_names(items, count, options, &layout);
	if (!prepared) {
		fprintf(stderr, "ls: out of memory\n");
		return 0;
	}

	/* What the entries measured with them add. */
	prepared = prepare_names(measured_too, measured_count, options, &measured_layout);
	if (!prepared) {
		fprintf(stderr, "ls: out of memory\n");
		return 0;
	}

	/* Their serial numbers and quotes count as the group's own. */
	if (measured_layout.inode_width > layout.inode_width)
		layout.inode_width = measured_layout.inode_width;
	if (measured_layout.some_quoted)
		layout.some_quoted = 1;

	/* The long format, which also writes a total for an empty directory. */
	if (options->format == LS_FORMAT_LONG) {
		printed = print_long_entries(path, items, count, measured_too, measured_count, options, &layout, total);
		if (!printed)
			return 0;
		return 1;
	}

	/* -s: a directory's total first, in every format. */
	if (options->blocks && total)
		print_total(items, count, options);

	/* Every other format writes nothing at all for no names. */
	if (count == 0)
		return 1;

	/* Chooses the layout of the names. */
	switch (options->format) {
	case LS_FORMAT_COLUMNS:
		printed = print_columns(items, count, options, &layout, 1);
		break;
	case LS_FORMAT_ACROSS:
		printed = print_columns(items, count, options, &layout, 0);
		break;
	case LS_FORMAT_COMMAS:
		print_separated(items, count, options, &layout, ',');
		printed = 1;
		break;
	default:
		print_one_per_line(items, count, options, &layout);
		printed = 1;
		break;
	}

	/* Reports names that could not be laid out. */
	if (!printed) {
		fprintf(stderr, "ls: out of memory\n");
		return 0;
	}

	/* Succeeded. */
	return 1;
}

/*
 * Finds the text each entry is written as, and what the listing shares:
 * the digits of its widest serial number and whether any name is quoted.
 * Returns 1, or 0 without memory.
 */
static int
prepare_names(
	struct entry *items,
	size_t count,
	const struct options *options,
	struct name_layout *layout)
{
	char digits[24];
	const char *quoted_too;
	size_t length;
	size_t index;
	int written;

	/* -F marks names with characters that a name containing them must be quoted for. */
	quoted_too = NULL;
	if (options->classify)
		quoted_too = LS_CLASSIFY_QUOTED;

	/* Each entry's text, and the widest serial number and block count. */
	layout->inode_width = 0;
	layout->blocks_width = 0;
	layout->some_quoted = 0;
	for (index = 0; index < count; index++) {
		free(items[index].shown);
		items[index].shown = NULL;
		written = quote_name(items[index].name,
				     options,
				     quoted_too,
				     &items[index].shown,
				     &items[index].shown_width,
				     &items[index].quoted);
		if (!written)
			return 0;

		/* A quoted name makes the others line up behind a space. */
		if (items[index].quoted)
			layout->some_quoted = 1;

		/* The digits of the serial number, or ? without the status. */
		inode_text(&items[index], digits);
		length = strlen(digits);
		if (length > layout->inode_width)
			layout->inode_width = length;

		/* -s: the digits of the block count. */
		blocks_text(&items[index], options, digits);
		length = strlen(digits);
		if (length > layout->blocks_width)
			layout->blocks_width = length;
	}

	/* Succeeded: every name has its text. */
	return 1;
}

/*
 * Writes a group of entries in the long format, after the total of their
 * blocks when it is a directory's.  Returns 1, or 0 when an entry could
 * not be listed.
 */
static int
print_long_entries(
	const char *path,
	struct entry *items,
	size_t count,
	const struct entry *measured_too,
	size_t measured_count,
	const struct options *options,
	const struct name_layout *layout,
	int total)
{
	struct long_widths widths;
	size_t index;
	int printed;
	int ok;

	/* The widths of the columns. */
	memset(&widths, 0, sizeof(widths));
	measure_long(items, count, options, &widths);
	measure_long(measured_too, measured_count, options, &widths);

	/* The total of the blocks, for a directory. */
	if (total)
		print_total(items, count, options);

	/* Each entry. */
	ok = 1;
	for (index = 0; index < count; index++) {
		printed = print_long(path, &items[index], options, &widths, layout);
		if (!printed)
			ok = 0;
	}

	/* Reports an entry that could not be listed. */
	if (!ok)
		return 0;

	/* Succeeded. */
	return 1;
}

/* Writes one name to a line (-1). */
static void
print_one_per_line(
	const struct entry *items,
	size_t count,
	const struct options *options,
	const struct name_layout *layout)
{
	size_t index;

	/* Each name on its own line. */
	for (index = 0; index < count; index++) {
		print_name(&items[index], options, layout);
		putchar('\n');
	}
}

/*
 * Writes names in columns as GNU ls does, filled down each column (-C) or
 * across each row (-x).  Without a limit on the width they all go on one
 * line.  Returns 1, or 0 without memory.
 */
static int
print_columns(
	const struct entry *items,
	size_t count,
	const struct options *options,
	const struct name_layout *layout,
	int down)
{
	struct column_fit *fits;
	size_t columns;
	int fitted;

	/* No limit on the width: one line, the names two spaces apart. */
	if (options->line_width == 0) {
		print_separated(items, count, options, layout, ' ');
		return 1;
	}

	/* The most columns that fit, with the width of each. */
	fitted = fit_columns(items, count, options, layout, down, &fits, &columns);
	if (!fitted)
		return 0;

	/* The names in those columns. */
	if (down) {
		print_down(items, count, options, layout, &fits[columns - 1U], columns);
	} else {
		print_across(items, count, options, layout, &fits[columns - 1U], columns);
	}

	/* The measures are done with; the widths of every candidate are one block. */
	free(fits[0].widths);
	free(fits);

	/* Succeeded. */
	return 1;
}

/*
 * Finds how many columns the names fit in, as GNU ls finds it: every
 * number of columns up to the most that could fit is measured at once,
 * each column as wide as its longest name and the two spaces after it
 * (none after the last), and the largest number whose line stays shorter
 * than the width wins.  Returns 1 with the measures (the caller frees
 * result[0].widths and result), or 0 without memory.
 */
static int
fit_columns(
	const struct entry *items,
	size_t count,
	const struct options *options,
	const struct name_layout *layout,
	int down,
	struct column_fit **result,
	size_t *result_columns)
{
	struct column_fit *fits;
	size_t *widths;
	size_t most;
	size_t candidates;
	size_t slots;
	size_t index;
	size_t candidate;
	size_t slot;
	size_t length;
	size_t needed;
	size_t columns;

	/* No more columns than the narrowest ones fill, nor than there are names. */
	most = options->line_width / LS_MIN_COLUMN_WIDTH;
	if (options->line_width % LS_MIN_COLUMN_WIDTH != 0)
		most++;
	candidates = count;
	if (most > 0 && most < count)
		candidates = most;

	/* One measure for each number of columns, 1 to candidates. */
	fits = calloc(candidates, sizeof(*fits));
	if (fits == NULL)
		return 0;

	/* The widths of all the candidates: 1 + 2 + ... + candidates of them. */
	if (candidates > ((size_t)-1 / sizeof(*widths) - 1U) / candidates) {
		free(fits);
		return 0;
	}

	/* One block for them, which the first candidate's widths point to. */
	slots = candidates * (candidates + 1U) / 2U;
	widths = malloc(slots * sizeof(*widths));
	if (widths == NULL) {
		free(fits);
		return 0;
	}

	/* Each candidate starts with every column at the narrowest. */
	slots = 0;
	for (candidate = 0; candidate < candidates; candidate++) {
		fits[candidate].fits = 1;
		fits[candidate].line_length = (candidate + 1U) * LS_MIN_COLUMN_WIDTH;
		fits[candidate].widths = widths + slots;
		for (slot = 0; slot <= candidate; slot++)
			fits[candidate].widths[slot] = LS_MIN_COLUMN_WIDTH;
		slots += candidate + 1U;
	}

	/* Each name widens its column in every candidate that still fits. */
	for (index = 0; index < count; index++) {
		length = name_length(&items[index], options, layout);
		for (candidate = 0; candidate < candidates; candidate++) {
			if (!fits[candidate].fits)
				continue;

			/* The column the name falls in with candidate + 1 columns. */
			if (down) {
				slot = index / ((count + candidate) / (candidate + 1U));
			} else {
				slot = index % (candidate + 1U);
			}

			/* Two spaces follow a name in every column but the last. */
			needed = length;
			if (slot != candidate)
				needed += 2U;

			/* A wider column lengthens the line, which may no longer fit. */
			if (fits[candidate].widths[slot] < needed) {
				fits[candidate].line_length += needed - fits[candidate].widths[slot];
				fits[candidate].widths[slot] = needed;
				fits[candidate].fits = 0;
				if (fits[candidate].line_length < options->line_width)
					fits[candidate].fits = 1;
			}
		}
	}

	/* The most columns that still fit; one always does. */
	columns = candidates;
	while (columns > 1U && !fits[columns - 1U].fits)
		columns--;

	/* Succeeded: the measures and the number of columns. */
	*result = fits;
	*result_columns = columns;
	return 1;
}

/* Writes names in columns filled down each column first (-C). */
static void
print_down(
	const struct entry *items,
	size_t count,
	const struct options *options,
	const struct name_layout *layout,
	const struct column_fit *fit,
	size_t columns)
{
	size_t rows;
	size_t row;
	size_t column;
	size_t index;
	size_t position;
	size_t length;
	size_t width;

	/* As many rows as the columns need. */
	rows = count / columns;
	if (count % columns != 0)
		rows++;

	/* Each row takes every rows-th name, starting with its own. */
	for (row = 0; row < rows; row++) {
		column = 0;
		index = row;
		position = 0;
		for (;;) {
			/* The name, in a column as wide as the widest in it. */
			length = name_length(&items[index], options, layout);
			width = fit->widths[column];
			column++;
			print_name(&items[index], options, layout);

			/* The last name of the row has nothing after it. */
			index += rows;
			if (index >= count)
				break;

			/* Blanks to the next column. */
			indent(position + length, position + width, options);
			position += width;
		}

		/* The row ends. */
		putchar('\n');
	}
}

/* Writes names in columns filled across each row first (-x). */
static void
print_across(
	const struct entry *items,
	size_t count,
	const struct options *options,
	const struct name_layout *layout,
	const struct column_fit *fit,
	size_t columns)
{
	size_t index;
	size_t column;
	size_t position;
	size_t length;
	size_t width;

	/* The first name. */
	print_name(&items[0], options, layout);
	length = name_length(&items[0], options, layout);
	width = fit->widths[0];
	position = 0;

	/* Each other name: on a new line at the first column, or after blanks to its column. */
	for (index = 1; index < count; index++) {
		column = index % columns;
		if (column == 0) {
			putchar('\n');
			position = 0;
		} else {
			indent(position + length, position + width, options);
			position += width;
		}

		/* The name, and the width of the column it is in. */
		print_name(&items[index], options, layout);
		length = name_length(&items[index], options, layout);
		width = fit->widths[column];
	}

	/* The last row ends. */
	putchar('\n');
}

/*
 * Writes names separated by a character and a space (-m with a comma, and
 * -C or -x with a space when there is no limit on the width), starting a
 * new line when the next name would reach the width.
 */
static void
print_separated(
	const struct entry *items,
	size_t count,
	const struct options *options,
	const struct name_layout *layout,
	char separator)
{
	size_t position;
	size_t length;
	size_t index;
	int fits;

	/* Each name, after the separator from the one before it. */
	position = 0;
	for (index = 0; index < count; index++) {
		length = 0;
		if (options->line_width != 0)
			length = name_length(&items[index], options, layout);

		/* The name stays on the line when it ends before the width. */
		if (index != 0) {
			fits = 0;
			if (options->line_width == 0) {
				fits = 1;
			} else if (position + length + 2U < options->line_width &&
				   position <= (size_t)-1 - length - 2U) {
				/* The sum is also checked for wrapping, as GNU ls checks it. */
				fits = 1;
			}

			/* The separator, then a space or a new line. */
			putchar(separator);
			if (fits) {
				putchar(' ');
				position += 2U;
			} else {
				putchar('\n');
				position = 0;
			}
		}

		/* The name. */
		print_name(&items[index], options, layout);
		position += length;
	}

	/* The line ends. */
	putchar('\n');
}

/*
 * Writes blanks from one column of the line to another: a tab wherever it
 * reaches a tab stop no further than the target, and spaces otherwise.
 */
static void
indent(
	size_t from,
	size_t to,
	const struct options *options)
{
	size_t tab;

	/* Each step toward the target. */
	tab = options->tab_size;
	while (from < to) {
		if (tab != 0 && to / tab > (from + 1U) / tab) {
			/* A tab to the next stop. */
			putchar('\t');
			from += tab - from % tab;
		} else {
			/* A space. */
			putchar(' ');
			from++;
		}
	}
}

/*
 * Returns the columns a name takes with what goes with it: the serial
 * number and a space (-i), the space that lines it up with quoted names,
 * and the mark of its type (-F).
 */
static size_t
name_length(
	const struct entry *item,
	const struct options *options,
	const struct name_layout *layout)
{
	char digits[24];
	size_t length;
	int padded;
	char mark;

	/* -i: the serial number, as wide as the widest except with -m, and a space. */
	length = 0;
	if (options->inode) {
		if (options->format == LS_FORMAT_COMMAS) {
			inode_text(item, digits);
			length += strlen(digits) + 1U;
		} else {
			length += layout->inode_width + 1U;
		}
	}

	/* -s: the block count, as wide as the widest except with -m, and a space. */
	if (options->blocks) {
		if (options->format == LS_FORMAT_COMMAS) {
			blocks_text(item, options, digits);
			length += strlen(digits) + 1U;
		} else {
			length += layout->blocks_width + 1U;
		}
	}

	/* The name as written, with the space that lines it up. */
	length += item->shown_width;
	padded = name_padded(item, options, layout);
	if (padded)
		length++;

	/* -F or -p: the mark, when the type has one. */
	mark = entry_mark(item, options);
	if (mark != '\0')
		length++;

	/* Succeeded: the columns. */
	return length;
}

/*
 * Writes a name with what goes with it: the serial number (-i), the space
 * that lines it up with quoted names, and the mark of its type (-F).
 */
static void
print_name(
	const struct entry *item,
	const struct options *options,
	const struct name_layout *layout)
{
	char digits[24];
	int width;
	int padded;
	char mark;

	/* -i: the serial number, right-aligned except with -m. */
	if (options->inode) {
		inode_text(item, digits);
		width = (int)layout->inode_width;
		if (options->format == LS_FORMAT_COMMAS)
			width = 0;
		printf("%*s ", width, digits);
	}

	/* -s: the block count, right-aligned except with -m. */
	if (options->blocks) {
		blocks_text(item, options, digits);
		width = (int)layout->blocks_width;
		if (options->format == LS_FORMAT_COMMAS)
			width = 0;
		printf("%*s ", width, digits);
	}

	/* The name, after a space when others in the listing are quoted. */
	padded = name_padded(item, options, layout);
	if (padded)
		putchar(' ');
	fputs(item->shown, stdout);

	/* -F or -p: the mark, when the type has one. */
	mark = entry_mark(item, options);
	if (mark != '\0')
		putchar(mark);
}

/* Returns 1 when a name gets a space before it to line up with the quoted names of its listing. */
static int
name_padded(
	const struct entry *item,
	const struct options *options,
	const struct name_layout *layout)
{
	/* Only a name without quotes, in a lined-up format, among quoted ones. */
	if (!options->align_quotes)
		return 0;
	if (!layout->some_quoted)
		return 0;
	if (item->quoted)
		return 0;

	/* Succeeded: it is padded. */
	return 1;
}

/* Writes the serial number of an entry, or ? when its status is unknown. */
static void
inode_text(
	const struct entry *item,
	char out[24])
{
	/* The number, when the status was read. */
	if (item->status_valid) {
		snprintf(out, 24, "%lu", (unsigned long)item->status.st_ino);
		return;
	}

	/* No status, no number. */
	snprintf(out, 24, "?");
}

/* Writes the header of a directory's listing: its name, quoted when it has a colon too, and a colon. */
static void
print_header(
	const char *path,
	const struct options *options)
{
	char *shown;
	size_t width;
	int quoted;
	int written;

	/* The name as written; without memory, as it is. */
	shown = NULL;
	written = quote_name(path, options, LS_HEADER_QUOTED, &shown, &width, &quoted);
	if (!written) {
		printf("%s:\n", path);
		return;
	}

	/* The header line. */
	printf("%s:\n", shown);
	free(shown);
}

/*
 * Finds the text a name is written as and the columns it takes: quoted as
 * a shell needs it, with ? for what cannot be shown, or as it is.
 * quoted_too lists characters that make a shell-quoted name need quotes
 * even though the shell would not.  Returns 1 with an allocated text the
 * caller frees, or 0 without memory.
 */
static int
quote_name(
	const char *name,
	const struct options *options,
	const char *quoted_too,
	char **shown,
	size_t *width,
	int *quoted)
{
	struct text_writer writer;
	size_t length;

	/* A name too long to reserve room for is refused like a failed allocation. */
	length = strlen(name);
	if (length > ((size_t)-1 - 8U) / 8U)
		return 0;

	/* Room for the longest text: a name of escapes is 7 bytes for each of its bytes. */
	writer.text = malloc(length * 8U + 8U);
	if (writer.text == NULL)
		return 0;
	writer.length = 0;

	/* Chooses how the name is written. */
	*quoted = 0;
	if (options->quoting == LS_QUOTING_SHELL) {
		/* Quoted for a shell, which leaves only printable characters to measure. */
		*quoted = quote_shell(name, length, quoted_too, &writer);
		*width = display_width(writer.text, writer.length);
	} else if (options->hide_control) {
		/* As it is, with ? for what cannot be shown. */
		hide_unprintable(name, length, &writer, width);
	} else {
		/* As it is. */
		write_bytes(&writer, name, length);
		*width = display_width(name, length);
	}

	/* Succeeded: the text, ended. */
	writer.text[writer.length] = '\0';
	*shown = writer.text;
	return 1;
}

/*
 * Writes a name in the shell-escape style of GNU ls: as it is when a
 * shell would read it as it is, and otherwise in quotes.  Returns 1 when
 * the name was quoted.
 */
static int
quote_shell(
	const char *name,
	size_t length,
	const char *quoted_too,
	struct text_writer *writer)
{
	size_t at;
	size_t bytes;
	int class;
	int needs_quotes;
	int double_quotes;
	int apostrophe;
	int printable;
	const char *listed;

	/* What each character asks: quotes, and whether double quotes could hold it. */
	needs_quotes = 0;
	double_quotes = 1;
	apostrophe = 0;
	at = 0;
	if (length == 0)
		needs_quotes = 1;
	while (at < length) {
		class = shell_class(name, length, at);
		bytes = 1;

		/* Chooses what the character asks of the quoting. */
		switch (class) {
		case LS_SHELL_PLAIN:
			break;
		case LS_SHELL_SPACE:
		case LS_SHELL_LEADING:
		case LS_SHELL_ALONE:
			/* Quotes, which may be double quotes. */
			needs_quotes = 1;
			break;
		case LS_SHELL_BARE:
			/* No quotes, but GNU ls does not use double quotes for a name with it. */
			double_quotes = 0;
			break;
		case LS_SHELL_APOSTROPHE:
			needs_quotes = 1;
			apostrophe = 1;
			break;
		case LS_SHELL_OTHER:
			/* A character the locale cannot print must be escaped. */
			bytes = character_length(name, length, at, &printable);
			if (!printable) {
				needs_quotes = 1;
				double_quotes = 0;
			}

			/* A printable character asks nothing. */
			break;
		default:
			/* The shell acts on it, and double quotes would not keep it plain. */
			needs_quotes = 1;
			double_quotes = 0;
			break;
		}

		/* A character the caller lists needs quotes too. */
		listed = NULL;
		if (!needs_quotes &&
		    bytes == 1U &&
		    quoted_too != NULL &&
		    name[at] != '\0')
			listed = strchr(quoted_too, name[at]);
		if (listed != NULL)
			needs_quotes = 1;

		/* The next character. */
		at += bytes;
	}

	/* A name a shell reads as it is. */
	if (!needs_quotes) {
		write_bytes(writer, name, length);
		return 0;
	}

	/* A name whose only trouble is an apostrophe reads best in double quotes. */
	if (apostrophe && double_quotes) {
		write_char(writer, '"');
		write_bytes(writer, name, length);
		write_char(writer, '"');
		return 1;
	}

	/* Succeeded: in single quotes, with escapes for what cannot be shown. */
	shell_escape_quote(name, length, writer);
	return 1;
}

/*
 * Returns the ls_shell_class of the character at a place in a name.  #
 * and ~ count only at the start, and { and } only as the whole name.
 */
static int
shell_class(
	const char *name,
	size_t length,
	size_t at)
{
	unsigned char byte;
	int alphanumeric;

	/* Letters and digits of ASCII, whatever the locale says. */
	byte = (unsigned char)name[at];
	alphanumeric = 0;
	if (byte >= 'a' && byte <= 'z') {
		alphanumeric = 1;
	} else if (byte >= 'A' && byte <= 'Z') {
		alphanumeric = 1;
	} else if (byte >= '0' && byte <= '9') {
		alphanumeric = 1;
	}

	/* They are plain. */
	if (alphanumeric)
		return LS_SHELL_PLAIN;

	/* Chooses the class of any other character. */
	switch (byte) {
	case '%':
	case '+':
	case ',':
	case '-':
	case '.':
	case '/':
	case ':':
	case ']':
	case '_':
		return LS_SHELL_PLAIN;
	case ' ':
		return LS_SHELL_SPACE;
	case '\'':
		return LS_SHELL_APOSTROPHE;
	case '#':
	case '~':
		/* Only a word's first character starts a comment or a home directory. */
		if (at == 0)
			return LS_SHELL_LEADING;
		return LS_SHELL_BARE;
	case '{':
	case '}':
		/* Only a word of just the brace is a brace group. */
		if (length == 1U)
			return LS_SHELL_ALONE;
		return LS_SHELL_BARE;
	case '!':
	case '"':
	case '$':
	case '&':
	case '(':
	case ')':
	case '*':
	case ';':
	case '<':
	case '=':
	case '>':
	case '?':
	case '[':
	case '\\':
	case '^':
	case '`':
	case '|':
		return LS_SHELL_SPECIAL;
	case '\a':
	case '\b':
	case '\f':
	case '\n':
	case '\r':
	case '\t':
	case '\v':
		return LS_SHELL_CONTROL;
	default:
		break;
	}

	/* Anything else is printable or not by the locale. */
	return LS_SHELL_OTHER;
}

/*
 * Writes a name in single quotes.  An apostrophe is written '\'', and a
 * run of characters that cannot be shown leaves the quotes for $'...'
 * with a C escape or three octal digits for each byte.
 */
static void
shell_escape_quote(
	const char *name,
	size_t length,
	struct text_writer *writer)
{
	static const char control_letters[] = "abtnvfr";
	char digits[5];
	size_t at;
	size_t bytes;
	size_t index;
	int class;
	int printable;
	int escaping;
	unsigned char byte;

	/* Each character, inside the opening quote. */
	write_char(writer, '\'');
	escaping = 0;
	at = 0;
	while (at < length) {
		class = shell_class(name, length, at);
		byte = (unsigned char)name[at];
		bytes = 1;
		printable = 1;
		if (class == LS_SHELL_OTHER)
			bytes = character_length(name, length, at, &printable);

		/* Chooses how the character is written. */
		if (class == LS_SHELL_CONTROL) {
			/* A control character with a C escape: \a is 7, the rest follow it in order. */
			if (!escaping)
				write_bytes(writer, "'$'", 3);
			escaping = 1;
			write_char(writer, '\\');
			write_char(writer, control_letters[byte - '\a']);
		} else if (class == LS_SHELL_APOSTROPHE) {
			/* An apostrophe closes any quotes, is escaped, and opens plain quotes again. */
			write_bytes(writer, "'\\''", 4);
			escaping = 0;
		} else if (!printable) {
			/* Each byte of what cannot be shown as three octal digits. */
			if (!escaping)
				write_bytes(writer, "'$'", 3);
			escaping = 1;
			for (index = 0; index < bytes; index++) {
				byte = (unsigned char)name[at + index];
				snprintf(digits, sizeof(digits), "\\%03o", (unsigned)byte);
				write_bytes(writer, digits, 4);
			}
		} else {
			/* A printable character, after leaving $'...' for plain quotes. */
			if (escaping)
				write_bytes(writer, "''", 2);
			escaping = 0;
			write_bytes(writer, name + at, bytes);
		}

		/* The next character. */
		at += bytes;
	}

	/* The closing quote, of whichever quotes are open. */
	write_char(writer, '\'');
}

/*
 * Writes a name as it is, with ? for each character the locale cannot
 * show (-q, and a terminal with -N), and finds the columns it takes.
 */
static void
hide_unprintable(
	const char *name,
	size_t length,
	struct text_writer *writer,
	size_t *width)
{
	mbstate_t state;
	wchar_t character;
	size_t at;
	size_t bytes;
	size_t multibyte;
	int columns;
	unsigned char byte;
	int printable;

	/* In a locale of single bytes, each byte is printable or a ?. */
	multibyte = MB_CUR_MAX;
	if (multibyte == 1U) {
		for (at = 0; at < length; at++) {
			byte = (unsigned char)name[at];
			printable = isprint(byte);
			if (printable) {
				write_char(writer, (char)byte);
			} else {
				write_char(writer, '?');
			}
		}

		/* Every byte takes a column. */
		*width = length;
		return;
	}

	/* Otherwise each character, which may be several bytes. */
	*width = 0;
	at = 0;
	while (at < length) {
		/* Printable ASCII is itself. */
		byte = (unsigned char)name[at];
		if (byte >= ' ' && byte <= '~') {
			write_char(writer, (char)byte);
			*width += 1U;
			at++;
			continue;
		}

		/* Anything else is decoded by the locale. */
		memset(&state, 0, sizeof(state));
		bytes = mbrtowc(&character, name + at, length - at, &state);
		if (bytes == (size_t)-1) {
			/* A byte that starts no character is a ?. */
			write_char(writer, '?');
			*width += 1U;
			at++;
			continue;
		}

		/* A character cut off by the end of the name is a ?. */
		if (bytes == (size_t)-2) {
			write_char(writer, '?');
			*width += 1U;
			break;
		}

		/* A null character, which a name cannot hold, would still be one byte. */
		if (bytes == 0)
			bytes = 1;

		/* A character with a width is itself, and any other a ?. */
		columns = wcwidth(character);
		if (columns >= 0) {
			write_bytes(writer, name + at, bytes);
			*width += (size_t)columns;
		} else {
			write_char(writer, '?');
			*width += 1U;
		}

		/* The next character. */
		at += bytes;
	}
}

/*
 * Returns the number of bytes of the character at a place in a text, and
 * whether the locale can print it.  A byte that starts no character is a
 * character of its own that cannot be printed; a character cut off by the
 * end of the text takes the rest of it.
 */
static size_t
character_length(
	const char *text,
	size_t length,
	size_t at,
	int *printable)
{
	mbstate_t state;
	wchar_t character;
	size_t multibyte;
	size_t bytes;
	int printing;

	/* In a locale of single bytes, the byte is the character. */
	multibyte = MB_CUR_MAX;
	if (multibyte == 1U) {
		printing = isprint((unsigned char)text[at]);
		*printable = 0;
		if (printing)
			*printable = 1;
		return 1;
	}

	/* Otherwise the locale decodes it. */
	memset(&state, 0, sizeof(state));
	bytes = mbrtowc(&character, text + at, length - at, &state);
	if (bytes == (size_t)-1) {
		*printable = 0;
		return 1;
	}

	/* A character cut off by the end takes the rest of the text. */
	if (bytes == (size_t)-2) {
		*printable = 0;
		return length - at;
	}

	/* A null character, which a name cannot hold, would still be one byte. */
	if (bytes == 0)
		bytes = 1;

	/* Succeeded: the character's bytes, and whether it prints. */
	printing = iswprint((wint_t)character);
	*printable = 0;
	if (printing)
		*printable = 1;
	return bytes;
}

/*
 * Returns the columns of a terminal a text takes, as GNU ls counts them: a
 * character by its width.  A text with a character that cannot be shown
 * or a byte that starts no character has no width GNU ls can count, and
 * it takes (size_t)-1, which GNU ls then adds up as it is: such a name
 * ends its line with -m and leaves no blanks after it with -C.  In a
 * locale of single bytes each printable byte takes a column.
 */
static size_t
display_width(
	const char *text,
	size_t length)
{
	mbstate_t state;
	wchar_t character;
	size_t multibyte;
	size_t width;
	size_t at;
	size_t bytes;
	int columns;
	int printing;

	/* In a locale of single bytes, each printable byte takes a column. */
	width = 0;
	multibyte = MB_CUR_MAX;
	if (multibyte == 1U) {
		for (at = 0; at < length; at++) {
			printing = isprint((unsigned char)text[at]);
			if (printing)
				width++;
		}

		/* The printable bytes. */
		return width;
	}

	/* Otherwise each character the locale decodes. */
	at = 0;
	while (at < length) {
		memset(&state, 0, sizeof(state));
		bytes = mbrtowc(&character, text + at, length - at, &state);
		if (bytes == (size_t)-1 || bytes == (size_t)-2) {
			/* A byte that starts no character, or a character cut off by the end. */
			return (size_t)-1;
		}

		/* A null character, which a name cannot hold, would still be one byte. */
		if (bytes == 0)
			bytes = 1;

		/* A character by its width; one without a width cannot be shown. */
		columns = wcwidth(character);
		if (columns < 0)
			return (size_t)-1;

		/* The character's columns, and the next character. */
		width += (size_t)columns;
		at += bytes;
	}

	/* Succeeded: the columns. */
	return width;
}

/* Appends bytes to a text that has room for them. */
static void
write_bytes(
	struct text_writer *writer,
	const char *bytes,
	size_t length)
{
	/* The bytes after what is there. */
	memcpy(writer->text + writer->length, bytes, length);
	writer->length += length;
}

/* Appends one character to a text that has room for it. */
static void
write_char(
	struct text_writer *writer,
	char character)
{
	/* The character after what is there. */
	writer->text[writer->length] = character;
	writer->length++;
}

/* Widens the columns of the long format to what a group of entries needs. */
static void
measure_long(
	const struct entry *items,
	size_t count,
	const struct options *options,
	struct long_widths *widths)
{
	char links[24];
	char size[32];
	char user_buffer[24];
	char group_buffer[24];
	const char *user;
	const char *group;
	size_t length;
	size_t index;

	/* Each entry whose status is known widens the columns it needs to. */
	for (index = 0; index < count; index++) {
		if (!items[index].status_valid)
			continue;

		/* The texts of the columns. */
		snprintf(links, sizeof(links), "%lu", (unsigned long)items[index].status.st_nlink);
		if (options->human) {
			human_size(items[index].status.st_size, size);
		} else {
			snprintf(size, sizeof(size), "%lld", (long long)items[index].status.st_size);
		}

		/* The owner and the group by name. */
		user = uid_name(items[index].status.st_uid, options->numeric, user_buffer);
		group = gid_name(items[index].status.st_gid, options->numeric, group_buffer);

		/* The link count. */
		length = strlen(links);
		if (length > widths->links)
			widths->links = length;

		/* The owner. */
		length = strlen(user);
		if (length > widths->user)
			widths->user = length;

		/* The group. */
		length = strlen(group);
		if (length > widths->group)
			widths->group = length;

		/* The size. */
		length = strlen(size);
		if (length > widths->size)
			widths->size = length;
	}
}

/*
 * Writes a size with -h: bytes below 1024, and otherwise the largest unit
 * that keeps it at least 1, with one decimal below 10 (1.5K, 12M).
 */
static void
human_size(
	off_t value,
	char out[16])
{
	static const char suffixes[] = "BKMGTPE";
	unsigned long long magnitude;
	unsigned long long scale;
	unsigned long long whole;
	unsigned long long remainder;
	unsigned long long tenth;
	unsigned unit;

	/* A negative size is written as it is. */
	if (value < 0) {
		snprintf(out, 16, "%lld", (long long)value);
		return;
	}

	/* The largest unit the size reaches. */
	magnitude = (unsigned long long)value;
	unit = 0;
	scale = 1;
	while (unit + 1U < sizeof(suffixes) - 1U && magnitude >= scale * 1024ULL) {
		scale *= 1024ULL;
		unit++;
	}

	/* Bytes. */
	if (unit == 0) {
		snprintf(out, 16, "%llu", magnitude);
		return;
	}

	/* One rounded decimal below 10, and a rounded whole number from 10 on. */
	whole = magnitude / scale;
	remainder = magnitude % scale;
	if (whole < 10U) {
		tenth = (remainder * 10ULL + scale / 2ULL) / scale;
		if (tenth == 10U) {
			whole++;
			tenth = 0;
		}

		/* The whole number, the decimal and the unit. */
		snprintf(out, 16, "%llu.%llu%c", whole, tenth, suffixes[unit]);
	} else {
		whole = (magnitude + scale / 2ULL) / scale;
		snprintf(out, 16, "%llu%c", whole, suffixes[unit]);
	}
}

/* Returns the name of a user, or the number when it has no name or numeric (-n) asks for it. */
static const char *
uid_name(
	uid_t id,
	int numeric,
	char out[24])
{
	struct passwd record;
	struct passwd *found;
	char buffer[512];
	int error;

	/* -n: the number. */
	if (numeric) {
		snprintf(out, 24, "%u", (unsigned)id);
		return out;
	}

	/* The name from the user database. */
	found = NULL;
	error = getpwuid_r(id, &record, buffer, sizeof(buffer), &found);
	if (error == 0 &&
	    found != NULL &&
	    found->pw_name != NULL) {
		snprintf(out, 24, "%s", found->pw_name);
		return out;
	}

	/* Succeeded: the number. */
	snprintf(out, 24, "%u", (unsigned)id);
	return out;
}

/* Returns the name of a group, or the number when it has no name or numeric (-n) asks for it. */
static const char *
gid_name(
	gid_t id,
	int numeric,
	char out[24])
{
	struct group record;
	struct group *found;
	char buffer[512];
	int error;

	/* -n: the number. */
	if (numeric) {
		snprintf(out, 24, "%u", (unsigned)id);
		return out;
	}

	/* The name from the group database. */
	found = NULL;
	error = getgrgid_r(id, &record, buffer, sizeof(buffer), &found);
	if (error == 0 &&
	    found != NULL &&
	    found->gr_name != NULL) {
		snprintf(out, 24, "%s", found->gr_name);
		return out;
	}

	/* Succeeded: the number. */
	snprintf(out, 24, "%u", (unsigned)id);
	return out;
}

/*
 * Writes one entry in the long format: mode, links, owner, group, size,
 * time and name, and where a symbolic link points.  Returns 1, or 0 after
 * a message when the status is unknown.
 */
static int
print_long(
	const char *directory,
	const struct entry *item,
	const struct options *options,
	const struct long_widths *widths,
	const struct name_layout *layout)
{
	char path[LS_PATH_CAPACITY];
	char digits[24];
	char mode[11];
	char size[32];
	char when[32];
	char user_buffer[24];
	char group_buffer[24];
	const char *user;
	const char *group;
	struct timespec shown_time;
	int joined;
	int link;
	int padded;
	char mark;

	/* Nothing is known of an entry without its status. */
	if (!item->status_valid) {
		command_error("ls", item->name);
		return 0;
	}

	/* The texts of the columns. */
	mode_text(item->status.st_mode, mode);
	if (options->human) {
		human_size(item->status.st_size, size);
	} else {
		snprintf(size, sizeof(size), "%lld", (long long)item->status.st_size);
	}

	/* The time (-c, -u), and the owner and the group by name. */
	shown_time = entry_time(item, options);
	ls_time(shown_time.tv_sec, when);
	user = uid_name(item->status.st_uid, options->numeric, user_buffer);
	group = gid_name(item->status.st_gid, options->numeric, group_buffer);

	/* -i: the file serial number first, as wide as the widest. */
	if (options->inode) {
		inode_text(item, digits);
		printf("%*s ", (int)layout->inode_width, digits);
	}

	/* -s: the block count, as wide as the widest. */
	if (options->blocks) {
		blocks_text(item, options, digits);
		printf("%*s ", (int)layout->blocks_width, digits);
	}

	/* The mode and the links, the owner (but -g) and the group (but -o), the size and the time. */
	printf("%s %*lu ", mode, (int)widths->links, (unsigned long)item->status.st_nlink);
	if (!options->no_owner)
		printf("%-*s ", (int)widths->user, user);
	if (!options->no_group)
		printf("%-*s ", (int)widths->group, group);
	printf("%*s %s ", (int)widths->size, size, when);

	/* The name, after a space when others in the listing are quoted. */
	padded = name_padded(item, options, layout);
	if (padded)
		putchar(' ');
	fputs(item->shown, stdout);

	/* A symbolic link: where it points, when that can be read. */
	link = S_ISLNK(item->status.st_mode);
	if (link) {
		joined = join_path(directory, item->name, path, sizeof(path));
		if (joined)
			print_link_target(path, options);
	} else {
		/* -F or -p: the mark of the type, when it has one. */
		mark = entry_mark(item, options);
		if (mark != '\0')
			putchar(mark);
	}

	/* Succeeded: the line ends. */
	putchar('\n');
	return 1;
}

/*
 * Writes " -> " and where a symbolic link points, quoted as a name is,
 * with -F the mark of the type of what it points to.  Nothing is written
 * when the link cannot be read.
 */
static void
print_link_target(
	const char *path,
	const struct options *options)
{
	char target[LS_PATH_CAPACITY];
	struct stat status;
	const char *quoted_too;
	char *shown;
	ssize_t length;
	size_t width;
	int quoted;
	int written;
	int error;
	char mark;

	/* The target, which may not be readable. */
	length = readlink(path, target, sizeof(target) - 1U);
	if (length < 0)
		return;
	target[length] = '\0';

	/* The arrow. */
	fputs(" -> ", stdout);

	/* The target as a name is written, with -F's marks quoted; without memory, as it is. */
	quoted_too = NULL;
	if (options->classify)
		quoted_too = LS_CLASSIFY_QUOTED;
	shown = NULL;
	written = quote_name(target, options, quoted_too, &shown, &width, &quoted);
	if (written) {
		fputs(shown, stdout);
		free(shown);
	} else {
		fputs(target, stdout);
	}

	/* -F: the mark of what the link points to, when it leads somewhere. */
	if (options->classify) {
		error = stat(path, &status);
		if (error == 0) {
			mark = type_mark(status.st_mode);
			if (mark != '\0')
				putchar(mark);
		}
	}
}

/* Writes the ten characters of a mode: the type, then rwx for each class with s and t. */
static void
mode_text(
	mode_t mode,
	char out[11])
{
	static const mode_t bits[] = {
		S_IRUSR, S_IWUSR, S_IXUSR,
		S_IRGRP, S_IWGRP, S_IXGRP,
		S_IROTH, S_IWOTH, S_IXOTH
	};
	static const char letters[] = "rwx";
	unsigned index;

	/* The type, then a letter or a dash for each permission. */
	out[0] = type_char(mode);
	for (index = 0; index < 9; index++) {
		out[index + 1] = '-';
		if ((mode & bits[index]) != 0)
			out[index + 1] = letters[index % 3];
	}

	/* Set-user-ID in the owner's execute place: s over x, S without it. */
	if ((mode & S_ISUID) != 0) {
		out[3] = 'S';
		if ((mode & S_IXUSR) != 0)
			out[3] = 's';
	}

	/* Set-group-ID in the group's execute place. */
	if ((mode & S_ISGID) != 0) {
		out[6] = 'S';
		if ((mode & S_IXGRP) != 0)
			out[6] = 's';
	}

	/* The sticky bit in the others' execute place: t over x, T without it. */
	if ((mode & S_ISVTX) != 0) {
		out[9] = 'T';
		if ((mode & S_IXOTH) != 0)
			out[9] = 't';
	}

	/* The text ends. */
	out[10] = '\0';
}

/* Returns the letter of the long format for the type of a file. */
static char
type_char(
	mode_t mode)
{
	/* A directory, a character or block device, a FIFO, a link, a socket. */
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return 'd';
	case S_IFCHR:
		return 'c';
	case S_IFBLK:
		return 'b';
	case S_IFIFO:
		return 'p';
	case S_IFLNK:
		return 'l';
	case S_IFSOCK:
		return 's';
	default:
		break;
	}

	/* A regular file. */
	return '-';
}

/*
 * Writes the time of the long format in UTC: month, day and hour:minute
 * for a time within the last six months (and the next hour), and month,
 * day and year for any other.
 */
static void
ls_time(
	time_t value,
	char out[32])
{
	static const char *const month_names[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	long long days;
	long long seconds;
	long long year;
	time_t now;
	int month;
	int length;
	int recent;

	/* The day since the epoch and the second in the day, the second never negative. */
	days = value / 86400;
	seconds = value % 86400;
	if (seconds < 0) {
		seconds += 86400;
		days--;
	}

	/* The year, counting whole years forward or back from 1970. */
	year = 1970;
	for (;;) {
		length = days_in_year(year);
		if (days < length)
			break;
		days -= length;
		year++;
	}
	while (days < 0) {
		year--;
		length = days_in_year(year);
		days += length;
	}

	/* The month, counting whole months into the year. */
	month = 0;
	while (month < 11) {
		length = days_in_month(month, year);
		if (days < length)
			break;
		days -= length;
		month++;
	}

	/* A time more than six months ago or an hour ahead is not recent; any time is when the clock is unknown. */
	now = time(NULL);
	recent = 1;
	if (now != (time_t)-1 &&
	    (value < now - LS_RECENT_SECONDS ||
	     value > now + 3600))
		recent = 0;

	/* A recent time shows the hour and minute, and any other the year. */
	if (recent) {
		snprintf(out, 32, "%s %2d %02lld:%02lld", month_names[month], (int)days + 1, seconds / 3600, (seconds / 60) % 60);
	} else {
		snprintf(out, 32, "%s %2d  %4lld", month_names[month], (int)days + 1, year);
	}
}

/* Returns the number of days of a year of the Gregorian calendar. */
static int
days_in_year(
	long long year)
{
	/* A year divisible by 400 is a leap year; by 100 otherwise not; by 4 otherwise it is. */
	if (year % 400 == 0)
		return 366;
	if (year % 100 == 0)
		return 365;
	if (year % 4 == 0)
		return 366;

	/* Any other year. */
	return 365;
}

/* Returns the number of days of a month (0 for January) in a year. */
static int
days_in_month(
	int month,
	long long year)
{
	static const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	int year_length;

	/* February has a 29th day in a leap year. */
	if (month == 1) {
		year_length = days_in_year(year);
		if (year_length == 366)
			return 29;
	}

	/* Any other month, or February in another year. */
	return lengths[month];
}

/*
 * Returns the -F mark for the type of a file, or 0 for none: / for a
 * directory, @ for a link, | for a FIFO, = for a socket, and * for a
 * regular file that someone may execute.
 */
static char
type_mark(
	mode_t mode)
{
	/* Chooses the mark by the type. */
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return '/';
	case S_IFLNK:
		return '@';
	case S_IFIFO:
		return '|';
	case S_IFSOCK:
		return '=';
	case S_IFREG:
		break;
	default:
		return '\0';
	}

	/* A regular file someone may execute. */
	if ((mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0)
		return '*';

	/* Any other regular file has none. */
	return '\0';
}

/* Frees entries, their names and the texts they were written as. */
static void
free_entries(
	struct entry *items,
	size_t count)
{
	size_t index;

	/* Each name and text, then the array. */
	for (index = 0; index < count; index++) {
		free(items[index].name);
		free(items[index].shown);
	}

	/* The array itself. */
	free(items);
}

/* The mark after a name: -F's for its type, or -p's slash after a directory; '\0' for none. */
static char
entry_mark(
	const struct entry *item,
	const struct options *options)
{
	int directory;

	/* Nothing is known without the status. */
	if (!item->status_valid)
		return '\0';

	/* -F: by the type. */
	if (options->classify)
		return type_mark(item->status.st_mode);

	/* -p: a directory's slash. */
	directory = S_ISDIR(item->status.st_mode);
	if (options->slash && directory)
		return '/';

	/* None. */
	return '\0';
}

/* Writes the blocks an entry takes (-s) in the units asked for, with -h as a size; ? without the status. */
static void
blocks_text(
	const struct entry *item,
	const struct options *options,
	char out[24])
{
	unsigned long long blocks;

	/* No status, no count. */
	if (!item->status_valid) {
		snprintf(out, 24, "?");
		return;
	}

	/* The count in blocks of 512 bytes, then as asked. */
	blocks = 0;
	if (item->status.st_blocks > 0)
		blocks = (unsigned long long)item->status.st_blocks;
	if (options->human) {
		human_size((off_t)(blocks * 512ULL), out);
		return;
	}

	/* The count. */
	snprintf(out, 24, "%llu", blocks_in_units(blocks, options));
}

/* Turns a count of 512-byte blocks into the units of the output: 512 bytes, or 1024 with -k (rounded up). */
static unsigned long long
blocks_in_units(
	unsigned long long blocks,
	const struct options *options)
{
	/* -k: kilobytes. */
	if (options->kilobytes)
		return (blocks + 1ULL) / 2ULL;

	/* XCU's unit. */
	return blocks;
}

/*
 * The time of an entry that -t sorts by and -l shows: modified, or with -c
 * changed, with -u accessed.  It is a copy: on a 32-bit ABI struct stat is
 * packed, so a pointer to its time need not be aligned for the time.
 */
static struct timespec
entry_time(
	const struct entry *item,
	const struct options *options)
{
	/* The field chosen. */
	if (options->time_field == LS_TIME_CHANGED)
		return item->status.st_ctim;
	if (options->time_field == LS_TIME_ACCESSED)
		return item->status.st_atim;

	/* Reports the modification time, the default. */
	return item->status.st_mtim;
}

/* Orders two times: less than, equal to or greater than 0 as the first is older, the same or newer. */
static int
compare_times(
	const struct timespec *left,
	const struct timespec *right)
{
	/* The seconds, then the nanoseconds. */
	if (left->tv_sec != right->tv_sec) {
		if (left->tv_sec < right->tv_sec)
			return -1;
		return 1;
	}

	/* The same second. */
	if (left->tv_nsec != right->tv_nsec) {
		if (left->tv_nsec < right->tv_nsec)
			return -1;
		return 1;
	}

	/* The same. */
	return 0;
}

/* Writes a directory's total of the blocks its entries take, in the units of the output (-k, -h). */
static void
print_total(
	const struct entry *items,
	size_t count,
	const struct options *options)
{
	unsigned long long blocks;
	char text[24];
	size_t index;

	/* The blocks of 512 bytes each entry takes. */
	blocks = 0;
	for (index = 0; index < count; index++) {
		if (items[index].status_valid && items[index].status.st_blocks > 0)
			blocks += (unsigned long long)items[index].status.st_blocks;
	}

	/* In the units asked for, or with -h as a size. */
	if (options->human)
		human_size((off_t)(blocks * 512ULL), text);
	else
		snprintf(text, sizeof(text), "%llu", blocks_in_units(blocks, options));
	printf("total %s\n", text);
}
