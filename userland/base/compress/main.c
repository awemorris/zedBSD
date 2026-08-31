/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD compress userland command.
 */

#include "userland/base/common/lzw.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct options {
	unsigned max_bits;
	int standard_output;
	int force;
	int verbose;
	int decompress;
};

static const char *program;

static const char *base_name(const char *path);
static void usage(void);
static int transform_stream(int input, int output, const struct options *options);
static void error_path(const char *path);
static int transform_file(const char *input_name, const struct options *options);
static char *output_name(const char *input, int decompress);

/*
 * Runs the compress command.
 */
int
main(
	int argc,
	char **argv)
{
	char *end;
	unsigned long value;
	int i_index_for;
	struct options options = {.max_bits = 16};
	int ch, failed;

	failed = 0;
	program = base_name(argv[0]);
	options.decompress =
	    !strcmp(program, "uncompress") || !strcmp(program, "zcat");

	/* Selects the matching value. */
	if (!strcmp(program, "zcat"))

	/* Parse each command-line option. */
		options.standard_output = 1;
	while ((ch = getopt(argc, argv,
			    options.decompress ? "cfv" : "b:cfv")) != -1) {
		/* Dispatch the selected operation case. */
		switch (ch) {
		case 'b':

		value = strtoul(optarg, &end, 10);

		/* Checks the current endpoint. */
		if (*end || value < 9 || value > 16) {
			usage();

			/* Reports operation failure. */
			return 2;
		}
		options.max_bits = (unsigned)value;
		break;
		case 'c':
			options.standard_output = 1;
			break;
		case 'f':
			options.force = 1;
			break;
		case 'v':
			options.verbose = 1;
			break;
		default:
			usage();

			/* Reports operation failure. */
			return 2;
		}
	}

	/* Validates the command-line arguments. */
	if (optind == argc) {
		/* Handles the transform stream condition. */
		if (transform_stream(STDIN_FILENO, STDOUT_FILENO, &options)) {
			error_path("standard input");

			/* Reports operation failure. */
			return 1;
		}

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining command-line operand. */
	for (i_index_for = optind; i_index_for < argc; i_index_for++)
		failed |= transform_file(argv[i_index_for], &options) == 1;

	/* Returns the computed result. */
	return failed;
}

/* Supports the base name operation. */
static const char *
base_name(
	const char *path)
{
	const char *slash;

	slash = strrchr(path, '/');

	/* Returns the computed result. */
	return slash ? slash + 1 : path;
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(stderr, "usage: %s [-cfv] [-b bits] [file ...]\n", program);
}

/* Supports the transform stream operation. */
static int
transform_stream(
	int input,
	int output,
	const struct options *options)
{
	int function_result;

	/* Computes the function result. */
	function_result = options->decompress
		   ? lzw_decompress(input, output)
		   : lzw_compress(input, output, options->max_bits, 1);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the error path operation. */
static void
error_path(
	const char *path)
{
	fprintf(stderr, "%s: %s: %s\n", program, path, strerror(errno));
}

/* Supports the transform file operation. */
static int
transform_file(
	const char *input_name,
	const struct options *options)
{
	struct stat input_stat, output_stat;
	char *name, *temporary;
	int input, output, result, saved;
	size_t length;

	name = NULL;
	temporary = NULL;
	input = -1;
	output = -1;
	result = 1;

	input = open(input_name, O_RDONLY);

	/* Handles a failed fstat operation. */
	if (input < 0 || fstat(input, &input_stat))
		goto fail;

	/* Handles a failed S ISREG operation. */
	if (!S_ISREG(input_stat.st_mode)) {
		errno = EINVAL;
		goto fail;
	}

	/* Checks the selected options. */
	if (options->standard_output) {
		/* Handles the transform stream condition. */
		if (transform_stream(input, STDOUT_FILENO, options))
			goto fail;
		close(input);

		/* Reports successful completion. */
		return 0;
	}
	name = output_name(input_name, options->decompress);

	/* Validates the current name. */
	if (!name)
		goto fail;

	/* Handles a failed access operation. */
	if (!options->force && !access(name, F_OK)) {
		errno = EEXIST;
		goto fail;
	}
	length = strlen(name);

	/* Checks the current data length. */
	if (length > SIZE_MAX - 16) {
		errno = ENAMETOOLONG;
		goto fail;
	}
	temporary = malloc(length + 16);

	/* Handles the temporary condition. */
	if (!temporary)
		goto fail;
	snprintf(temporary, length + 16, "%s.tmp.XXXXXX", name);
	output = mkstemp(temporary);

	/* Handles the output condition. */
	if (output < 0)
		goto fail;

	/* Handles the fchmod condition. */
	if (fchmod(output, input_stat.st_mode & 07777) ||
	    transform_stream(input, output, options) || fsync(output) ||
	    fstat(output, &output_stat))
		goto fail;

	/* Checks the selected options. */
	if (!options->decompress && !options->force &&
	    output_stat.st_size >= input_stat.st_size) {
		/* Checks the selected options. */
		if (options->verbose)
			fprintf(stderr, "%s: %s unchanged; no space saved\n",
				program, input_name);
		close(output);
		output = -1;
		unlink(temporary);
		close(input);
		free(name);
		free(temporary);

		/* Reports operation failure. */
		return 2;
	}

	/* Handles the close condition. */
	if (close(output)) {
		output = -1;
		goto fail;
	}
	output = -1;

	/* Handles the rename condition. */
	if (rename(temporary, name) || unlink(input_name))
		goto fail;

	/* Checks the selected options. */
	if (options->verbose)
		fprintf(stderr, "%s: %s -> %s\n", program, input_name, name);
	result = 0;
	goto out;
fail:
	saved = errno;
	error_path(input_name);

	/* Handles the output condition. */
	if (output >= 0)
		close(output);

	/* Handles the temporary condition. */
	if (temporary)
		unlink(temporary);
	errno = saved;
out:

	/* Validates the current input. */
	if (input >= 0)
		close(input);
	free(name);
	free(temporary);

	/* Returns the computed result. */
	return result;
}

/* Supports the output name operation. */
static char *
output_name(
	const char *input,
	int decompress)
{
	size_t length;
	char *name;

	length = strlen(input);

	/* Handles the decompress condition. */
	if (decompress) {
		/* Checks the current data length. */
		if (length < 2 || strcmp(input + length - 2, ".Z")) {
			errno = EINVAL;

			/* Reports that no result is available. */
			return NULL;
		}
		name = malloc(length - 1);

		/* Validates the current name. */
		if (!name)
			return NULL;
		memcpy(name, input, length - 2);
		name[length - 2] = '\0';
	} else {
		/* Checks the current data length. */
		if (length > SIZE_MAX - 3) {
			errno = ENAMETOOLONG;

			/* Reports that no result is available. */
			return NULL;
		}
		name = malloc(length + 3);

		/* Validates the current name. */
		if (!name)
			return NULL;
		snprintf(name, length + 3, "%s.Z", input);
	}

	/* Returns the computed result. */
	return name;
}
