/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD tee userland command.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct tee_output {
	const char *name;
	int descriptor;
	int active;
};

struct tee_options {
	int append;
	int ignore_interrupt;
};

static int tee_parse_options(int argc, char **argv, struct tee_options *options, int *count);
static void tee_usage(void);
static int tee_open_outputs(struct tee_output *outputs, char **names, size_t count, int append);
static int tee_write_output(struct tee_output *output, const void *data, size_t size);
static int tee_close_outputs(struct tee_output *outputs, size_t count);

/*
 * Copies standard input to standard output and every named file.
 */
int
main(
	int argc,
	char **argv)
{
	struct tee_options options;
	struct tee_output *outputs;
	unsigned char buffer[4096];
	ssize_t count;
	size_t output_count;
	size_t index;
	int first_operand;
	int operand_count;
	int stdout_active;
	int failed;
	int status;

	memset(&options, 0, sizeof(options));
	first_operand = tee_parse_options(argc, argv, &options, &operand_count);

	/* Handles the first operand condition. */
	if (first_operand < 0) {
		tee_usage();

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed signal operation. */
	if (options.ignore_interrupt &&
	    signal(SIGINT, (void (*)(int))SIG_IGN) == SIG_ERR) {
		command_error("tee", "SIGINT");

		/* Reports operation failure. */
		return 1;
	}

	/* One output per operand. */
	output_count = (size_t)operand_count;
	if (output_count > SIZE_MAX / sizeof(*outputs)) {
		errno = EOVERFLOW;
		command_error("tee", "output list");

		/* Reports operation failure. */
		return 1;
	}
	outputs = NULL;

	/* Handles the output count condition. */
	if (output_count != 0) {
		outputs = calloc(output_count, sizeof(*outputs));

		/* Handles the outputs availability. */
		if (outputs == NULL) {
			command_error("tee", "output list");

			/* Reports operation failure. */
			return 1;
		}
	}

	failed = tee_open_outputs(
		outputs,
		argv + first_operand,
		output_count,
		options.append);
	stdout_active = 1;

	/* Forward each unbuffered input chunk to every still-active output. */
	for (;;) {
		count = read(STDIN_FILENO, buffer, sizeof(buffer));

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count < 0) {
			command_error("tee", "standard input");
			failed = 1;
			break;
		}

		/* Checks the remaining item count. */
		if (count == 0)
			break;

		/* Handles the stdout active condition. */
		if (stdout_active) {
			status = command_write_all(
				STDOUT_FILENO,
				buffer,
				(size_t)count);

			/* Checks the operation status. */
			if (status != 0) {
				command_error("tee", "standard output");
				stdout_active = 0;
				failed = 1;
			}
		}

		/* A failed file must not prevent writes to later outputs. */
		for (index = 0; index < output_count; index++) {
			status = tee_write_output(
				&outputs[index],
				buffer,
				(size_t)count);

			/* Checks the operation status. */
			if (status != 0)
				failed = 1;
		}
	}

	status = tee_close_outputs(outputs, output_count);

	/* Checks the operation status. */
	if (status != 0)
		failed = 1;
	free(outputs);

	/* Returns the computed result. */
	return failed;
}

/*
 * Parses append and interrupt-handling modes, with GNU's long forms
 * (--append, --ignore-interrupts) and -p (--output-error[=MODE]), which
 * is taken: an output that fails is reported and dropped anyway.  The
 * operands are gathered from argv[1] on, count of them; returns 1, or -1
 * for an option tee does not take.
 */
static int
tee_parse_options(
	int argc,
	char **argv,
	struct tee_options *options,
	int *count)
{
	static const struct command_long_option names[] = {
		{"append", COMMAND_VALUE_NONE, 'a'},
		{"ignore-interrupts", COMMAND_VALUE_NONE, 'i'},
		{"output-error", COMMAND_VALUE_OPTIONAL, 'p'},
		{NULL, 0, 0}
	};
	struct command_options scan;
	int option;

	/* The scan of the command line. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "tee";
	scan.letters = "aip";
	scan.names = names;
	command_options_start(&scan);

	/* Accept combined and repeated standard options. */
	for (;;) {
		option = command_options_next(&scan);
		if (option == COMMAND_OPTION_END)
			break;

		/* Dispatch the selected command-line option. */
		switch (option) {
		case 'a':
			options->append = 1;
			break;
		case 'i':
			options->ignore_interrupt = 1;
			break;
		case 'p':
			/* A failed output is reported and dropped anyway. */
			break;
		default:
			/* Reports operation failure. */
			return -1;
		}
	}

	/* Succeeded: the operands follow argv[0]. */
	*count = scan.operand_count;
	return 1;
}

/* Prints the standard option synopsis. */
static void
tee_usage(
	void)
{
	fprintf(stderr, "usage: tee [-ai] [file ...]\n");
}

/* Opens every file operand while retaining failures in the final status. */
static int
tee_open_outputs(
	struct tee_output *outputs,
	char **names,
	size_t count,
	int append)
{
	size_t index;
	int flags;
	int failed;

	flags = O_WRONLY | O_CREAT;

	/* Handles the append condition. */
	if (append)
		flags |= O_APPEND;
	else
		flags |= O_TRUNC;
	failed = 0;

	/* Open operands independently so one inaccessible file is isolated. */
	for (index = 0; index < count; index++) {
		outputs[index].name = names[index];
		outputs[index].descriptor = open(names[index], flags, 0666);

		/* Handles the outputs condition. */
		if (outputs[index].descriptor < 0) {
			command_error("tee", names[index]);
			failed = 1;
		} else {
			outputs[index].active = 1;
		}
	}

	/* Returns the computed result. */
	return failed;
}

/* Writes one output or diagnoses and retires it after its first failure. */
static int
tee_write_output(
	struct tee_output *output,
	const void *data,
	size_t size)
{
	int saved;
	int status;

	/* Handles the output condition. */
	if (!output->active)
		return 0;

	status = command_write_all(output->descriptor, data, size);

	/* Checks the operation status. */
	if (status == 0)
		return 0;

	saved = errno;
	output->active = 0;
	close(output->descriptor);
	errno = saved;
	command_error("tee", output->name);

	/* Reports operation failure. */
	return -1;
}

/* Closes every remaining output and reports close-time errors. */
static int
tee_close_outputs(
	struct tee_output *outputs,
	size_t count)
{
	size_t index;
	int failed;
	int status;

	failed = 0;

	/* Close all active outputs even after an earlier close fails. */
	for (index = 0; index < count; index++) {
		/* Handles the outputs condition. */
		if (!outputs[index].active)
			continue;

		outputs[index].active = 0;
		status = close(outputs[index].descriptor);

		/* Checks the operation status. */
		if (status != 0) {
			command_error("tee", outputs[index].name);
			failed = 1;
		}
	}

	/* Returns the computed result. */
	return failed;
}
