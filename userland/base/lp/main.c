/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD lp userland command.
 */

#include "userland/base/lp/lpd-client.h"

#include "userland/base/common/command.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define PRINT_NAME_MAX 255

struct print_options {
	const char *destination;
	const char *title;
	unsigned copies;
	int mail;
	int silent;
	int lpr_mode;
};

static const char *print_program_name(const char *path);
static int print_parse_options(int argc, char **argv, struct print_options *options);
static int print_parse_copies(const char *text, unsigned *result);
static void print_usage(const char *program, int lpr_mode);
static int print_get_host(char output[PRINT_NAME_MAX + 1]);
static int print_safe_identity(char output[PRINT_NAME_MAX + 1], const char *input, const char *fallback);
static int print_get_user(char output[PRINT_NAME_MAX + 1]);
static int print_submit_one(const struct print_options *options, const struct lpd_destination *destination, const char *host, const char *user, const char *path, unsigned sequence);
static const char *print_source_name(const char *path);
static int print_stage_input(const char *path, int *descriptor, uint64_t *size);

/*
 * Submits PDF operands through the selected command frontend.
 */
int
main(
	int argc,
	char **argv)
{
	struct print_options options;
	struct lpd_destination destination;
	char host[PRINT_NAME_MAX + 1];
	char user[PRINT_NAME_MAX + 1];
	const char *program;
	unsigned sequence;
	int first_operand;
	int failed;
	int status;
	int index;

	program = print_program_name(argc > 0 ? argv[0] : NULL);

	/* Handles a failed signal operation. */
	if (signal(SIGPIPE, (void (*)(int))SIG_IGN) == SIG_ERR) {
		command_error(program, "SIGPIPE");

		/* Reports operation failure. */
		return 1;
	}
	memset(&options, 0, sizeof(options));
	options.copies = 1;
	options.lpr_mode = strcmp(program, "lpr") == 0;

	first_operand = print_parse_options(argc, argv, &options);

	/* Handles the first operand condition. */
	if (first_operand < 0) {
		print_usage(program, options.lpr_mode);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the destination availability. */
	if (options.destination == NULL || *options.destination == '\0') {
		fprintf(stderr, "%s: no print destination is configured\n", program);

		/* Reports operation failure. */
		return 1;
	}

	status = lpd_parse_destination(options.destination, &destination);

	/* Checks the operation status. */
	if (status != 0) {
		fprintf(stderr, "%s: invalid destination: %s\n", program,
		    options.destination);

		/* Reports operation failure. */
		return 1;
	}

	status = print_get_host(host);

	/* Checks the operation status. */
	if (status != 0) {
		command_error(program, "host name");

		/* Reports operation failure. */
		return 1;
	}
	status = print_get_user(user);

	/* Checks the operation status. */
	if (status != 0) {
		command_error(program, "user name");

		/* Reports operation failure. */
		return 1;
	}

	failed = 0;
	sequence = (unsigned)getpid();

	/* Validates the command-line arguments. */
	if (first_operand == argc) {
		status = print_submit_one(
			&options,
			&destination,
			host,
			user,
			NULL,
			sequence);

		/* Checks the operation status. */
		if (status != 0) {
			command_error(program, "standard input");
			failed = 1;
		} else if (!options.lpr_mode && !options.silent) {
			status = printf(
				"request id is %s-%03u\n",
				destination.queue,
				sequence % 1000U);

			/* Checks the operation status. */
			if (status < 0)
				failed = 1;
		}
	} else {
		/* Submit every operand as an independent job. */
		for (index = first_operand; index < argc; index++) {
			status = print_submit_one(
				&options,
				&destination,
				host,
				user,
				argv[index],
				sequence);

			/* Checks the operation status. */
			if (status != 0) {
				command_error(program, argv[index]);
				failed = 1;
			} else if (!options.lpr_mode && !options.silent) {
				status = printf(
					"request id is %s-%03u\n",
					destination.queue,
					sequence % 1000U);

				/* Checks the operation status. */
				if (status < 0)
					failed = 1;
			}
			sequence++;
		}
	}

	/* Handles the end-of-file condition. */
	if (fflush(stdout) == EOF)
		failed = 1;

	/* Returns the computed result. */
	return failed;
}

/* Returns the final component of a command path. */
static const char *
print_program_name(
	const char *path)
{
	const char *slash;

	/* Handles the path availability. */
	if (path == NULL || *path == '\0')
		return "lp";

	slash = strrchr(path, '/');

	/* Handles the slash availability. */
	if (slash == NULL)
		return path;

	/* Returns the computed result. */
	return slash + 1;
}

/* Parses lp or lpr command-line options. */
static int
print_parse_options(
	int argc,
	char **argv,
	struct print_options *options)
{
	const char *environment;
	const char *program;
	int option;
	int status;

	program = print_program_name(argc > 0 ? argv[0] : NULL);
	opterr = 0;

	/* Checks the selected options. */
	if (options->lpr_mode) {
		/* Parse the BSD-compatible convenience frontend. */
		while ((option = getopt(argc, argv, "#:J:mP:")) != -1) {
			/* Dispatch the selected command-line option. */
			switch (option) {
			case '#':
				status = print_parse_copies(optarg, &options->copies);

				/* Checks the operation status. */
				if (status != 0)
					return -1;
				break;
			case 'J':
				options->title = optarg;
				break;
			case 'm':
				options->mail = 1;
				break;
			case 'P':
				options->destination = optarg;
				break;
			default:
				/* Reports operation failure. */
				return -1;
			}
		}
	} else {
		/* Parse the POSIX lp frontend. */
		while ((option = getopt(argc, argv, "cd:mn:o:st:w")) != -1) {
			/* Dispatch the selected command-line option. */
			switch (option) {
			case 'c':
				break;
			case 'd':
				options->destination = optarg;
				break;
			case 'm':
				options->mail = 1;
				break;
			case 'n':
				status = print_parse_copies(optarg, &options->copies);

				/* Checks the operation status. */
				if (status != 0)
					return -1;
				break;
			case 'o':
				/* Selects the matching value. */
				if (strcmp(optarg, "raw") != 0) {
					fprintf(
						stderr,
						"%s: unsupported printer option: %s\n",
						program,
						optarg);

					/* Reports operation failure. */
					return -1;
				}
				break;
			case 's':
				options->silent = 1;
				break;
			case 't':
				options->title = optarg;
				break;
			case 'w':
				fprintf(
					stderr,
					"%s: -w requires a print-completion service\n",
					program);

				/* Reports operation failure. */
				return -1;
			default:
				/* Reports operation failure. */
				return -1;
			}
		}
	}

	/* Handles the destination availability. */
	if (options->destination == NULL) {
		/* Checks the selected options. */
		if (options->lpr_mode) {
			environment = getenv("PRINTER");
		} else {
			environment = getenv("LPDEST");

			/* Handles the environment availability. */
			if (environment == NULL || *environment == '\0')
				environment = getenv("PRINTER");
		}
		options->destination = environment;
	}

	/* Returns the computed result. */
	return optind;
}

/* Parses a bounded positive copy count. */
static int
print_parse_copies(
	const char *text,
	unsigned *result)
{
	unsigned long long value;
	int status;

	status = command_parse_ull(text, &value);

	/* Checks the operation status. */
	if (status != 0 || value == 0 || value > 999ULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	*result = (unsigned)value;

	/* Reports successful completion. */
	return 0;
}

/* Prints the selected frontend's usage synopsis. */
static void
print_usage(
	const char *program,
	int lpr_mode)
{
	/* Handles the lpr mode condition. */
	if (lpr_mode) {
		fprintf(
			stderr,
			"usage: %s [-m] [-P host[:port]/queue] "
			"[-# copies] [-J title] [file ...]\n",
			program);
	} else {
		fprintf(
			stderr,
			"usage: %s [-cms] [-d host[:port]/queue] "
			"[-n copies] [-o raw] [-t title] [file ...]\n",
			program);
	}
}

/* Reads and sanitizes the local host name. */
static int
print_get_host(
	char output[PRINT_NAME_MAX + 1])
{
	int function_result;
	char host[PRINT_NAME_MAX + 1];
	int status;

	status = gethostname(host, sizeof(host));

	/* Checks the operation status. */
	if (status != 0)
		return -1;
	host[PRINT_NAME_MAX] = '\0';

	/* Obtains the print safe identity result. */
	function_result = print_safe_identity(output, host, "zedbsd");

	/* Returns the computed result. */
	return function_result;
}

/* Converts an identity to a safe LPD token. */
static int
print_safe_identity(
	char output[PRINT_NAME_MAX + 1],
	const char *input,
	const char *fallback)
{
	size_t index;
	unsigned char value;

	/* Handles the input availability. */
	if (input == NULL || *input == '\0')
		input = fallback;

	/* Handles the input availability. */
	if (input == NULL || *input == '\0') {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Copy a bounded printable token and replace unsafe bytes. */
	for (index = 0; input[index] != '\0' && index < PRINT_NAME_MAX; index++) {
		value = (unsigned char)input[index];

		/* Validates the current value. */
		if (value < 0x21U || value > 0x7eU || value == '/')
			output[index] = '_';
		else
			output[index] = (char)value;
	}

	/* Validates the current input. */
	if (input[index] != '\0') {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}

	output[index] = '\0';

	/* Reports successful completion. */
	return 0;
}

/* Resolves and sanitizes the submitting user name. */
static int
print_get_user(
	char output[PRINT_NAME_MAX + 1])
{
	int function_result;
	struct passwd *account;
	char numeric[32];
	int length;

	account = getpwuid(getuid());

	/* Handles the account availability. */
	if (account != NULL && account->pw_name != NULL) {
		/* Obtains the print safe identity result. */
		function_result = print_safe_identity(output, account->pw_name, "user");

		/* Returns the computed result. */
		return function_result;
	}

	length = snprintf(numeric, sizeof(numeric), "%lu", (unsigned long)getuid());

	/* Checks the current data length. */
	if (length < 0 || (size_t)length >= sizeof(numeric)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the print safe identity result. */
	function_result = print_safe_identity(output, numeric, "user");

	/* Returns the computed result. */
	return function_result;
}

/* Stages and submits one command operand. */
static int
print_submit_one(
	const struct print_options *options,
	const struct lpd_destination *destination,
	const char *host,
	const char *user,
	const char *path,
	unsigned sequence)
{
	char source[PRINT_NAME_MAX + 1];
	char title[PRINT_NAME_MAX + 1];
	const char *source_input;
	const char *title_input;
	uint64_t size;
	int descriptor;
	int status;
	int saved;

	source_input = print_source_name(path);
	status = print_safe_identity(source, source_input, "document.pdf");

	/* Checks the operation status. */
	if (status != 0)
		return -1;

	title_input = options->title == NULL ? source_input : options->title;
	status = print_safe_identity(title, title_input, "document");

	/* Checks the operation status. */
	if (status != 0)
		return -1;

	status = print_stage_input(path, &descriptor, &size);

	/* Checks the operation status. */
	if (status != 0)
		return -1;

	status = lpd_submit(
		destination,
		descriptor,
		size,
		host,
		user,
		title,
		source,
		options->copies,
		options->mail,
		sequence);
	saved = errno;

	/* Handles a failed close operation. */
	if (close(descriptor) != 0 && status == 0) {
		status = -1;
		saved = errno;
	}
	errno = saved;

	/* Returns the computed result. */
	return status;
}

/* Returns a display name for one input operand. */
static const char *
print_source_name(
	const char *path)
{
	const char *slash;

	/* Handles the path availability. */
	if (path == NULL || strcmp(path, "-") == 0)
		return "stdin.pdf";

	slash = strrchr(path, '/');

	/* Handles the slash availability. */
	if (slash == NULL)
		return path;

	/* Handles the slash condition. */
	if (slash[1] == '\0')
		return "document.pdf";

	/* Returns the computed result. */
	return slash + 1;
}

/* Copies one input into an unlinked staging file and validates PDF magic. */
static int
print_stage_input(
	const char *path,
	int *descriptor,
	uint64_t *size)
{
	unsigned char buffer[4096];
	unsigned char magic[5];
	char temporary[] = "/tmp/zedbsd-lp.XXXXXX";
	uint64_t total;
	ssize_t count;
	int source;
	int staging;
	int source_owned;
	int status;
	int saved;

	/* Handles the path availability. */
	if (path == NULL || strcmp(path, "-") == 0) {
		source = STDIN_FILENO;
		source_owned = 0;
	} else {
		source = open(path, O_RDONLY);

		/* Handles the source condition. */
		if (source < 0)
			return -1;
		source_owned = 1;
	}

	staging = mkstemp(temporary);

	/* Handles the staging condition. */
	if (staging < 0) {
		saved = errno;

		/* Handles the source owned condition. */
		if (source_owned)
			close(source);
		errno = saved;

		/* Reports operation failure. */
		return -1;
	}
	status = unlink(temporary);

	/* Checks the operation status. */
	if (status != 0) {
		saved = errno;
		close(staging);

		/* Handles the source owned condition. */
		if (source_owned)
			close(source);
		errno = saved;

		/* Reports operation failure. */
		return -1;
	}

	total = 0;

	/* Stage all input so the exact LPD byte count is known. */
	for (;;) {
		count = read(source, buffer, sizeof(buffer));

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count < 0)
			break;

		/* Checks the remaining item count. */
		if (count == 0)
			break;

		/* Handles the total condition. */
		if (UINT64_MAX - total < (uint64_t)count) {
			errno = EOVERFLOW;
			count = -1;
			break;
		}

		status = command_write_all(staging, buffer, (size_t)count);

		/* Checks the operation status. */
		if (status != 0) {
			count = -1;
			break;
		}
		total += (uint64_t)count;
	}

	saved = errno;

	/* Handles a failed close operation. */
	if (source_owned && close(source) != 0 && count >= 0) {
		count = -1;
		saved = errno;
	}

	/* Checks the remaining item count. */
	if (count < 0) {
		close(staging);
		errno = saved;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed lseek operation. */
	if (lseek(staging, 0, SEEK_SET) < 0) {
		saved = errno;
		close(staging);
		errno = saved;

		/* Reports operation failure. */
		return -1;
	}
	count = read(staging, magic, sizeof(magic));

	/* Checks the remaining item count. */
	if (count != (ssize_t)sizeof(magic) || memcmp(magic, "%PDF-", 5) != 0) {
		close(staging);
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed lseek operation. */
	if (lseek(staging, 0, SEEK_SET) < 0) {
		saved = errno;
		close(staging);
		errno = saved;

		/* Reports operation failure. */
		return -1;
	}

	*descriptor = staging;
	*size = total;

	/* Reports successful completion. */
	return 0;
}
