/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/common/uucodec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	int base64 = 0;
	int index = 1;
	int input = STDIN_FILENO;
	unsigned mode = 0666;
	struct stat status;

	if (index < argc && strcmp(argv[index], "-m") == 0) {
		base64 = 1;
		index++;
	}
	if (index < argc && strcmp(argv[index], "--") == 0)
		index++;
	if (argc - index == 2) {
		input = open(argv[index], O_RDONLY | O_CLOEXEC);
		if (input < 0) {
			fprintf(stderr, "uuencode: %s: %s\n", argv[index],
				strerror(errno));
			return 1;
		}
		if (fstat(input, &status) == 0)
			mode = (unsigned)status.st_mode;
		index++;
	} else if (argc - index != 1) {
		fprintf(stderr, "usage: uuencode [-m] [file] decode_path\n");
		return 2;
	}
	if (uu_encode_fd(input, base64, mode, argv[index]) != 0) {
		fprintf(stderr, "uuencode: %s\n", strerror(errno));
		if (input != STDIN_FILENO)
			(void)close(input);
		return 1;
	}
	if (input != STDIN_FILENO && close(input) != 0) {
		fprintf(stderr, "uuencode: %s\n", strerror(errno));
		return 1;
	}
	return 0;
}
