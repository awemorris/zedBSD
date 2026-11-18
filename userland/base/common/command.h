/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares shared userland command support.
 */

#ifndef KERN_USERLAND_COMMON_COMMAND_H
#define KERN_USERLAND_COMMON_COMMAND_H

#include <stddef.h>
#include <stdio.h>
#include <time.h>

/* How an option written in full takes its value. */
#define COMMAND_VALUE_NONE	0	/* no value: --name */
#define COMMAND_VALUE_REQUIRED	1	/* --name=value or --name value */
#define COMMAND_VALUE_OPTIONAL	2	/* only --name=value */

/* What command_options_next reports besides an option's code. */
#define COMMAND_OPTION_END	(-1)	/* the options are over */
#define COMMAND_OPTION_NUMBER	(-2)	/* -NUM: the digits are the value */
#define COMMAND_OPTION_ERROR	'?'	/* unknown, ambiguous or missing a value */

/*
 * One option written in full (--name), as the GNU utilities take them.
 *
 * A table of these ends with a NULL name.  The code is what the scan
 * reports for the option, usually the letter of its short form, so that
 * both forms share one case of the caller's switch.
 */
struct command_long_option {
	const char *name;
	int value;
	int code;
};

/*
 * A scan of a command line in the manner of the GNU utilities.
 *
 * The caller fills in the arguments, the letters (as getopt takes them: a
 * letter, then ':' for a value that is required and '::' for one that may
 * only be attached, as in -i.bak), the table of long options and whether
 * -NUM is an option, then calls command_options_start.  Each call to
 * command_options_next reports one option and its value; operands met on
 * the way are set aside in argv, in order, so that when the scan ends they
 * are operands[0] to operands[operand_count - 1].  Options after operands
 * are still options (the GNU order), unless POSIXLY_CORRECT is set in the
 * environment, when the first operand ends them as POSIX has it.
 */
struct command_options {
	int argc;
	char **argv;
	const char *program;
	const char *letters;
	const struct command_long_option *names;
	int numbers;

	/* Whether options may follow operands. */
	int permute;

	/* The next argument, and the rest of a run of letters (-abc). */
	int index;
	const char *cursor;

	/* The value of the option just reported, or NULL. */
	const char *value;

	/* The digits of -NUM, as a string. */
	char number[24];

	/* The operands, gathered in argv from index 1 on. */
	char **operands;
	int operand_count;
};

void command_options_start(struct command_options *scan);
int command_options_next(struct command_options *scan);
int command_parse_date(const char *text, const struct timespec *now, int utc, struct timespec *moment);

int command_write_all(int descriptor, const void *data, size_t length);
int command_copy_fd(int input, int output);
int command_parse_ull(const char *text, unsigned long long *result);
int command_parse_mode(const char *text, unsigned *result);
void command_error(const char *command, const char *operand);
long command_read_line(FILE *stream, char **line, size_t *capacity);
int command_exec(const char *name, char *const argv[]);

#endif
