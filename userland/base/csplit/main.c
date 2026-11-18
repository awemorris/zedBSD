/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Splits a file by context (POSIX XCU csplit).
 *
 *	csplit [-ks] [-f prefix] [-n number] file arg...
 *
 * The file (standard input for -) is cut into pieces named prefix (xx by
 * default) and a number of number digits (2 by default).  Each arg ends a
 * piece:
 *
 *	/rexp/[offset]	before the next line the basic regular expression
 *			matches, moved by offset lines
 *	%rexp%[offset]	the same, but the lines before it are skipped,
 *			not written
 *	line_no		before that line
 *	{num}		the previous arg num more times; a line number
 *			repeats at multiples of itself
 *
 * A regular expression is searched from the line after the one the last
 * piece ended before, or from the first line at the start.  What is left
 * after the last arg is the last piece.  The size of each piece is written
 * unless -s is given.  On an error the rest of the input is written as a
 * piece, the pieces are removed unless -k is given, and csplit fails.
 */

#include <errno.h>
#include <limits.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The kinds of arg. */
#define CSPLIT_LINE 0
#define CSPLIT_REGEX 1
#define CSPLIT_SKIP 2

/*
 * One arg, with its repeat count.
 *
 * pattern is compiled for the regular expression kinds; number is the
 * line number of CSPLIT_LINE or the offset of the others.
 */
struct csplit_arg {
	const char *text;
	int kind;
	regex_t pattern;
	long number;
	long repeat;
};

/*
 * The input, held whole, and the pieces made from it.
 *
 * One instance lives for the run.  lines[i] is the offset of line i + 1
 * in data; lines[count] is the size of data.  next is the 1-based number
 * of the line the next piece starts with.
 */
struct csplit_state {
	char *data;
	size_t size;
	size_t *lines;
	long count;
	long next;
	int searched;
	const char *prefix;
	int digits;
	int keep;
	int silent;
	long pieces;
};

static int read_options(int argc, char **argv, struct csplit_state *state);
static int parse_args(int count, char **texts, struct csplit_arg *args, long *used);
static int parse_arg(const char *text, struct csplit_arg *arg);
static int read_input(const char *path, struct csplit_state *state);
static int run_arg(struct csplit_state *state, const struct csplit_arg *arg, long repetition);
static int write_piece(struct csplit_state *state, long end);
static int fail(struct csplit_state *state);
static void remove_pieces(const struct csplit_state *state);
static int piece_name(const struct csplit_state *state, long index, char *name, size_t size);
static void usage(void);

/*
 * Runs csplit.
 */
int
main(
	int argc,
	char **argv)
{
	struct csplit_state state;
	struct csplit_arg *args;
	long count;
	long index;
	long repetition;
	int first;
	int status;

	/* Reads the options; the file and the args follow them. */
	first = read_options(argc, argv, &state);
	if (argc - first < 2)
		usage();

	/* Parses every arg before the input is touched. */
	args = calloc((size_t)(argc - first), sizeof(*args));
	if (args == NULL) {
		fprintf(stderr, "csplit: out of memory\n");
		return 1;
	}

	/* Parses the args. */
	status = parse_args(argc - first - 1, argv + first + 1, args, &count);
	if (status != 0)
		return 1;

	/* Reads the input. */
	status = read_input(argv[first], &state);
	if (status != 0)
		return 1;

	/* Applies each arg, as many times as it repeats. */
	for (index = 0; index < count; index++) {
		for (repetition = 0; repetition <= args[index].repeat; repetition++) {
			status = run_arg(&state, &args[index], repetition);
			if (status != 0) {
				fail(&state);
				return 1;
			}
		}
	}

	/* The rest of the input is the last piece. */
	status = write_piece(&state, state.count + 1);
	if (status != 0) {
		remove_pieces(&state);
		return 1;
	}

	/* A failed write of the sizes is an error too. */
	fflush(stdout);
	status = ferror(stdout);
	if (status != 0) {
		fprintf(stderr, "csplit: write error\n");
		return 1;
	}

	/* Succeeded: every piece was written. */
	return 0;
}

/*
 * Reads the options and returns the index of the file operand.  An
 * invalid option ends csplit with a diagnostic.
 */
static int
read_options(
	int argc,
	char **argv,
	struct csplit_state *state)
{
	char *end;
	long digits;
	int option;

	/* The defaults: xx and two digits. */
	memset(state, 0, sizeof(*state));
	state->prefix = "xx";
	state->digits = 2;
	state->next = 1;

	/* Reads each option. */
	for (;;) {
		option = getopt(argc, argv, "f:kn:s");
		if (option == -1)
			break;

		/* Records what the option asks for. */
		switch (option) {
		case 'f':
			state->prefix = optarg;
			break;
		case 'k':
			state->keep = 1;
			break;
		case 'n':
			/* The number of digits must be a small positive number. */
			errno = 0;
			digits = strtol(optarg, &end, 10);
			if (end == optarg || *end != '\0' || digits <= 0 || digits > 18) {
				fprintf(stderr, "csplit: invalid number: '%s'\n", optarg);
				exit(1);
			}

			/* Keeps the number of digits. */
			state->digits = (int)digits;
			break;
		case 's':
			state->silent = 1;
			break;
		default:
			usage();
			break;
		}
	}

	/* Reports where the operands start. */
	return optind;
}

/*
 * Parses the args into the array, folding each {num} into the arg before
 * it.  Line numbers must be positive and not decrease.  Stores the number
 * of args.
 */
static int
parse_args(
	int count,
	char **texts,
	struct csplit_arg *args,
	long *used)
{
	char *end;
	long repeat;
	long last_line;
	long index;
	int status;

	/* Takes each text in turn. */
	*used = 0;
	last_line = 0;
	for (index = 0; index < count; index++) {
		/* {num} repeats the arg before it. */
		if (texts[index][0] == '{') {
			errno = 0;
			repeat = strtol(texts[index] + 1, &end, 10);
			if (*used == 0 || end == texts[index] + 1 || end[0] != '}' || end[1] != '\0' || repeat < 0 || errno != 0) {
				fprintf(stderr, "csplit: '%s': invalid repetition\n", texts[index]);
				return -1;
			}

			/* The previous arg repeats that many more times. */
			args[*used - 1].repeat = repeat;
			continue;
		}

		/* Anything else is an arg of its own. */
		status = parse_arg(texts[index], &args[*used]);
		if (status != 0)
			return -1;

		/* Line numbers must not decrease. */
		if (args[*used].kind == CSPLIT_LINE) {
			if (args[*used].number < last_line) {
				fprintf(stderr, "csplit: line number '%s' is smaller than preceding line number, %ld\n", texts[index], last_line);
				return -1;
			}

			/* Remembers the line number for the next one. */
			last_line = args[*used].number;
		}

		/* The arg is kept. */
		(*used)++;
	}

	/* Succeeded: the args. */
	return 0;
}

/* Parses one arg: /rexp/offset, %rexp%offset or a line number. */
static int
parse_arg(
	const char *text,
	struct csplit_arg *arg)
{
	char expression[LINE_MAX];
	const char *close;
	char *end;
	size_t length;
	int delimiter;
	int status;

	/* A line number is decimal and positive. */
	arg->text = text;
	delimiter = text[0];
	if (delimiter != '/' && delimiter != '%') {
		errno = 0;
		arg->number = strtol(text, &end, 10);
		if (end == text || *end != '\0' || arg->number <= 0 || errno != 0) {
			fprintf(stderr, "csplit: '%s': invalid pattern\n", text);
			return -1;
		}

		/* Succeeded: a line number. */
		arg->kind = CSPLIT_LINE;
		return 0;
	}

	/* The expression runs to the matching delimiter, which a backslash escapes. */
	close = text + 1;
	while (*close != '\0' && *close != delimiter) {
		if (close[0] == '\\' && close[1] != '\0')
			close++;
		close++;
	}

	/* The closing delimiter must be there. */
	if (*close != delimiter) {
		fprintf(stderr, "csplit: '%s': unmatched %c\n", text, delimiter);
		return -1;
	}

	/* Measures the expression between the delimiters. */
	length = (size_t)(close - text - 1);
	if (length >= sizeof(expression)) {
		fprintf(stderr, "csplit: '%s': expression too long\n", text);
		return -1;
	}

	/* Copies it with a terminator. */
	memcpy(expression, text + 1, length);
	expression[length] = '\0';

	/* Compiles it as a basic regular expression. */
	status = regcomp(&arg->pattern, expression, REG_NOSUB);
	if (status != 0) {
		fprintf(stderr, "csplit: '%s': invalid regular expression\n", text);
		return -1;
	}

	/* An optional signed offset follows. */
	arg->number = 0;
	if (close[1] != '\0') {
		errno = 0;
		arg->number = strtol(close + 1, &end, 10);
		if (*end != '\0' || errno != 0 || (close[1] != '+' && close[1] != '-' && (close[1] < '0' || close[1] > '9'))) {
			fprintf(stderr, "csplit: '%s': invalid offset\n", text);
			return -1;
		}
	}

	/* Succeeded: /rexp/ writes a piece, %rexp% skips. */
	arg->kind = CSPLIT_REGEX;
	if (delimiter == '%')
		arg->kind = CSPLIT_SKIP;
	return 0;
}

/* Reads the whole input and finds its lines. */
static int
read_input(
	const char *path,
	struct csplit_state *state)
{
	FILE *input;
	char *grown;
	size_t capacity;
	size_t got;
	size_t index;
	size_t *offsets;
	long count;
	int compare;
	int failed;

	/* - is standard input. */
	input = stdin;
	compare = strcmp(path, "-");
	if (compare != 0) {
		input = fopen(path, "r");
		if (input == NULL) {
			fprintf(stderr, "csplit: %s: %s\n", path, strerror(errno));
			return -1;
		}
	}

	/* Reads everything. */
	capacity = 65536;
	state->data = malloc(capacity);
	if (state->data == NULL) {
		fprintf(stderr, "csplit: out of memory\n");
		return -1;
	}

	/* Reads until the end of the input, growing the buffer. */
	for (;;) {
		if (state->size == capacity) {
			capacity *= 2;
			grown = realloc(state->data, capacity);
			if (grown == NULL) {
				fprintf(stderr, "csplit: out of memory\n");
				return -1;
			}

			/* Uses the larger buffer from now on. */
			state->data = grown;
		}

		/* Reads what fits. */
		got = fread(state->data + state->size, 1, capacity - state->size, input);
		if (got == 0)
			break;
		state->size += got;
	}

	/* Tells a read error from the end of the input. */
	failed = ferror(input);
	if (failed) {
		fprintf(stderr, "csplit: %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* Counts the lines; a last line without a newline counts too. */
	count = 0;
	for (index = 0; index < state->size; index++) {
		if (state->data[index] == '\n')
			count++;
	}

	/* A last line without a newline counts too. */
	if (state->size > 0 && state->data[state->size - 1] != '\n')
		count++;

	/* Records where each line starts, and the end. */
	offsets = malloc(((size_t)count + 1) * sizeof(*offsets));
	if (offsets == NULL) {
		fprintf(stderr, "csplit: out of memory\n");
		return -1;
	}

	/* Records where each line starts. */
	count = 0;
	offsets[0] = 0;
	for (index = 0; index < state->size; index++) {
		if (state->data[index] == '\n') {
			count++;
			offsets[count] = index + 1;
		}
	}

	/* A last line without a newline ends at the end of the data. */
	if (state->size > 0 && state->data[state->size - 1] != '\n') {
		count++;
		offsets[count] = state->size;
	}

	/* Succeeded: the input and its lines. */
	state->lines = offsets;
	state->count = count;
	return 0;
}

/*
 * Applies one arg, for the repetition given.  Returns -1 after a
 * diagnosed failure, with the pieces still to be cleaned up.
 */
static int
run_arg(
	struct csplit_state *state,
	const struct csplit_arg *arg,
	long repetition)
{
	char *line;
	size_t length;
	long target;
	long search;
	int status;
	int found;

	/* A line number repeats at multiples of itself. */
	if (arg->kind == CSPLIT_LINE) {
		target = arg->number * (repetition + 1);
		if (target > state->count || target < state->next) {
			fprintf(stderr, "csplit: '%s': line number out of range", arg->text);
			if (repetition > 0)
				fprintf(stderr, " on repetition %ld", repetition);
			fputc('\n', stderr);
			return -1;
		}

		/* Writes the lines before the target line. */
		status = write_piece(state, target);
		if (status != 0)
			return -1;
		return 0;
	}

	/* Searches from the line after the last cut, or the first line. */
	search = state->next;
	if (state->searched)
		search++;
	found = 0;
	for (; search <= state->count; search++) {
		length = state->lines[search] - state->lines[search - 1];
		line = malloc(length + 1);
		if (line == NULL) {
			fprintf(stderr, "csplit: out of memory\n");
			return -1;
		}

		/* Copies the line without its newline for the match. */
		memcpy(line, state->data + state->lines[search - 1], length);
		if (length > 0 && line[length - 1] == '\n')
			length--;
		line[length] = '\0';
		status = regexec(&arg->pattern, line, 0, NULL, 0);
		free(line);
		if (status == 0) {
			found = 1;
			break;
		}
	}

	/* An expression that matches no line is an error. */
	if (!found) {
		fprintf(stderr, "csplit: '%s': match not found", arg->text);
		if (repetition > 0)
			fprintf(stderr, " on repetition %ld", repetition);
		fputc('\n', stderr);
		return -1;
	}

	/* The cut is the matching line moved by the offset. */
	target = search + arg->number;
	if (target < state->next || target > state->count + 1) {
		fprintf(stderr, "csplit: '%s': line number out of range\n", arg->text);
		return -1;
	}

	/* Later searches start after the cut. */
	state->searched = 1;

	/* %rexp% skips the lines before the cut; /rexp/ writes them. */
	if (arg->kind == CSPLIT_SKIP) {
		state->next = target;
		return 0;
	}

	/* Writes the lines before the cut. */
	status = write_piece(state, target);
	if (status != 0)
		return -1;

	/* Succeeded: the piece was written. */
	return 0;
}

/*
 * Writes the lines from the next line up to, not including, line end as
 * the next piece, and writes its size.
 */
static int
write_piece(
	struct csplit_state *state,
	long end)
{
	char name[PATH_MAX + 1];
	FILE *output;
	size_t start;
	size_t stop;
	size_t written;
	int status;

	/* Names the piece. */
	status = piece_name(state, state->pieces, name, sizeof(name));
	if (status != 0) {
		fprintf(stderr, "csplit: too many files\n");
		return -1;
	}

	/* Creates it; it counts as made even if writing fails. */
	output = fopen(name, "w");
	if (output == NULL) {
		fprintf(stderr, "csplit: %s: %s\n", name, strerror(errno));
		return -1;
	}

	/* The piece counts as made, even if writing it fails. */
	state->pieces++;

	/* Writes its lines. */
	start = state->lines[state->next - 1];
	stop = state->lines[end - 1];
	written = fwrite(state->data + start, 1, stop - start, output);
	status = fclose(output);
	if (written != stop - start || status != 0) {
		fprintf(stderr, "csplit: %s: %s\n", name, strerror(errno));
		return -1;
	}

	/* Writes its size, and moves to the line after it. */
	if (!state->silent)
		printf("%lu\n", (unsigned long)(stop - start));
	state->next = end;
	return 0;
}

/*
 * Handles an error: writes the rest of the input as a piece, then removes
 * the pieces unless -k is given.
 */
static int
fail(
	struct csplit_state *state)
{
	/* The rest of the input becomes a piece, as other systems do. */
	write_piece(state, state->count + 1);
	fflush(stdout);

	/* Removes every piece unless they are to be kept. */
	if (!state->keep)
		remove_pieces(state);
	return -1;
}

/* Removes every piece made so far. */
static void
remove_pieces(
	const struct csplit_state *state)
{
	char name[PATH_MAX + 1];
	long index;
	int status;

	/* Removes them in order; a name that cannot be made has no file. */
	for (index = 0; index < state->pieces; index++) {
		status = piece_name(state, index, name, sizeof(name));
		if (status == 0)
			unlink(name);
	}
}

/* Makes the name of a piece: the prefix and the number in its digits. */
static int
piece_name(
	const struct csplit_state *state,
	long index,
	char *name,
	size_t size)
{
	long limit;
	int digit;
	int count;

	/* The number must fit in the digits. */
	limit = 1;
	for (digit = 0; digit < state->digits; digit++)
		limit *= 10;
	if (index >= limit)
		return -1;

	/* Writes the name. */
	count = snprintf(name, size, "%s%0*ld", state->prefix, state->digits, index);
	if (count < 0 || (size_t)count >= size)
		return -1;

	/* Succeeded: the name. */
	return 0;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: csplit [-ks] [-f prefix] [-n number] file arg...\n");
	exit(1);
}
