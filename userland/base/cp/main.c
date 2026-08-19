/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *leaf(const char *path) { const char *p = strrchr(path, '/'); return p ? p + 1 : path; }
static int copy_file(const char *source, const char *destination,
    const char **failed_operand)
{
	struct stat from, to; int input = -1, output = -1, result = -1;
	*failed_operand = source;
	if (stat(source, &from))
		goto done;
	if (!S_ISREG(from.st_mode)) {
		errno = EINVAL;
		goto done;
	}
	if (stat(destination, &to) == 0 && from.st_dev == to.st_dev && from.st_ino == to.st_ino) { errno = EINVAL; goto done; }
	input = open(source, O_RDONLY); if (input < 0) goto done;
	*failed_operand = destination;
	output = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (output < 0)
		goto done;
	if (command_copy_fd(input, output) || close(output)) { output = -1; goto done; }
	output = -1; result = 0;
done:
	if (output >= 0)
		(void)close(output);
	if (input >= 0)
		(void)close(input);
	return result;
}
int main(int argc, char **argv)
{
	struct stat destination_status; int i, failed = 0, destination_is_dir;
	if (argc > 1 && !strcmp(argv[1], "--")) { argv++; argc--; }
	if (argc < 3) { fprintf(stderr, "usage: cp source... destination\n"); return 1; }
	destination_is_dir = stat(argv[argc - 1], &destination_status) == 0 && S_ISDIR(destination_status.st_mode);
	if (argc > 3 && !destination_is_dir) { fprintf(stderr, "cp: destination is not a directory\n"); return 1; }
	for (i = 1; i < argc - 1; i++) {
		char target[1024]; const char *destination = argv[argc - 1];
		if (destination_is_dir) { if (snprintf(target, sizeof(target), "%s/%s", destination, leaf(argv[i])) >= (int)sizeof(target)) { errno = ENAMETOOLONG; command_error("cp", argv[i]); failed = 1; continue; } destination = target; }
		{
			const char *failed_operand;
			if (copy_file(argv[i], destination, &failed_operand)) {
				command_error("cp", failed_operand);
				failed = 1;
			}
		}
	}
	return failed;
}
