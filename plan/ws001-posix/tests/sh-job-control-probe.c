/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int
checkpoint(const char *path, const char *name)
{
	pid_t group = getpgrp();
	pid_t foreground = tcgetpgrp(STDIN_FILENO);
	char record[192];
	int descriptor, length;

	length = snprintf(record, sizeof(record),
			  "%s pid=%ld pgrp=%ld foreground=%ld equal=%d\n", name,
			  (long)getpid(), (long)group, (long)foreground,
			  foreground == group);
	if (length < 0 || (size_t)length >= sizeof(record))
		return 0;
	descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (descriptor < 0)
		return 0;
	if (write(descriptor, record, (size_t)length) != length) {
		(void)close(descriptor);
		return 0;
	}
	(void)close(descriptor);
	return foreground == group;
}

static int
read_one_line(void)
{
	char byte;
	ssize_t count;

	do
		count = read(STDIN_FILENO, &byte, 1);
	while (count < 0 && errno == EINTR);
	while (count == 1 && byte != '\n') {
		do
			count = read(STDIN_FILENO, &byte, 1);
		while (count < 0 && errno == EINTR);
	}
	return count == 1;
}

int
main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr,
			"usage: %s pipeline|fg|background CHECKPOINT-LOG\n",
			argv[0]);
		return 2;
	}
	if (strcmp(argv[1], "pipeline") == 0) {
		int foreground = checkpoint(argv[2], "PIPELINE_CHECK");
		puts("P014-PIPELINE-READY");
		(void)fflush(stdout);
		if (!read_one_line())
			return 1;
		puts("P014-PIPELINE-DONE");
		return foreground ? 0 : 1;
	}
	if (strcmp(argv[1], "fg") == 0) {
		if (!checkpoint(argv[2], "FG_INITIAL_CHECK"))
			return 1;
		if (raise(SIGTSTP) != 0)
			return 1;
		if (!checkpoint(argv[2], "FG_RESUMED_CHECK"))
			return 1;
		puts("P014-FG-READY");
		(void)fflush(stdout);
		if (!read_one_line())
			return 1;
		puts("P014-FG-DONE");
		return 0;
	}
	if (strcmp(argv[1], "background") == 0) {
		int was_background =
		    !checkpoint(argv[2], "BACKGROUND_INITIAL_CHECK");

		printf("P014-BACKGROUND-READY pid=%ld\n", (long)getpid());
		(void)fflush(stdout);
		if (!read_one_line())
			return 1;
		if (!checkpoint(argv[2], "BACKGROUND_RESUMED_CHECK"))
			return 1;
		puts("P014-BACKGROUND-DONE");
		return was_background ? 0 : 1;
	}
	fprintf(stderr, "unknown probe mode: %s\n", argv[1]);
	return 2;
}
