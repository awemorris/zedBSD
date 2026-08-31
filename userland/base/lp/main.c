/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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
static void print_usage(const char *program, int lpr_mode);
static int print_parse_copies(const char *text, unsigned *result);
static int print_parse_options(int argc, char **argv, struct print_options *options);
static int print_safe_identity(char output[PRINT_NAME_MAX + 1], const char *input, const char *fallback);
static int print_get_host(char output[PRINT_NAME_MAX + 1]);
static int print_get_user(char output[PRINT_NAME_MAX + 1]);
static const char *print_source_name(const char *path);
static int print_stage_input(const char *path, int *descriptor, uint64_t *size);
static int print_submit_one(const struct print_options *options, const struct lpd_destination *destination, const char *host, const char *user, const char *path, unsigned sequence);

/* Submits PDF operands through the selected command frontend. */
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
	if (signal(SIGPIPE, (void (*)(int))SIG_IGN) == SIG_ERR) {
		command_error(program, "SIGPIPE");

		return 1;
	}
	memset(&options, 0, sizeof(options));
	options.copies = 1;
	options.lpr_mode = strcmp(program, "lpr") == 0;

	first_operand = print_parse_options(argc, argv, &options);
	if (first_operand < 0) {
		print_usage(program, options.lpr_mode);

		return 1;
	}

	if (options.destination == NULL || *options.destination == '\0') {
		fprintf(stderr, "%s: no print destination is configured\n", program);

		return 1;
	}

	status = lpd_parse_destination(options.destination, &destination);
	if (status != 0) {
		fprintf(stderr, "%s: invalid destination: %s\n", program,
		    options.destination);

		return 1;
	}

	status = print_get_host(host);
	if (status != 0) {
		command_error(program, "host name");

		return 1;
	}
	status = print_get_user(user);
	if (status != 0) {
		command_error(program, "user name");

		return 1;
	}

	failed = 0;
	sequence = (unsigned)getpid();
	if (first_operand == argc) {
		status = print_submit_one(
			&options,
			&destination,
			host,
			user,
			NULL,
			sequence);
		if (status != 0) {
			command_error(program, "standard input");
			failed = 1;
		} else if (!options.lpr_mode && !options.silent) {
			status = printf(
				"request id is %s-%03u\n",
				destination.queue,
				sequence % 1000U);
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
			if (status != 0) {
				command_error(program, argv[index]);
				failed = 1;
			} else if (!options.lpr_mode && !options.silent) {
				status = printf(
					"request id is %s-%03u\n",
					destination.queue,
					sequence % 1000U);
				if (status < 0)
					failed = 1;
			}
			sequence++;
		}
	}

	if (fflush(stdout) == EOF)
		failed = 1;

	return failed;
}

/* Returns the final component of a command path. */
static const char *
print_program_name(
	const char *path)
{
	const char *slash;

	if (path == NULL || *path == '\0')
		return "lp";

	slash = strrchr(path, '/');
	if (slash == NULL)
		return path;

	return slash + 1;
}

/* Prints the selected frontend's usage synopsis. */
static void
print_usage(
	const char *program,
	int lpr_mode)
{
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

/* Parses a bounded positive copy count. */
static int
print_parse_copies(
	const char *text,
	unsigned *result)
{
	unsigned long long value;
	int status;

	status = command_parse_ull(text, &value);
	if (status != 0 || value == 0 || value > 999ULL) {
		errno = EINVAL;

		return -1;
	}

	*result = (unsigned)value;

	return 0;
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
	if (options->lpr_mode) {
		/* Parse the BSD-compatible convenience frontend. */
		while ((option = getopt(argc, argv, "#:J:mP:")) != -1) {
			switch (option) {
			case '#':
				status = print_parse_copies(optarg, &options->copies);
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
				return -1;
			}
		}
	} else {
		/* Parse the POSIX lp frontend. */
		while ((option = getopt(argc, argv, "cd:mn:o:st:w")) != -1) {
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
				if (status != 0)
					return -1;
				break;
			case 'o':
				if (strcmp(optarg, "raw") != 0) {
					fprintf(
						stderr,
						"%s: unsupported printer option: %s\n",
						program,
						optarg);
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
				return -1;
			default:
				return -1;
			}
		}
	}

	if (options->destination == NULL) {
		if (options->lpr_mode) {
			environment = getenv("PRINTER");
		} else {
			environment = getenv("LPDEST");
			if (environment == NULL || *environment == '\0')
				environment = getenv("PRINTER");
		}
		options->destination = environment;
	}

	return optind;
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

	if (input == NULL || *input == '\0')
		input = fallback;
	if (input == NULL || *input == '\0') {
		errno = EINVAL;

		return -1;
	}

	/* Copy a bounded printable token and replace unsafe bytes. */
	for (index = 0; input[index] != '\0' && index < PRINT_NAME_MAX; index++) {
		value = (unsigned char)input[index];
		if (value < 0x21U || value > 0x7eU || value == '/')
			output[index] = '_';
		else
			output[index] = (char)value;
	}
	if (input[index] != '\0') {
		errno = ENAMETOOLONG;

		return -1;
	}

	output[index] = '\0';

	return 0;
}

/* Reads and sanitizes the local host name. */
static int
print_get_host(
	char output[PRINT_NAME_MAX + 1])
{
	char host[PRINT_NAME_MAX + 1];
	int status;

	status = gethostname(host, sizeof(host));
	if (status != 0)
		return -1;
	host[PRINT_NAME_MAX] = '\0';

	return print_safe_identity(output, host, "zedbsd");
}

/* Resolves and sanitizes the submitting user name. */
static int
print_get_user(
	char output[PRINT_NAME_MAX + 1])
{
	struct passwd *account;
	char numeric[32];
	int length;

	account = getpwuid(getuid());
	if (account != NULL && account->pw_name != NULL) {
		return print_safe_identity(output, account->pw_name, "user");
	}

	length = snprintf(numeric, sizeof(numeric), "%lu", (unsigned long)getuid());
	if (length < 0 || (size_t)length >= sizeof(numeric)) {
		errno = EOVERFLOW;

		return -1;
	}

	return print_safe_identity(output, numeric, "user");
}

/* Returns a display name for one input operand. */
static const char *
print_source_name(
	const char *path)
{
	const char *slash;

	if (path == NULL || strcmp(path, "-") == 0)
		return "stdin.pdf";

	slash = strrchr(path, '/');
	if (slash == NULL)
		return path;
	if (slash[1] == '\0')
		return "document.pdf";

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

	if (path == NULL || strcmp(path, "-") == 0) {
		source = STDIN_FILENO;
		source_owned = 0;
	} else {
		source = open(path, O_RDONLY);
		if (source < 0)
			return -1;
		source_owned = 1;
	}

	staging = mkstemp(temporary);
	if (staging < 0) {
		saved = errno;
		if (source_owned)
			close(source);
		errno = saved;

		return -1;
	}
	status = unlink(temporary);
	if (status != 0) {
		saved = errno;
		close(staging);
		if (source_owned)
			close(source);
		errno = saved;

		return -1;
	}

	total = 0;

	/* Stage all input so the exact LPD byte count is known. */
	for (;;) {
		count = read(source, buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			break;
		if (count == 0)
			break;
		if (UINT64_MAX - total < (uint64_t)count) {
			errno = EOVERFLOW;
			count = -1;
			break;
		}

		status = command_write_all(staging, buffer, (size_t)count);
		if (status != 0) {
			count = -1;
			break;
		}
		total += (uint64_t)count;
	}

	saved = errno;
	if (source_owned && close(source) != 0 && count >= 0) {
		count = -1;
		saved = errno;
	}
	if (count < 0) {
		close(staging);
		errno = saved;

		return -1;
	}

	if (lseek(staging, 0, SEEK_SET) < 0) {
		saved = errno;
		close(staging);
		errno = saved;

		return -1;
	}
	count = read(staging, magic, sizeof(magic));
	if (count != (ssize_t)sizeof(magic) || memcmp(magic, "%PDF-", 5) != 0) {
		close(staging);
		errno = EINVAL;

		return -1;
	}
	if (lseek(staging, 0, SEEK_SET) < 0) {
		saved = errno;
		close(staging);
		errno = saved;

		return -1;
	}

	*descriptor = staging;
	*size = total;

	return 0;
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
	if (status != 0)
		return -1;

	title_input = options->title == NULL ? source_input : options->title;
	status = print_safe_identity(title, title_input, "document");
	if (status != 0)
		return -1;

	status = print_stage_input(path, &descriptor, &size);
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
	if (close(descriptor) != 0 && status == 0) {
		status = -1;
		saved = errno;
	}
	errno = saved;

	return status;
}
