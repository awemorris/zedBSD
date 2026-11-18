/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Constructs argument lists and invokes a utility (POSIX XCU xargs).
 *
 *	xargs [-prtx] [-0|-E eofstr] [-I replstr|-L number|-n number]
 *	      [-s size] [utility [argument...]]
 *
 * Arguments are read from standard input.  Without -0 they are separated
 * by blanks and newlines; single quotes, double quotes and backslashes
 * quote, and an argument equal to the -E string ends the input.  With -0
 * each null byte ends one argument and no other byte is special.
 *
 * The utility (echo when none is named) runs as many times as the
 * arguments need: -n limits the arguments of one run, -L the input lines,
 * and -s the bytes of the command line, counted as the utility name and
 * each argument with its terminating null byte.  -I runs the utility once
 * for each input line, with the line put in place of the replacement
 * string inside the initial arguments.  -t writes each command line to
 * standard error first, -p also asks on /dev/tty, and -r runs nothing when
 * the input has no argument at all.
 *
 * A utility that exits with a status from 1 to 254 makes xargs exit with
 * 123 in the end; one that exits with 255 or is killed by a signal stops
 * xargs at once with 124 or 125.  A utility that cannot be found gives 127
 * and one that cannot be run 126.
 *
 * GNU's extensions are taken too (the user's decision for WS045,
 * 2026-09-27): -d delim (--delimiter) separates arguments by one
 * character (a character, or \n, \t, \\, \0, \xHH or \ooo) with no quoting,
 * as -0 (--null) does with the null byte, and neither has an end string;
 * -P n (--max-procs) runs up to n utilities at once (0: no limit); -a
 * file (--arg-file) reads the arguments from the file, leaving standard
 * input to the utility; -o (--open-tty) gives the utility /dev/tty as its
 * standard input; -e[eof], -i[replace] and -l[lines] are the old forms of
 * -E, -I and -L; and the long options --eof, --replace, --max-lines,
 * --max-args, --interactive, --no-run-if-empty, --max-chars, --verbose
 * and --exit.  The options end at the utility.
 */

#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "include/kern/os-name.h"

/* The largest command line xargs builds when -s does not ask for less. */
#define XARGS_DEFAULT_SIZE 131072

/* The room POSIX keeps free below ARG_MAX for the utility's own use. */
#define XARGS_ARG_MAX_RESERVE 2048

/* The status after a utility exited with a status from 1 to 254. */
#define XARGS_STATUS_UTILITY_FAILED 123

/* The status after a utility exited with 255. */
#define XARGS_STATUS_UTILITY_ABORTED 124

/* The status after a utility was killed by a signal. */
#define XARGS_STATUS_UTILITY_KILLED 125

/* The status when the utility was found but could not be run. */
#define XARGS_STATUS_NOT_RUNNABLE 126

/* The status when the utility could not be found. */
#define XARGS_STATUS_NOT_FOUND 127

/* The codes of the long options that have no letter. */
#define OPTION_HELP 256
#define OPTION_VERSION 257

/*
 * The options written in full, read by the scan of the command line; a
 * long option shares its code with its letter.
 */
static const struct command_long_option xargs_long_options[] = {
	{"arg-file", COMMAND_VALUE_REQUIRED, 'a'},
	{"delimiter", COMMAND_VALUE_REQUIRED, 'd'},
	{"eof", COMMAND_VALUE_OPTIONAL, 'e'},
	{"exit", COMMAND_VALUE_NONE, 'x'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"interactive", COMMAND_VALUE_NONE, 'p'},
	{"max-args", COMMAND_VALUE_REQUIRED, 'n'},
	{"max-chars", COMMAND_VALUE_REQUIRED, 's'},
	{"max-lines", COMMAND_VALUE_OPTIONAL, 'l'},
	{"max-procs", COMMAND_VALUE_REQUIRED, 'P'},
	{"no-run-if-empty", COMMAND_VALUE_NONE, 'r'},
	{"null", COMMAND_VALUE_NONE, '0'},
	{"open-tty", COMMAND_VALUE_NONE, 'o'},
	{"replace", COMMAND_VALUE_OPTIONAL, 'i'},
	{"verbose", COMMAND_VALUE_NONE, 't'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{NULL, 0, 0}
};

/* Where the arguments are read from: standard input, or -a's file. */
static FILE *xargs_input;

/*
 * The options given on the command line.
 *
 * One instance lives for the whole run.  A zero limit means that the
 * option was not given; max_size is always set, to -s or to the default.
 * delimiter is -d's character, or -1; null_separated is -0 (and -d).
 * max_procs is -P's count, LONG_MAX for no limit.
 * system_room is what the system leaves for one command line counted as
 * the kernel counts it, with a pointer for every string; it bounds the
 * command line whatever -s says.
 */
struct xargs_options {
	int null_separated;
	int delimiter;
	int argument_file;
	int open_tty;
	long max_procs;
	const char *eof_string;
	const char *replace_string;
	long max_lines;
	long max_arguments;
	size_t max_size;
	size_t system_room;
	int prompt;
	int trace;
	int exit_on_overflow;
	int skip_empty_input;
};

/*
 * One argument read from standard input.
 *
 * The text grows as bytes arrive and is reused for the next argument; the
 * command copies the text it keeps.  line_end says that the argument was
 * the last one of an input line, which is what -L counts.
 */
struct xargs_token {
	char *text;
	size_t length;
	size_t capacity;
	int line_end;
};

/*
 * The command line being built.
 *
 * The first base_count entries are the utility and its initial arguments;
 * they stay for every run.  The entries after them are the arguments read
 * from the input for the next run, each allocated and freed by the run.
 * size counts every entry with its null byte, as -s counts it, and
 * system_size also the pointer to each entry and the terminating null
 * pointer, as the kernel counts it.  lines counts the input lines the
 * arguments came from.
 */
struct xargs_command {
	char **argv;
	size_t count;
	size_t capacity;
	size_t base_count;
	size_t base_size;
	size_t size;
	size_t base_system_size;
	size_t system_size;
	long lines;
};

/*
 * What has happened over the whole run.
 *
 * status is the exit status xargs reports when it reaches the end of the
 * input; ran says whether the utility has been invoked at least once, so
 * that input without arguments still runs it once without -r.  answers is
 * /dev/tty for -p, opened on the first question.  running counts the
 * utilities started and not yet waited for (-P); name is the utility's.
 */
struct xargs_run {
	int status;
	int ran;
	FILE *answers;
	long running;
	const char *name;
};

static int read_options(int argc, char **argv, struct xargs_options *options);
static int parse_count(const char *text, long *count);
static int parse_delimiter(const char *text, int *delimiter);
static size_t largest_size(void);
static int read_token(const struct xargs_options *options, struct xargs_token *token);
static int read_quoted(struct xargs_token *token, int quote);
static int read_null_token(struct xargs_token *token, int delimiter);
static int read_line_token(struct xargs_token *token);
static int read_blank_token(struct xargs_token *token);
static int append_byte(struct xargs_token *token, int byte);
static int is_blank(int byte);
static int is_eof_string(const struct xargs_options *options, const struct xargs_token *token);
static int command_init(struct xargs_command *command, int argc, char **argv, int first);
static int command_append(struct xargs_command *command, const char *text, size_t length);
static void command_reset(struct xargs_command *command);
static int run_arguments(const struct xargs_options *options, struct xargs_command *command, struct xargs_run *run);
static int run_insert(const struct xargs_options *options, struct xargs_command *command, struct xargs_run *run);
static int run_command(const struct xargs_options *options, char **argv, struct xargs_run *run);
static int reap_child(struct xargs_run *run);
static int reap_all(struct xargs_run *run, int status);
static int confirm(const struct xargs_options *options, char **argv, struct xargs_run *run);
static int ask(struct xargs_run *run);
static char *replace_all(const char *template, const char *pattern, const char *replacement, size_t *size);
static void usage(void);

/*
 * Runs xargs.
 */
int
main(
	int argc,
	char **argv)
{
	struct xargs_options options;
	struct xargs_command command;
	struct xargs_run run;
	int first;
	int error;
	int status;

	/* Reads the options; the utility and its arguments are left from argv[1] on. */
	first = read_options(argc, argv, &options);

	/* Starts the command line with the utility and its initial arguments. */
	error = command_init(&command, first + 1, argv, 1);
	if (error != 0) {
		fprintf(stderr, "xargs: out of memory\n");
		return 1;
	}

	/* Refuses initial arguments that alone exceed the command line size. */
	if (command.base_size > options.max_size) {
		fprintf(stderr, "xargs: argument line too long\n");
		return 1;
	}

	/* Nothing has run and nothing has failed yet. */
	run.status = 0;
	run.ran = 0;
	run.answers = NULL;
	run.running = 0;
	run.name = command.argv[0];

	/* -I runs once per line; otherwise the arguments are gathered. */
	if (options.replace_string != NULL)
		status = run_insert(&options, &command, &run);
	else
		status = run_arguments(&options, &command, &run);

	/* Waits for the utilities still running (-P); an early stop wins. */
	status = reap_all(&run, status);
	if (status != 0)
		return status;

	/* Succeeded: reports whether any invocation of the utility failed. */
	return run.status;
}

/*
 * Reads the options and returns the number of operands (the utility and
 * its arguments), which are left in argv from argv[1] on.  An invalid
 * option ends xargs with a usage message.
 */
static int
read_options(
	int argc,
	char **argv,
	struct xargs_options *options)
{
	struct command_options scan;
	const char *file;
	size_t largest;
	long size;
	int option;
	int valid;
	int compare;

	/* No option given: blanks separate, no end string, no limit. */
	memset(options, 0, sizeof(*options));
	options->delimiter = -1;
	options->max_procs = 1;
	largest = largest_size();
	options->system_room = largest;
	options->max_size = XARGS_DEFAULT_SIZE;
	if (options->max_size > largest)
		options->max_size = largest;
	file = NULL;

	/* Takes each option; the utility operand ends them in any order. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "xargs";
	scan.letters = "0a:d:E:e::I:i::L:l::n:oP:prs:tx";
	scan.names = xargs_long_options;
	command_options_start(&scan);
	scan.permute = 0;
	for (;;) {
		option = command_options_next(&scan);
		if (option == COMMAND_OPTION_END)
			break;

		/* Records what the option asks for. */
		switch (option) {
		case '0':
			options->null_separated = 1;
			options->delimiter = '\0';
			break;
		case 'a':
			file = scan.value;
			break;
		case 'd':
			valid = parse_delimiter(scan.value, &options->delimiter);
			if (!valid) {
				fprintf(stderr, "xargs: invalid delimiter '%s'\n", scan.value);
				exit(1);
			}

			/* -d separates as -0 does. */
			options->null_separated = 1;
			break;
		case 'E':
			options->eof_string = scan.value;
			break;
		case 'e':
			/* The old form: no value turns the end string off. */
			options->eof_string = scan.value;
			if (scan.value == NULL)
				options->eof_string = "";
			break;
		case 'I':
		case 'i':
			/* -I runs once per line and implies -x; it replaces -L and -n. */
			options->replace_string = scan.value;
			if (scan.value == NULL)
				options->replace_string = "{}";
			options->exit_on_overflow = 1;
			options->max_lines = 0;
			options->max_arguments = 0;
			break;
		case 'L':
		case 'l':
			/* The last of -I, -L and -n given is the one that counts. */
			options->max_lines = 1;
			if (scan.value != NULL) {
				valid = parse_count(scan.value, &options->max_lines);
				if (!valid)
					usage();
			}

			/* -L replaces -I and -n. */
			options->replace_string = NULL;
			options->max_arguments = 0;
			break;
		case 'n':
			valid = parse_count(scan.value, &options->max_arguments);
			if (!valid)
				usage();
			options->replace_string = NULL;
			options->max_lines = 0;
			break;
		case 'o':
			options->open_tty = 1;
			break;
		case 'P':
			/* 0 is no limit. */
			compare = strcmp(scan.value, "0");
			options->max_procs = LONG_MAX;
			valid = 1;
			if (compare != 0)
				valid = parse_count(scan.value, &options->max_procs);
			if (!valid)
				usage();
			break;
		case 'p':
			/* Asking shows the command line, as -t does. */
			options->prompt = 1;
			options->trace = 1;
			break;
		case 'r':
			options->skip_empty_input = 1;
			break;
		case 's':
			/* A size larger than the system allows is lowered to it. */
			valid = parse_count(scan.value, &size);
			if (!valid)
				usage();
			options->max_size = (size_t)size;
			if (options->max_size > largest)
				options->max_size = largest;
			break;
		case 't':
			options->trace = 1;
			break;
		case 'x':
			options->exit_on_overflow = 1;
			break;
		case OPTION_VERSION:
			printf("xargs (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
			break;
		default:
			usage();
			break;
		}
	}

	/* An empty end string turns the end string off, as -0 and -d do. */
	if (options->eof_string != NULL && options->eof_string[0] == '\0')
		options->eof_string = NULL;
	if (options->null_separated)
		options->eof_string = NULL;

	/* -a: the arguments come from the file; standard input stays the utility's. */
	xargs_input = stdin;
	if (file != NULL) {
		xargs_input = fopen(file, "r");
		if (xargs_input == NULL) {
			fprintf(stderr, "xargs: %s: %s\n", file, strerror(errno));
			exit(1);
		}

		/* The utility keeps standard input. */
		options->argument_file = 1;
	}

	/* Reports how many operands there are. */
	return scan.operand_count;
}

/*
 * Reads -d's delimiter: one character, or an escape: \n, \t, \r, \f, \v,
 * \a, \b, \\, \xHH (hexadecimal) or \ooo (octal, \0 for the null byte).
 * Returns whether it was one.
 */
static int
parse_delimiter(
	const char *text,
	int *delimiter)
{
	static const char escapes[] = "n\nt\tr\rf\fv\va\ab\b\\\\";
	const char *found;
	char *end;
	unsigned long value;
	size_t index;

	/* One character. */
	if (text[0] != '\0' && text[1] == '\0') {
		*delimiter = (unsigned char)text[0];
		return 1;
	}

	/* Otherwise an escape. */
	if (text[0] != '\\')
		return 0;

	/* A letter escape: the letters are at the even places of the table. */
	if (text[1] != '\0' && text[2] == '\0') {
		for (index = 0; escapes[index] != '\0'; index += 2) {
			if (escapes[index] == text[1]) {
				*delimiter = (unsigned char)escapes[index + 1];
				return 1;
			}
		}
	}

	/* A number: \xHH in hexadecimal, or octal digits. */
	found = text + 1;
	if (text[1] == 'x') {
		found = text + 2;
		value = strtoul(found, &end, 16);
	} else {
		value = strtoul(found, &end, 8);
	}

	/* The digits, all of them, of a byte. */
	if (*found < '0' || *found > 'f' || end == found || *end != '\0' || value > 255)
		return 0;

	/* Succeeded: the byte. */
	*delimiter = (int)value;
	return 1;
}

/* Parses a positive decimal count; returns whether it was one. */
static int
parse_count(
	const char *text,
	long *count)
{
	char *end;
	long value;

	/* Converts the whole text; anything left over is not a count. */
	errno = 0;
	value = strtol(text, &end, 10);
	if (end == text || *end != '\0')
		return 0;

	/* Refuses a count out of range or not positive. */
	if (errno != 0)
		return 0;
	if (value <= 0)
		return 0;

	/* Succeeded: the count. */
	*count = value;
	return 1;
}

/*
 * Computes the largest command line the system accepts, which is ARG_MAX
 * less the reserve POSIX keeps free and less the environment the utility
 * inherits.  Strings are counted with a pointer each, because the kernel
 * counts the pointer tables against the same limit.
 */
static size_t
largest_size(void)
{
	extern char **environ;
	char **entry;
	size_t environment;
	long arg_max;
	size_t largest;

	/* The system limit, or the POSIX minimum when it is not known. */
	arg_max = sysconf(_SC_ARG_MAX);
	if (arg_max <= 0)
		arg_max = _POSIX_ARG_MAX;

	/* The environment takes its strings and pointers out of the same room. */
	environment = sizeof(char *);
	for (entry = environ; entry != NULL && *entry != NULL; entry++)
		environment += strlen(*entry) + 1 + sizeof(char *);

	/* Keeps at least LINE_MAX, which POSIX promises to -s. */
	largest = (size_t)arg_max;
	if (largest > XARGS_ARG_MAX_RESERVE + environment + LINE_MAX)
		largest -= XARGS_ARG_MAX_RESERVE + environment;
	else
		largest = LINE_MAX;

	/* Reports the size. */
	return largest;
}

/*
 * Reads the next argument.  Returns 1 for an argument, 0 at the end of the
 * input or at the end string, and -1 after a diagnosed error.
 */
static int
read_token(
	const struct xargs_options *options,
	struct xargs_token *token)
{
	int status;
	int matched;

	/* Starts an empty argument. */
	token->length = 0;
	token->line_end = 0;

	/* Reads it by the rules the options select. */
	if (options->null_separated)
		status = read_null_token(token, options->delimiter);
	else if (options->replace_string != NULL)
		status = read_line_token(token);
	else
		status = read_blank_token(token);

	/* Passes on the end of the input and errors. */
	if (status <= 0)
		return status;

	/* The end string ends the input as the end of the file does. */
	matched = is_eof_string(options, token);
	if (matched)
		return 0;

	/* Succeeded: one argument. */
	return 1;
}

/*
 * Reads one argument ended by the delimiter (the null byte for -0).  Every
 * delimiter ends one, so two in a row give an empty argument; bytes after
 * the last delimiter are the last argument.
 */
static int
read_null_token(
	struct xargs_token *token,
	int delimiter)
{
	int byte;
	int started;
	int error;

	/* Collects the bytes up to the null byte. */
	started = 0;
	for (;;) {
		byte = getc(xargs_input);
		if (byte == EOF)
			break;
		started = 1;

		/* The delimiter ends the argument and its line. */
		if (byte == delimiter) {
			token->line_end = 1;
			break;
		}

		/* Keeps any other byte as it is. */
		error = append_byte(token, byte);
		if (error != 0)
			return -1;
	}

	/* Reports the end of the input when no byte came. */
	if (!started)
		return 0;

	/* Succeeded: an argument, ended by the end of the file if not by null. */
	token->line_end = 1;
	return 1;
}

/*
 * Reads one line as one argument for -I.  Leading blanks are skipped and
 * lines with nothing else are ignored; quotes and backslashes still quote,
 * and blanks inside the line are kept.
 */
static int
read_line_token(
	struct xargs_token *token)
{
	int byte;
	int started;
	int error;
	int blank;

	/* Skips blank lines and the blanks that start a line. */
	started = 0;
	for (;;) {
		byte = getc(xargs_input);
		if (byte == EOF)
			return 0;

		/* Anything but a blank or a newline starts the argument. */
		blank = is_blank(byte);
		if (byte != '\n' && !blank)
			break;
	}

	/* Collects the line up to its unescaped newline. */
	for (;;) {
		/* The newline or the end of the file ends the line. */
		if (byte == EOF || byte == '\n')
			break;

		/* Quotes keep their text; a backslash keeps the next byte. */
		if (byte == '\'' || byte == '"') {
			error = read_quoted(token, byte);
			if (error != 0)
				return -1;
		} else if (byte == '\\') {
			byte = getc(xargs_input);
			if (byte == EOF)
				break;
			error = append_byte(token, byte);
			if (error != 0)
				return -1;
		} else {
			error = append_byte(token, byte);
			if (error != 0)
				return -1;
		}

		/* The line holds something; goes on with the next byte. */
		started = 1;
		byte = getc(xargs_input);
	}

	/* Reports a line that turned out to hold nothing. */
	if (!started)
		return 0;

	/* Succeeded: the line is the argument. */
	token->line_end = 1;
	return 1;
}

/*
 * Reads one argument separated by blanks and newlines.  After the
 * argument, the blanks that follow it are read too, so that line_end tells
 * whether a newline ended the input line there: a line whose last
 * character is a blank goes on into the next line.
 */
static int
read_blank_token(
	struct xargs_token *token)
{
	int byte;
	int error;
	int blank;
	int trailing_blank;

	/* Skips the separators before the argument. */
	for (;;) {
		byte = getc(xargs_input);
		if (byte == EOF)
			return 0;

		/* Anything but a separator starts the argument. */
		blank = is_blank(byte);
		if (byte != '\n' && !blank)
			break;
	}

	/* Collects the argument up to the next unquoted separator. */
	for (;;) {
		/* An unquoted separator or the end of the file ends it. */
		blank = is_blank(byte);
		if (byte == EOF || byte == '\n' || blank)
			break;

		/* Quotes keep their text; a backslash keeps the next byte. */
		if (byte == '\'' || byte == '"') {
			error = read_quoted(token, byte);
			if (error != 0)
				return -1;
		} else if (byte == '\\') {
			byte = getc(xargs_input);
			if (byte == EOF)
				break;
			error = append_byte(token, byte);
			if (error != 0)
				return -1;
		} else {
			error = append_byte(token, byte);
			if (error != 0)
				return -1;
		}

		/* Goes on with the next byte. */
		byte = getc(xargs_input);
	}

	/* Reads the blanks after the argument to learn how its line ends. */
	trailing_blank = 0;
	for (;;) {
		blank = is_blank(byte);
		if (!blank)
			break;
		trailing_blank = 1;
		byte = getc(xargs_input);
	}

	/*
	 * A newline right after the argument ends the line; after a blank it
	 * continues the line instead.  The end of the file ends the line, and
	 * the start of another argument is put back for the next call.
	 */
	if (byte == EOF) {
		token->line_end = 1;
	} else if (byte == '\n') {
		if (!trailing_blank)
			token->line_end = 1;
	} else {
		ungetc(byte, xargs_input);
	}

	/* Succeeded: one argument. */
	return 1;
}

/*
 * Reads the text of a quoted string up to its closing quote.  A newline
 * or the end of the file before the quote closes is an error.
 */
static int
read_quoted(
	struct xargs_token *token,
	int quote)
{
	int byte;
	int error;

	/* Keeps each byte up to the matching quote. */
	for (;;) {
		byte = getc(xargs_input);
		if (byte == quote)
			break;

		/* The quote must close on the same line. */
		if (byte == EOF || byte == '\n') {
			if (quote == '\'')
				fprintf(stderr, "xargs: unmatched single quote\n");
			else
				fprintf(stderr, "xargs: unmatched double quote\n");
			return -1;
		}

		/* Keeps the quoted byte. */
		error = append_byte(token, byte);
		if (error != 0)
			return -1;
	}

	/* Succeeded: the quoted text is part of the argument. */
	return 0;
}

/* Adds one byte to the argument, growing its buffer. */
static int
append_byte(
	struct xargs_token *token,
	int byte)
{
	char *grown;
	size_t capacity;

	/* Grows the buffer, keeping room for the terminating null byte. */
	if (token->length + 1 >= token->capacity) {
		capacity = token->capacity * 2;
		if (capacity < 64)
			capacity = 64;
		grown = realloc(token->text, capacity);
		if (grown == NULL) {
			fprintf(stderr, "xargs: out of memory\n");
			return -1;
		}

		/* Uses the larger buffer from now on. */
		token->text = grown;
		token->capacity = capacity;
	}

	/* Stores the byte and keeps the text terminated. */
	token->text[token->length] = (char)byte;
	token->length++;
	token->text[token->length] = '\0';
	return 0;
}

/* Tells whether a byte is a blank of the POSIX locale. */
static int
is_blank(
	int byte)
{
	/* Space and tab are the blanks. */
	if (byte == ' ')
		return 1;
	if (byte == '\t')
		return 1;

	/* Anything else is not. */
	return 0;
}

/* Tells whether an argument is the -E end string. */
static int
is_eof_string(
	const struct xargs_options *options,
	const struct xargs_token *token)
{
	int compare;

	/* Without -E, or with -0, no argument ends the input. */
	if (options->eof_string == NULL)
		return 0;
	if (options->null_separated)
		return 0;

	/* An empty argument never matches. */
	if (token->length == 0)
		return 0;

	/* Compares the whole argument with the end string. */
	compare = strcmp(token->text, options->eof_string);
	if (compare == 0)
		return 1;

	/* Not the end string. */
	return 0;
}

/*
 * Starts the command line with the utility and its initial arguments, or
 * with echo when no utility is named.
 */
static int
command_init(
	struct xargs_command *command,
	int argc,
	char **argv,
	int first)
{
	static char echo[] = "echo";
	size_t index;

	/* Room for the initial entries and some arguments. */
	memset(command, 0, sizeof(*command));
	command->capacity = (size_t)(argc - first) + 64;
	command->argv = calloc(command->capacity, sizeof(*command->argv));
	if (command->argv == NULL)
		return -1;

	/* The utility and its arguments, or echo alone. */
	if (first >= argc) {
		command->argv[0] = echo;
		command->count = 1;
	} else {
		for (index = 0; index < (size_t)(argc - first); index++)
			command->argv[index] = argv[first + (int)index];
		command->count = (size_t)(argc - first);
	}

	/* Counts the initial entries with their null bytes. */
	command->base_count = command->count;
	command->size = 0;
	for (index = 0; index < command->count; index++)
		command->size += strlen(command->argv[index]) + 1;
	command->base_size = command->size;

	/* The kernel also counts a pointer to each and the null pointer. */
	command->system_size = command->size + (command->count + 1) * sizeof(char *);
	command->base_system_size = command->system_size;

	/* Succeeded: no input argument yet. */
	command->argv[command->count] = NULL;
	return 0;
}

/* Adds a copy of one input argument to the command line. */
static int
command_append(
	struct xargs_command *command,
	const char *text,
	size_t length)
{
	char **grown;
	char *copy;
	size_t capacity;

	/* Grows the entry array, keeping room for the terminating null. */
	if (command->count + 1 >= command->capacity) {
		capacity = command->capacity * 2;
		grown = realloc(command->argv, capacity * sizeof(*grown));
		if (grown == NULL) {
			fprintf(stderr, "xargs: out of memory\n");
			return -1;
		}

		/* Uses the larger array from now on. */
		command->argv = grown;
		command->capacity = capacity;
	}

	/* Copies the argument; the input buffer is reused. */
	copy = malloc(length + 1);
	if (copy == NULL) {
		fprintf(stderr, "xargs: out of memory\n");
		return -1;
	}

	/* Fills the copy. */
	memcpy(copy, text, length);
	copy[length] = '\0';

	/* Adds it and counts its bytes. */
	command->argv[command->count] = copy;
	command->count++;
	command->argv[command->count] = NULL;
	command->size += length + 1;
	command->system_size += length + 1 + sizeof(char *);
	return 0;
}

/* Drops the input arguments, keeping the utility and its initial ones. */
static void
command_reset(
	struct xargs_command *command)
{
	size_t index;

	/* Frees the copies of the input arguments. */
	for (index = command->base_count; index < command->count; index++)
		free(command->argv[index]);

	/* Back to the initial entries alone. */
	command->count = command->base_count;
	command->argv[command->count] = NULL;
	command->size = command->base_size;
	command->system_size = command->base_system_size;
	command->lines = 0;
}

/*
 * Gathers input arguments into command lines within the -n, -L and -s
 * limits and runs the utility for each.  Returns 0 at the end of the input
 * or the exit status that stops xargs early.
 */
static int
run_arguments(
	const struct xargs_options *options,
	struct xargs_command *command,
	struct xargs_run *run)
{
	struct xargs_token token;
	size_t needed;
	size_t system_needed;
	long gathered;
	int status;
	int error;
	int full;
	int pending;
	int overflow;

	/* Reads arguments until the end of the input. */
	memset(&token, 0, sizeof(token));
	for (;;) {
		status = read_token(options, &token);
		if (status < 0)
			return 1;
		if (status == 0)
			break;

		/*
		 * The argument must fit at least alone with the initial ones,
		 * both as -s counts and as the kernel counts.
		 */
		needed = token.length + 1;
		system_needed = needed + sizeof(char *);
		overflow = 0;
		if (command->base_size + needed > options->max_size)
			overflow = 1;
		else if (command->base_system_size + system_needed > options->system_room)
			overflow = 1;
		if (overflow) {
			fprintf(stderr, "xargs: argument line too long\n");
			return 1;
		}

		/* Learns whether the argument still fits on this command line. */
		overflow = 0;
		if (command->size + needed > options->max_size)
			overflow = 1;
		else if (command->system_size + system_needed > options->system_room)
			overflow = 1;

		/*
		 * An argument that does not fit starts the next command line.
		 * With -x, a line cut short of its -n or -L count is an error.
		 */
		if (overflow) {
			if (options->exit_on_overflow) {
				if (options->max_arguments != 0 || options->max_lines != 0) {
					fprintf(stderr, "xargs: argument list too long\n");
					return 1;
				}
			}

			/* Runs the full command line and starts the next one. */
			status = run_command(options, command->argv, run);
			command_reset(command);
			if (status != 0)
				return status;
		}

		/* Adds the argument to the command line. */
		error = command_append(command, token.text, token.length);
		if (error != 0)
			return 1;

		/* A line holding -n arguments or -L input lines runs now. */
		full = 0;
		gathered = (long)(command->count - command->base_count);
		if (options->max_arguments != 0 && gathered >= options->max_arguments)
			full = 1;
		if (token.line_end)
			command->lines++;
		if (options->max_lines != 0 && command->lines >= options->max_lines)
			full = 1;

		/* Runs the full command line and starts the next one. */
		if (full) {
			status = run_command(options, command->argv, run);
			command_reset(command);
			if (status != 0)
				return status;
		}
	}

	/*
	 * Runs what is left, or the utility alone when the input gave no
	 * argument at all and -r was not given.
	 */
	gathered = (long)(command->count - command->base_count);
	pending = 0;
	if (gathered > 0)
		pending = 1;
	else if (!run->ran && !options->skip_empty_input)
		pending = 1;

	/* Runs the last command line. */
	if (pending) {
		status = run_command(options, command->argv, run);
		command_reset(command);
		if (status != 0)
			return status;
	}

	/* Succeeded: the whole input was used. */
	free(token.text);
	return 0;
}

/*
 * Runs the utility once for each input line with -I, putting the line in
 * place of the replacement string in every initial argument.
 */
static int
run_insert(
	const struct xargs_options *options,
	struct xargs_command *command,
	struct xargs_run *run)
{
	struct xargs_token token;
	char **argv;
	size_t index;
	size_t size;
	size_t argument_size;
	int status;
	int overflow;

	/* Room for a command line of the same shape as the initial one. */
	argv = calloc(command->base_count + 1, sizeof(*argv));
	if (argv == NULL) {
		fprintf(stderr, "xargs: out of memory\n");
		return 1;
	}

	/* Reads lines until the end of the input. */
	memset(&token, 0, sizeof(token));
	for (;;) {
		status = read_token(options, &token);
		if (status < 0)
			return 1;
		if (status == 0)
			break;

		/* Builds each argument with the line in place of the string. */
		size = 0;
		for (index = 0; index < command->base_count; index++) {
			argv[index] = replace_all(command->argv[index], options->replace_string, token.text, &argument_size);
			if (argv[index] == NULL) {
				fprintf(stderr, "xargs: out of memory\n");
				return 1;
			}

			/* Counts the argument with its null byte. */
			size += argument_size;
		}

		/* Ends the argument vector. */
		argv[command->base_count] = NULL;

		/*
		 * -I implies -x: a command line that does not fit, as -s counts
		 * or as the kernel counts, stops xargs.
		 */
		overflow = 0;
		if (size > options->max_size)
			overflow = 1;
		else if (size + (command->base_count + 1) * sizeof(char *) > options->system_room)
			overflow = 1;
		status = 0;
		if (overflow) {
			fprintf(stderr, "xargs: argument line too long\n");
			status = 1;
		} else {
			status = run_command(options, argv, run);
		}

		/* Frees the built arguments before going on or stopping. */
		for (index = 0; index < command->base_count; index++)
			free(argv[index]);
		if (status != 0)
			return status;
	}

	/* Succeeded: every line was used. */
	free(argv);
	free(token.text);
	return 0;
}

/*
 * Copies a template with every occurrence of a pattern replaced; stores
 * the size of the result with its null byte.
 */
static char *
replace_all(
	const char *template,
	const char *pattern,
	const char *replacement,
	size_t *size)
{
	const char *found;
	const char *at;
	char *result;
	char *out;
	size_t pattern_length;
	size_t replacement_length;
	size_t count;
	size_t length;

	/* Counts the occurrences to size the result. */
	pattern_length = strlen(pattern);
	replacement_length = strlen(replacement);
	count = 0;
	at = template;
	for (;;) {
		found = strstr(at, pattern);
		if (found == NULL)
			break;
		count++;
		at = found + pattern_length;
	}

	/* Allocates the result. */
	length = strlen(template) - count * pattern_length + count * replacement_length;
	result = malloc(length + 1);
	if (result == NULL)
		return NULL;

	/* Copies the text between occurrences and the replacement for each. */
	out = result;
	at = template;
	for (;;) {
		found = strstr(at, pattern);
		if (found == NULL)
			break;
		memcpy(out, at, (size_t)(found - at));
		out += found - at;
		memcpy(out, replacement, replacement_length);
		out += replacement_length;
		at = found + pattern_length;
	}

	/* Copies the text after the last occurrence. */
	strcpy(out, at);

	/* Succeeded: the replaced argument. */
	*size = length + 1;
	return result;
}

/*
 * Runs the utility with one command line and waits for it.  Returns 0 to
 * go on, or the status that stops xargs: 124 after an exit status of 255,
 * 125 after a signal, 126 or 127 when the utility could not be run.
 */
static int
run_command(
	const struct xargs_options *options,
	char **argv,
	struct xargs_run *run)
{
	int report[2];
	int exec_error;
	int descriptor;
	int error;
	int wait_status;
	int accepted;
	int status;
	ssize_t got;
	pid_t child;
	pid_t waited;

	/* Shows the command line and, with -p, asks whether to run it. */
	accepted = confirm(options, argv, run);
	if (accepted < 0)
		return 1;
	run->ran = 1;
	if (!accepted)
		return 0;

	/* -P: waits for one utility when as many as allowed are running. */
	if (run->running >= options->max_procs) {
		status = reap_child(run);
		if (status != 0)
			return status;
	}

	/*
	 * A pipe closed on exec tells whether the utility started: it carries
	 * the error of an exec that failed and nothing otherwise.
	 */
	error = pipe(report);
	if (error != 0) {
		fprintf(stderr, "xargs: pipe: %s\n", strerror(errno));
		return 1;
	}

	/* Closes the writing end in the utility once it starts. */
	fcntl(report[1], F_SETFD, FD_CLOEXEC);

	/* Starts the utility in a child. */
	fflush(stderr);
	child = fork();
	if (child < 0) {
		fprintf(stderr, "xargs: fork: %s\n", strerror(errno));
		close(report[0]);
		close(report[1]);
		return 1;
	}

	/*
	 * The child reads nothing of xargs's input: its standard input is
	 * /dev/null, /dev/tty with -o, or xargs's own with -a.  It reports a
	 * failed exec through the pipe.
	 */
	if (child == 0) {
		close(report[0]);
		descriptor = -1;
		if (options->open_tty)
			descriptor = open("/dev/tty", O_RDONLY);
		else if (!options->argument_file)
			descriptor = open("/dev/null", O_RDONLY);
		if (descriptor >= 0) {
			dup2(descriptor, STDIN_FILENO);
			if (descriptor != STDIN_FILENO)
				close(descriptor);
		}

		/* Runs the utility, or reports why it could not. */
		execvp(argv[0], argv);
		exec_error = errno;
		got = write(report[1], &exec_error, sizeof(exec_error));
		(void)got;
		_exit(XARGS_STATUS_NOT_FOUND);
	}

	/* Learns whether the exec failed; the pipe closes when it succeeds. */
	close(report[1]);
	exec_error = 0;
	do {
		got = read(report[0], &exec_error, sizeof(exec_error));
	} while (got < 0 && errno == EINTR);
	close(report[0]);

	/* A utility that could not be run stops xargs. */
	if (got == (ssize_t)sizeof(exec_error)) {
		do {
			waited = waitpid(child, &wait_status, 0);
		} while (waited < 0 && errno == EINTR);
		fprintf(stderr, "xargs: %s: %s\n", argv[0], strerror(exec_error));
		if (exec_error == ENOENT)
			return XARGS_STATUS_NOT_FOUND;
		return XARGS_STATUS_NOT_RUNNABLE;
	}

	/* It runs; without -P it is waited for now. */
	run->running++;
	if (options->max_procs == 1) {
		status = reap_child(run);
		return status;
	}

	/* Succeeded: xargs goes on with the input. */
	return 0;
}

/*
 * Waits for one utility to end and takes its status: 255 and a signal
 * stop xargs (124 and 125); another failure is remembered as 123.
 */
static int
reap_child(
	struct xargs_run *run)
{
	int wait_status;
	int signaled;
	int exit_status;
	pid_t waited;

	/* Any of the utilities running. */
	do {
		waited = waitpid(-1, &wait_status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited < 0) {
		fprintf(stderr, "xargs: wait: %s\n", strerror(errno));
		run->running = 0;
		return 1;
	}

	/* One fewer running. */
	run->running--;

	/* A utility killed by a signal stops xargs. */
	signaled = WIFSIGNALED(wait_status);
	if (signaled) {
		fprintf(stderr, "xargs: %s: terminated by signal %d\n", run->name, WTERMSIG(wait_status));
		return XARGS_STATUS_UTILITY_KILLED;
	}

	/* A utility that exits with 255 stops xargs. */
	exit_status = WEXITSTATUS(wait_status);
	if (exit_status == 255) {
		fprintf(stderr, "xargs: %s: exited with status 255; aborting\n", run->name);
		return XARGS_STATUS_UTILITY_ABORTED;
	}

	/* Any other failure is remembered for the final status. */
	if (exit_status != 0)
		run->status = XARGS_STATUS_UTILITY_FAILED;

	/* Succeeded: the utility ended. */
	return 0;
}

/*
 * Waits for every utility still running.  Returns the status xargs stops
 * with: the one given, or the first a utility's end calls for.
 */
static int
reap_all(
	struct xargs_run *run,
	int status)
{
	int result;

	/* Each utility; the first reason to stop is kept. */
	while (run->running > 0) {
		result = reap_child(run);
		if (status == 0)
			status = result;
	}

	/* The first reason to stop, or none. */
	return status;
}

/*
 * Writes the command line to standard error for -t and asks about it for
 * -p.  Returns 1 to run it, 0 to skip it, and -1 when /dev/tty cannot be
 * read.
 */
static int
confirm(
	const struct xargs_options *options,
	char **argv,
	struct xargs_run *run)
{
	size_t index;
	int answer;

	/* Without -t nothing is shown and the command runs. */
	if (!options->trace)
		return 1;

	/* Writes the utility and its arguments separated by spaces. */
	for (index = 0; argv[index] != NULL; index++) {
		if (index != 0)
			fputc(' ', stderr);
		fputs(argv[index], stderr);
	}

	/* Ends the line for -t alone. */
	if (!options->prompt) {
		fputc('\n', stderr);
		fflush(stderr);
		return 1;
	}

	/* Asks for -p and runs only after an affirmative answer. */
	fputs(" ?...", stderr);
	fflush(stderr);
	answer = ask(run);
	if (answer < 0)
		return -1;

	/* Reports the answer. */
	return answer;
}

/*
 * Reads one answer line from /dev/tty.  Returns 1 for an affirmative
 * answer, 0 for anything else, and -1 when the terminal cannot be read.
 */
static int
ask(
	struct xargs_run *run)
{
	int byte;
	int first;

	/* Opens the terminal on the first question. */
	if (run->answers == NULL) {
		run->answers = fopen("/dev/tty", "r");
		if (run->answers == NULL) {
			fprintf(stderr, "xargs: /dev/tty: %s\n", strerror(errno));
			return -1;
		}
	}

	/* Reads the answer line, keeping its first character. */
	first = EOF;
	for (;;) {
		byte = fgetc(run->answers);
		if (byte == EOF || byte == '\n')
			break;
		if (first == EOF)
			first = byte;
	}

	/* An answer starting with y is affirmative in the POSIX locale. */
	if (first == 'y' || first == 'Y')
		return 1;

	/* Anything else declines. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the options POSIX gives xargs. */
	fprintf(stderr,
		"usage: xargs [-prtx] [-0|-E eofstr] [-I replstr|-L number|-n number]\n"
		"             [-s size] [utility [argument...]]\n");
	exit(1);
}
