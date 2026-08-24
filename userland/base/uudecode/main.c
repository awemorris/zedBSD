/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/common/uucodec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	const char *output = NULL;
	const char *input_path = NULL;
	int input = STDIN_FILENO;
	int index = 1;

	if (index < argc && strcmp(argv[index], "-o") == 0) {
		if (++index >= argc)
			goto usage;
		output = argv[index++];
	}
	if (index < argc && strcmp(argv[index], "--") == 0)
		index++;
	if (index < argc)
		input_path = argv[index++];
	if (index != argc)
		goto usage;
	if (input_path != NULL && strcmp(input_path, "-") != 0) {
		input = open(input_path, O_RDONLY | O_CLOEXEC);
		if (input < 0) {
			fprintf(stderr, "uudecode: %s: %s\n", input_path,
				strerror(errno));
			return 1;
		}
	}
	if (uu_decode_fd(input, output) != 0) {
		fprintf(stderr, "uudecode: %s\n", strerror(errno));
		if (input != STDIN_FILENO)
			(void)close(input);
		return 1;
	}
	if (input != STDIN_FILENO && close(input) != 0) {
		fprintf(stderr, "uudecode: %s\n", strerror(errno));
		return 1;
	}
	return 0;

usage:
	fprintf(stderr, "usage: uudecode [-o output] [file]\n");
	return 2;
}
