/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static const char *
base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static void
error_path(const char *path)
{
	fprintf(stderr, "%s: %s: %s\n", program, path, strerror(errno));
}

static char *
output_name(const char *input, int decompress)
{
	size_t length = strlen(input);
	char *name;
	if (decompress) {
		if (length < 2 || strcmp(input + length - 2, ".Z")) {
			errno = EINVAL;
			return NULL;
		}
		name = malloc(length - 1);
		if (!name)
			return NULL;
		memcpy(name, input, length - 2);
		name[length - 2] = '\0';
	} else {
		if (length > SIZE_MAX - 3) {
			errno = ENAMETOOLONG;
			return NULL;
		}
		name = malloc(length + 3);
		if (!name)
			return NULL;
		snprintf(name, length + 3, "%s.Z", input);
	}
	return name;
}

static int
transform_stream(int input, int output, const struct options *options)
{
	return options->decompress
		   ? lzw_decompress(input, output)
		   : lzw_compress(input, output, options->max_bits, 1);
}

static int
transform_file(const char *input_name, const struct options *options)
{
	struct stat input_stat, output_stat;
	char *name = NULL, *temporary = NULL;
	int input = -1, output = -1, result = 1, saved;
	size_t length;

	input = open(input_name, O_RDONLY);
	if (input < 0 || fstat(input, &input_stat))
		goto fail;
	if (!S_ISREG(input_stat.st_mode)) {
		errno = EINVAL;
		goto fail;
	}
	if (options->standard_output) {
		if (transform_stream(input, STDOUT_FILENO, options))
			goto fail;
		close(input);
		return 0;
	}
	name = output_name(input_name, options->decompress);
	if (!name)
		goto fail;
	if (!options->force && !access(name, F_OK)) {
		errno = EEXIST;
		goto fail;
	}
	length = strlen(name);
	if (length > SIZE_MAX - 16) {
		errno = ENAMETOOLONG;
		goto fail;
	}
	temporary = malloc(length + 16);
	if (!temporary)
		goto fail;
	snprintf(temporary, length + 16, "%s.tmp.XXXXXX", name);
	output = mkstemp(temporary);
	if (output < 0)
		goto fail;
	if (fchmod(output, input_stat.st_mode & 07777) ||
	    transform_stream(input, output, options) || fsync(output) ||
	    fstat(output, &output_stat))
		goto fail;
	if (!options->decompress && !options->force &&
	    output_stat.st_size >= input_stat.st_size) {
		if (options->verbose)
			fprintf(stderr, "%s: %s unchanged; no space saved\n",
				program, input_name);
		close(output);
		output = -1;
		unlink(temporary);
		close(input);
		free(name);
		free(temporary);
		return 2;
	}
	if (close(output)) {
		output = -1;
		goto fail;
	}
	output = -1;
	if (rename(temporary, name) || unlink(input_name))
		goto fail;
	if (options->verbose)
		fprintf(stderr, "%s: %s -> %s\n", program, input_name, name);
	result = 0;
	goto out;
fail:
	saved = errno;
	error_path(input_name);
	if (output >= 0)
		close(output);
	if (temporary)
		unlink(temporary);
	errno = saved;
out:
	if (input >= 0)
		close(input);
	free(name);
	free(temporary);
	return result;
}

static void
usage(void)
{
	fprintf(stderr, "usage: %s [-cfv] [-b bits] [file ...]\n", program);
}

int
main(int argc, char **argv)
{
	struct options options = {.max_bits = 16};
	int ch, failed = 0;
	program = base_name(argv[0]);
	options.decompress =
	    !strcmp(program, "uncompress") || !strcmp(program, "zcat");
	if (!strcmp(program, "zcat"))
		options.standard_output = 1;
	while ((ch = getopt(argc, argv,
			    options.decompress ? "cfv" : "b:cfv")) != -1) {
		switch (ch) {
		case 'b': {
			char *end;
			unsigned long value = strtoul(optarg, &end, 10);
			if (*end || value < 9 || value > 16) {
				usage();
				return 2;
			}
			options.max_bits = (unsigned)value;
			break;
		}
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
			return 2;
		}
	}
	if (optind == argc) {
		if (transform_stream(STDIN_FILENO, STDOUT_FILENO, &options)) {
			error_path("standard input");
			return 1;
		}
		return 0;
	}
	for (int i = optind; i < argc; i++)
		failed |= transform_file(argv[i], &options) == 1;
	return failed;
}
