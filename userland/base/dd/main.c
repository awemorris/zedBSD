/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Converts and copies a file (POSIX XCU dd).
 *
 *	dd [operand...]
 *
 * The operands are if=file, of=file, ibs=expr, obs=expr, bs=expr,
 * cbs=expr, skip=n, seek=n, count=n and conv=value[,value...]; an expr
 * is a number, optionally followed by k (1024) or b (512), or several of
 * them joined by x and multiplied.
 *
 * The input is read one ibs block at a time (512 bytes by default); each
 * block may be padded to its full size (sync), have its byte pairs swapped
 * (swab), be converted between ASCII and EBCDIC (ascii, ebcdic, ibm) and
 * between cases (lcase, ucase), and be turned from newline-terminated lines
 * into fixed cbs records (block) or back (unblock).  The result is written
 * in obs blocks; with bs= and no conversion each input block is written as
 * it is.  skip= skips input blocks and seek= output blocks; without
 * notrunc the output is truncated after the seek.  noerror goes on after a
 * read error.
 *
 * At the end, and on SIGINT, the numbers of whole and partial blocks read
 * and written are reported on standard error, with the records that block
 * had to cut.
 */

#include "userland/base/dd/tables.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* The block size without ibs=, obs= or bs=. */
#define DD_DEFAULT_BLOCK 512

/* The conversions of conv=, one bit each. */
#define DD_ASCII 0x001
#define DD_EBCDIC 0x002
#define DD_IBM 0x004
#define DD_BLOCK 0x008
#define DD_UNBLOCK 0x010
#define DD_LCASE 0x020
#define DD_UCASE 0x040
#define DD_SWAB 0x080
#define DD_NOERROR 0x100
#define DD_NOTRUNC 0x200
#define DD_SYNC 0x400

/* The operands, by what they set. */
#define DD_OPERAND_IF 1
#define DD_OPERAND_OF 2
#define DD_OPERAND_CONV 3
#define DD_OPERAND_SKIP 4
#define DD_OPERAND_SEEK 5
#define DD_OPERAND_COUNT 6
#define DD_OPERAND_IBS 7
#define DD_OPERAND_OBS 8
#define DD_OPERAND_BS 9
#define DD_OPERAND_CBS 10

/* The conversions that change the data, which stop bs= from copying blocks as they are. */
#define DD_CHANGES (DD_ASCII | DD_EBCDIC | DD_IBM | DD_BLOCK | DD_UNBLOCK | DD_LCASE | DD_UCASE | DD_SWAB)

/*
 * What the operands ask for.
 *
 * One instance lives for the run.
 */
struct dd_options {
	const char *input_path;
	const char *output_path;
	size_t ibs;
	size_t obs;
	size_t cbs;
	int bs_given;
	unsigned long long skip;
	unsigned long long seek;
	unsigned long long count;
	int count_given;
	int conversions;
};

/*
 * The state of the copy.
 *
 * One instance lives for the run.  out holds the output not yet written
 * as a full obs block; record holds the record being built by block or
 * unblock, and column how much of it is filled.  The counters are what
 * the report on standard error gives.
 */
struct dd_state {
	const struct dd_options *options;
	int input;
	int output;
	unsigned char *in;
	unsigned char *out;
	size_t out_used;
	unsigned char *record;
	size_t column;
	int cutting;
	unsigned long long in_full;
	unsigned long long in_partial;
	unsigned long long out_full;
	unsigned long long out_partial;
	unsigned long long truncated;
};

/*
 * One name of an operand or of a conversion, and what it stands for.
 *
 * The tables below are constant for the life of the program.
 */
struct dd_name {
	const char *name;
	int value;
};

/* The operand names. */
static const struct dd_name dd_operands[] = {
	{ "if", DD_OPERAND_IF },
	{ "of", DD_OPERAND_OF },
	{ "conv", DD_OPERAND_CONV },
	{ "skip", DD_OPERAND_SKIP },
	{ "seek", DD_OPERAND_SEEK },
	{ "count", DD_OPERAND_COUNT },
	{ "ibs", DD_OPERAND_IBS },
	{ "obs", DD_OPERAND_OBS },
	{ "bs", DD_OPERAND_BS },
	{ "cbs", DD_OPERAND_CBS },
};

/* The conversion names of conv=. */
static const struct dd_name dd_conversions[] = {
	{ "ascii", DD_ASCII },
	{ "ebcdic", DD_EBCDIC },
	{ "ibm", DD_IBM },
	{ "block", DD_BLOCK },
	{ "unblock", DD_UNBLOCK },
	{ "lcase", DD_LCASE },
	{ "ucase", DD_UCASE },
	{ "swab", DD_SWAB },
	{ "noerror", DD_NOERROR },
	{ "notrunc", DD_NOTRUNC },
	{ "sync", DD_SYNC },
};

/*
 * Whether SIGINT has arrived.
 *
 * The handler sets it and the copy loop, which sees the read interrupted,
 * reports and ends; it is never cleared.
 */
static volatile sig_atomic_t dd_interrupted;

static int read_operands(int argc, char **argv, struct dd_options *options);
static int read_operand(const char *name, size_t length, const char *value, struct dd_options *options);
static int read_expression(const char *text, unsigned long long *value);
static int read_conversions(const char *text, int *conversions);
static int find_name(const struct dd_name *names, size_t count, const char *text, size_t length);
static int check_conversions(struct dd_options *options);
static int allocate_blocks(struct dd_state *state);
static int open_files(struct dd_state *state);
static int skip_input(struct dd_state *state);
static int seek_output(struct dd_state *state);
static int copy_blocks(struct dd_state *state);
static void convert_block(struct dd_state *state, unsigned char *block, size_t length);
static int emit(struct dd_state *state, const unsigned char *bytes, size_t length);
static int emit_record(struct dd_state *state, const unsigned char *bytes, size_t length);
static int finish_record(struct dd_state *state);
static int flush_output(struct dd_state *state, int final);
static int write_all(int descriptor, const unsigned char *bytes, size_t length);
static void report(const struct dd_state *state);
static const char *input_name(const struct dd_options *options);
static const char *output_name(const struct dd_options *options);
static void on_interrupt(int number);

/*
 * Runs dd.
 */
int
main(
	int argc,
	char **argv)
{
	struct dd_options options;
	struct dd_state state;
	struct sigaction previous;
	int status;
	int failed;

	/* Reads the operands. */
	status = read_operands(argc, argv, &options);
	if (status != 0)
		return 1;

	/* Allocates the blocks. */
	memset(&state, 0, sizeof(state));
	state.options = &options;
	status = allocate_blocks(&state);
	if (status != 0) {
		fprintf(stderr, "dd: out of memory\n");
		return 1;
	}

	/* Opens the files, skips and seeks. */
	status = open_files(&state);
	if (status != 0)
		return 1;

	/* Skips the input blocks. */
	status = skip_input(&state);
	if (status != 0) {
		report(&state);
		return 1;
	}

	/* Moves past the output blocks. */
	status = seek_output(&state);
	if (status != 0) {
		report(&state);
		return 1;
	}

	/*
	 * SIGINT reports what was copied before dd ends, unless it was
	 * ignored when dd started, as it is for a background command.
	 */
	sigaction(SIGINT, NULL, &previous);
	if (previous.sa_handler != SIG_IGN)
		signal(SIGINT, on_interrupt);

	/* Copies. */
	failed = 0;
	status = copy_blocks(&state);
	if (status != 0)
		failed = 1;

	/* A record left at the end of the input is finished. */
	if (state.column > 0 || state.cutting) {
		status = finish_record(&state);
		if (status != 0)
			failed = 1;
	}

	/* Writes what is left and closes the output. */
	status = flush_output(&state, 1);
	if (status != 0)
		failed = 1;
	status = close(state.output);
	if (status != 0) {
		fprintf(stderr, "dd: %s: %s\n", output_name(&options), strerror(errno));
		failed = 1;
	}

	/* Reports the counts. */
	report(&state);

	/* An interrupt ends dd as SIGINT would. */
	if (dd_interrupted) {
		signal(SIGINT, SIG_DFL);
		raise(SIGINT);
	}

	/* Reports a failure. */
	if (failed)
		return 1;

	/* Succeeded: the input was copied. */
	return 0;
}

/*
 * Reads the operands, name=value each.  Writes a diagnostic and returns
 * -1 for an invalid one.
 */
static int
read_operands(
	int argc,
	char **argv,
	struct dd_options *options)
{
	const char *equals;
	size_t length;
	int index;
	int status;
	int compare;

	/* The defaults; the standard files are NULL. */
	memset(options, 0, sizeof(*options));
	options->ibs = DD_DEFAULT_BLOCK;
	options->obs = DD_DEFAULT_BLOCK;

	/* -- may come first. */
	index = 1;
	if (argc > 1) {
		compare = strcmp(argv[1], "--");
		if (compare == 0)
			index = 2;
	}

	/* Reads each operand. */
	for (; index < argc; index++) {
		/* The name ends at the equals sign. */
		equals = strchr(argv[index], '=');
		if (equals == NULL) {
			fprintf(stderr, "dd: invalid operand: '%s'\n", argv[index]);
			return -1;
		}

		/* Measures the name. */
		length = (size_t)(equals - argv[index]);

		/* The value is read by what the name sets. */
		status = read_operand(argv[index], length, equals + 1, options);
		if (status != 0) {
			fprintf(stderr, "dd: invalid operand: '%s'\n", argv[index]);
			return -1;
		}
	}

	/* The conversions must agree with each other. */
	status = check_conversions(options);
	if (status != 0)
		return -1;

	/* Succeeded: the operands are read. */
	return 0;
}

/* Reads one operand given its name and value. */
static int
read_operand(
	const char *name,
	size_t length,
	const char *value,
	struct dd_options *options)
{
	unsigned long long number;
	int kind;
	int status;

	/* Finds what the name sets. */
	kind = find_name(dd_operands, sizeof(dd_operands) / sizeof(dd_operands[0]), name, length);
	if (kind < 0)
		return -1;

	/* The files and the conversions take text. */
	if (kind == DD_OPERAND_IF) {
		options->input_path = value;
		return 0;
	} else if (kind == DD_OPERAND_OF) {
		options->output_path = value;
		return 0;
	} else if (kind == DD_OPERAND_CONV) {
		status = read_conversions(value, &options->conversions);
		if (status != 0)
			return -1;
		return 0;
	}

	/* The others take a number. */
	status = read_expression(value, &number);
	if (status != 0)
		return -1;

	/* Block counts may be zero. */
	if (kind == DD_OPERAND_SKIP) {
		options->skip = number;
		return 0;
	} else if (kind == DD_OPERAND_SEEK) {
		options->seek = number;
		return 0;
	} else if (kind == DD_OPERAND_COUNT) {
		options->count = number;
		options->count_given = 1;
		return 0;
	}

	/* Sizes must be positive and fit in memory. */
	if (number == 0 || number > (unsigned long long)(~(size_t)0 / 4))
		return -1;

	/* Keeps the size. */
	switch (kind) {
	case DD_OPERAND_IBS:
		options->ibs = (size_t)number;
		break;
	case DD_OPERAND_OBS:
		options->obs = (size_t)number;
		break;
	case DD_OPERAND_BS:
		options->ibs = (size_t)number;
		options->obs = (size_t)number;
		options->bs_given = 1;
		break;
	default:
		options->cbs = (size_t)number;
		break;
	}

	/* Succeeded: the operand is read. */
	return 0;
}

/*
 * Finds a name of a given length in a table and gives its value, or -1
 * when it is not there.
 */
static int
find_name(
	const struct dd_name *names,
	size_t count,
	const char *text,
	size_t length)
{
	size_t index;
	size_t name_length;
	int compare;

	/* Compares each name of the same length. */
	for (index = 0; index < count; index++) {
		name_length = strlen(names[index].name);
		if (name_length != length)
			continue;
		compare = strncmp(names[index].name, text, length);
		if (compare == 0)
			return names[index].value;
	}

	/* Not found. */
	return -1;
}

/*
 * Checks that at most one character set, one of block and unblock, and
 * one case are asked for, and adds the conversions cbs= implies.
 */
static int
check_conversions(
	struct dd_options *options)
{
	int charset;

	/* One character set at most. */
	charset = options->conversions & (DD_ASCII | DD_EBCDIC | DD_IBM);
	if (charset != 0 && (charset & (charset - 1)) != 0) {
		fprintf(stderr, "dd: conv=ascii, ebcdic and ibm exclude each other\n");
		return -1;
	}

	/* One of block and unblock at most. */
	if ((options->conversions & DD_BLOCK) && (options->conversions & DD_UNBLOCK)) {
		fprintf(stderr, "dd: conv=block and unblock exclude each other\n");
		return -1;
	}

	/* One case at most. */
	if ((options->conversions & DD_LCASE) && (options->conversions & DD_UCASE)) {
		fprintf(stderr, "dd: conv=lcase and ucase exclude each other\n");
		return -1;
	}

	/* With cbs=, ascii unblocks and ebcdic and ibm block. */
	if (options->cbs > 0) {
		if (options->conversions & DD_ASCII)
			options->conversions |= DD_UNBLOCK;
		if (options->conversions & (DD_EBCDIC | DD_IBM))
			options->conversions |= DD_BLOCK;
	}

	/* block and unblock mean nothing without a record size. */
	if (options->cbs == 0)
		options->conversions &= ~(DD_BLOCK | DD_UNBLOCK);

	/* Succeeded: the conversions agree. */
	return 0;
}

/*
 * Reads an expression: numbers with an optional k or b, joined by x and
 * multiplied.
 */
static int
read_expression(
	const char *text,
	unsigned long long *value)
{
	const char *cursor;
	unsigned long long product;
	unsigned long long factor;
	unsigned long long unit;
	int digits;

	/* Multiplies each factor in. */
	product = 1;
	cursor = text;
	for (;;) {
		/* The digits of the factor. */
		factor = 0;
		digits = 0;
		while (*cursor >= '0' && *cursor <= '9') {
			if (factor > (~0ULL - 9) / 10)
				return -1;
			factor = factor * 10 + (unsigned long long)(*cursor - '0');
			digits++;
			cursor++;
		}

		/* A factor needs digits. */
		if (digits == 0)
			return -1;

		/* Its unit. */
		unit = 1;
		if (*cursor == 'k') {
			unit = 1024;
			cursor++;
		} else if (*cursor == 'b') {
			unit = 512;
			cursor++;
		}

		/* Multiplies, refusing an overflow. */
		if (factor != 0 && unit > ~0ULL / factor)
			return -1;
		factor *= unit;
		if (factor != 0 && product > ~0ULL / factor)
			return -1;
		product *= factor;

		/* x joins another factor; anything else ends the text. */
		if (*cursor != 'x')
			break;
		cursor++;
	}

	/* Nothing may follow. */
	if (*cursor != '\0')
		return -1;

	/* Succeeded: the value. */
	*value = product;
	return 0;
}

/* Reads the comma-separated conversions of conv=. */
static int
read_conversions(
	const char *text,
	int *conversions)
{
	const char *cursor;
	size_t length;
	int bit;

	/* Takes each name between commas. */
	cursor = text;
	for (;;) {
		length = strcspn(cursor, ",");
		bit = find_name(dd_conversions, sizeof(dd_conversions) / sizeof(dd_conversions[0]), cursor, length);
		if (bit < 0)
			return -1;
		*conversions |= bit;

		/* The end of the list, or the next name. */
		cursor += length;
		if (*cursor == '\0')
			break;
		cursor++;
	}

	/* Succeeded: the conversions are read. */
	return 0;
}

/* Allocates the input block, the output block and the record, one at a time. */
static int
allocate_blocks(
	struct dd_state *state)
{
	const struct dd_options *options;

	/* The input block. */
	options = state->options;
	state->in = malloc(options->ibs);
	if (state->in == NULL)
		return -1;

	/* The output block, with room for what one input block may add. */
	state->out = malloc(options->obs + options->ibs + options->cbs + 2);
	if (state->out == NULL)
		return -1;

	/* The record of block and unblock. */
	state->record = malloc(options->cbs + 2);
	if (state->record == NULL)
		return -1;

	/* Succeeded: the blocks are allocated. */
	return 0;
}

/*
 * Opens the input and the output.  The output is created if need be and,
 * unless conv=notrunc, truncated after the seek= blocks.
 */
static int
open_files(
	struct dd_state *state)
{
	const struct dd_options *options;
	struct stat status_of_output;
	int flags;
	int status;
	int regular;

	/* The input: standard input unless if= names a file. */
	options = state->options;
	state->input = STDIN_FILENO;
	if (options->input_path != NULL) {
		state->input = open(options->input_path, O_RDONLY);
		if (state->input < 0) {
			fprintf(stderr, "dd: %s: %s\n", options->input_path, strerror(errno));
			return -1;
		}
	}

	/* The output: standard output unless of= names a file. */
	state->output = STDOUT_FILENO;
	if (options->output_path == NULL)
		return 0;
	flags = O_WRONLY | O_CREAT;
	if (!(options->conversions & DD_NOTRUNC) && options->seek == 0)
		flags |= O_TRUNC;
	state->output = open(options->output_path, flags, 0666);
	if (state->output < 0) {
		fprintf(stderr, "dd: %s: %s\n", options->output_path, strerror(errno));
		return -1;
	}

	/* A seek without notrunc cuts a regular file after the seek. */
	if (options->conversions & DD_NOTRUNC)
		return 0;
	if (options->seek == 0)
		return 0;
	status = fstat(state->output, &status_of_output);
	if (status != 0)
		return 0;
	regular = S_ISREG(status_of_output.st_mode);
	if (!regular)
		return 0;
	status = ftruncate(state->output, (off_t)(options->seek * options->obs));
	if (status != 0) {
		fprintf(stderr, "dd: %s: %s\n", options->output_path, strerror(errno));
		return -1;
	}

	/* Succeeded: both are open. */
	return 0;
}

/* Skips the skip= input blocks: by seeking, or by reading them. */
static int
skip_input(
	struct dd_state *state)
{
	const struct dd_options *options;
	unsigned long long block;
	off_t moved;
	ssize_t got;

	/* Nothing to skip. */
	options = state->options;
	if (options->skip == 0)
		return 0;

	/* A seekable input is moved. */
	moved = lseek(state->input, (off_t)(options->skip * options->ibs), SEEK_CUR);
	if (moved >= 0)
		return 0;

	/* Anything else is read block by block. */
	for (block = 0; block < options->skip; block++) {
		got = read(state->input, state->in, options->ibs);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			fprintf(stderr, "dd: %s: %s\n", input_name(options), strerror(errno));
			return -1;
		}

		/* The end of the input ends the skipping. */
		if (got == 0)
			break;
	}

	/* Succeeded: the blocks are skipped. */
	return 0;
}

/*
 * Moves past the seek= output blocks: by seeking, or on a pipe by writing
 * null bytes in their place.
 */
static int
seek_output(
	struct dd_state *state)
{
	const struct dd_options *options;
	unsigned long long block;
	off_t moved;
	int status;

	/* Nothing to seek. */
	options = state->options;
	if (options->seek == 0)
		return 0;

	/* A seekable output is moved. */
	moved = lseek(state->output, (off_t)(options->seek * options->obs), SEEK_CUR);
	if (moved >= 0)
		return 0;

	/* Anything else gets null blocks. */
	memset(state->out, 0, options->obs);
	for (block = 0; block < options->seek; block++) {
		status = write_all(state->output, state->out, options->obs);
		if (status != 0) {
			fprintf(stderr, "dd: %s: %s\n", output_name(options), strerror(errno));
			return -1;
		}
	}

	/* Succeeded: the output is positioned. */
	return 0;
}

/* Reads, converts and writes input blocks until the end or count=. */
static int
copy_blocks(
	struct dd_state *state)
{
	const struct dd_options *options;
	size_t length;
	ssize_t got;
	int status;
	int pad;

	/* One input block at a time. */
	options = state->options;
	for (;;) {
		if (options->count_given && state->in_full + state->in_partial >= options->count)
			break;

		/* Reads one block with one read. */
		got = read(state->input, state->in, options->ibs);
		if (got < 0 && errno == EINTR && !dd_interrupted)
			continue;
		if (dd_interrupted)
			return 0;
		if (got < 0) {
			fprintf(stderr, "dd: %s: %s\n", input_name(options), strerror(errno));
			if (!(options->conversions & DD_NOERROR))
				return -1;

			/* noerror goes on, with a whole padded block for sync. */
			state->in_partial++;
			if (!(options->conversions & DD_SYNC))
				continue;
			got = 0;
		} else if (got == 0) {
			break;
		} else if ((size_t)got == options->ibs) {
			state->in_full++;
		} else {
			state->in_partial++;
		}

		/* The length of the block read. */
		length = (size_t)got;

		/* sync pads a short block, with spaces for block and unblock. */
		if ((options->conversions & DD_SYNC) && length < options->ibs) {
			pad = 0;
			if (options->conversions & (DD_BLOCK | DD_UNBLOCK))
				pad = ' ';
			memset(state->in + length, pad, options->ibs - length);
			length = options->ibs;
		}

		/* bs= without conversions writes each block as it is. */
		if (options->bs_given && !(options->conversions & DD_CHANGES)) {
			status = write_all(state->output, state->in, length);
			if (status != 0) {
				fprintf(stderr, "dd: %s: %s\n", output_name(options), strerror(errno));
				return -1;
			}

			/* Counts it as a full or a partial block. */
			if (length == options->obs)
				state->out_full++;
			else
				state->out_partial++;
			continue;
		}

		/* Converts the block and hands it to the output. */
		convert_block(state, state->in, length);
		status = 0;
		if (options->conversions & (DD_BLOCK | DD_UNBLOCK))
			status = emit_record(state, state->in, length);
		else
			status = emit(state, state->in, length);
		if (status != 0)
			return -1;
	}

	/* Succeeded: the input is used up. */
	return 0;
}

/*
 * Converts a block in place: swaps byte pairs, translates to ASCII, and
 * changes case.  The conversions that change the length (block,
 * unblock) and the translation from ASCII happen as the bytes are
 * emitted.
 */
static void
convert_block(
	struct dd_state *state,
	unsigned char *block,
	size_t length)
{
	const struct dd_options *options;
	unsigned char byte;
	size_t index;

	/* swab swaps each pair; an odd last byte stays. */
	options = state->options;
	if (options->conversions & DD_SWAB) {
		for (index = 0; index + 1 < length; index += 2) {
			byte = block[index];
			block[index] = block[index + 1];
			block[index + 1] = byte;
		}
	}

	/* ascii translates the EBCDIC input first. */
	if (options->conversions & DD_ASCII) {
		for (index = 0; index < length; index++)
			block[index] = dd_ebcdic_to_ascii[block[index]];
	}

	/* Case conversion works on ASCII. */
	if (options->conversions & DD_LCASE) {
		for (index = 0; index < length; index++) {
			if (block[index] >= 'A' && block[index] <= 'Z')
				block[index] = (unsigned char)(block[index] - 'A' + 'a');
		}
	}

	/* Or to upper case. */
	if (options->conversions & DD_UCASE) {
		for (index = 0; index < length; index++) {
			if (block[index] >= 'a' && block[index] <= 'z')
				block[index] = (unsigned char)(block[index] - 'a' + 'A');
		}
	}
}

/*
 * Turns converted bytes into records: block makes each newline-terminated
 * line a cbs record padded with spaces, cutting a longer line and
 * counting it; unblock makes each cbs record a line without its trailing
 * spaces.
 */
static int
emit_record(
	struct dd_state *state,
	const unsigned char *bytes,
	size_t length)
{
	const struct dd_options *options;
	size_t index;
	unsigned char byte;
	int status;

	/* Takes each byte in turn. */
	options = state->options;
	for (index = 0; index < length; index++) {
		byte = bytes[index];

		/* unblock: a full record becomes a line. */
		if (options->conversions & DD_UNBLOCK) {
			state->record[state->column] = byte;
			state->column++;
			if (state->column == options->cbs) {
				status = finish_record(state);
				if (status != 0)
					return -1;
			}

			/* The byte went into the record. */
			continue;
		}

		/* block: a newline ends the record. */
		if (byte == '\n') {
			status = finish_record(state);
			if (status != 0)
				return -1;
			continue;
		}

		/* The rest of a line too long for its record is dropped. */
		if (state->cutting)
			continue;
		if (state->column == options->cbs) {
			state->truncated++;
			state->cutting = 1;
			continue;
		}

		/* The byte joins the record. */
		state->record[state->column] = byte;
		state->column++;
	}

	/* Succeeded: the bytes are in records. */
	return 0;
}

/*
 * Ends the record being built: block pads it to cbs with spaces, unblock
 * drops its trailing spaces and adds a newline.  Then it is emitted.
 */
static int
finish_record(
	struct dd_state *state)
{
	const struct dd_options *options;
	size_t length;
	int status;

	/* unblock: the record without trailing spaces, and a newline. */
	options = state->options;
	if (options->conversions & DD_UNBLOCK) {
		length = state->column;
		while (length > 0 && state->record[length - 1] == ' ')
			length--;
		state->record[length] = '\n';
		status = emit(state, state->record, length + 1);
		state->column = 0;
		if (status != 0)
			return -1;
		return 0;
	}

	/* block: the record padded to its size. */
	memset(state->record + state->column, ' ', options->cbs - state->column);
	status = emit(state, state->record, options->cbs);
	state->column = 0;
	state->cutting = 0;
	if (status != 0)
		return -1;

	/* Succeeded: the record was emitted. */
	return 0;
}

/*
 * Hands converted bytes to the output: translated to EBCDIC for ebcdic
 * and ibm, and written in full obs blocks.
 */
static int
emit(
	struct dd_state *state,
	const unsigned char *bytes,
	size_t length)
{
	const struct dd_options *options;
	size_t index;
	unsigned char byte;
	int status;

	/* Each byte, translated when asked. */
	options = state->options;
	for (index = 0; index < length; index++) {
		byte = bytes[index];
		if (options->conversions & DD_EBCDIC)
			byte = dd_ascii_to_ebcdic[byte];
		else if (options->conversions & DD_IBM)
			byte = dd_ascii_to_ibm[byte];
		state->out[state->out_used] = byte;
		state->out_used++;

		/* A full block goes out. */
		if (state->out_used == options->obs) {
			status = flush_output(state, 0);
			if (status != 0)
				return -1;
		}
	}

	/* Succeeded: the bytes are handed over. */
	return 0;
}

/* Writes the output held: a full block, or at the end a partial one. */
static int
flush_output(
	struct dd_state *state,
	int final)
{
	const struct dd_options *options;
	int status;

	/* Nothing held. */
	options = state->options;
	if (state->out_used == 0)
		return 0;

	/* Only the end writes a partial block. */
	if (state->out_used < options->obs && !final)
		return 0;

	/* Writes it and counts it. */
	status = write_all(state->output, state->out, state->out_used);
	if (status != 0) {
		fprintf(stderr, "dd: %s: %s\n", output_name(options), strerror(errno));
		return -1;
	}

	/* Counts it as a full or a partial block. */
	if (state->out_used == options->obs)
		state->out_full++;
	else
		state->out_partial++;
	state->out_used = 0;
	return 0;
}

/* Writes a whole buffer, however many writes it takes. */
static int
write_all(
	int descriptor,
	const unsigned char *bytes,
	size_t length)
{
	ssize_t written;
	size_t offset;

	/* Writes until every byte is out. */
	offset = 0;
	while (offset < length) {
		written = write(descriptor, bytes + offset, length - offset);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		/* Goes on after what was written. */
		offset += (size_t)written;
	}

	/* Succeeded: the whole buffer was written. */
	return 0;
}

/* Reports the blocks read and written, and the records cut. */
static void
report(
	const struct dd_state *state)
{
	/* Whole and partial blocks in and out. */
	fprintf(stderr, "%llu+%llu records in\n", state->in_full, state->in_partial);
	fprintf(stderr, "%llu+%llu records out\n", state->out_full, state->out_partial);

	/* The records block cut short. */
	if (state->truncated == 1)
		fprintf(stderr, "1 truncated record\n");
	else if (state->truncated > 1)
		fprintf(stderr, "%llu truncated records\n", state->truncated);
}

/* Notes SIGINT for the copy loop. */
static void
on_interrupt(
	int number)
{
	/* The loop sees the interrupted read and ends. */
	(void)number;
	dd_interrupted = 1;
}

/* Names the input in diagnostics. */
static const char *
input_name(
	const struct dd_options *options)
{
	/* The file, or standard input. */
	if (options->input_path != NULL)
		return options->input_path;
	return "standard input";
}

/* Names the output in diagnostics. */
static const char *
output_name(
	const struct dd_options *options)
{
	/* The file, or standard output. */
	if (options->output_path != NULL)
		return options->output_path;
	return "standard output";
}
