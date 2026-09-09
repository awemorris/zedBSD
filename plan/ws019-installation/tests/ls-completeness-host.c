/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static unsigned reads;
static int read_failure, close_failure;
static struct dirent *checked_readdir(DIR *directory);
static int checked_closedir(DIR *directory);
#define main tested_ls_main
#define readdir checked_readdir
#define closedir checked_closedir
#include "userland/base/ls/main.c"
#undef main
#undef readdir
#undef closedir

static struct dirent *checked_readdir(DIR *directory)
{
	reads++;
	if (read_failure && reads == 2) {
		errno = EIO;
		return NULL;
	}
	return readdir(directory);
}
static int checked_closedir(DIR *directory)
{
	int error;
	error = closedir(directory);
	if (close_failure) {
		errno = EIO;
		return -1;
	}
	return error;
}
int main(int argc, char **argv)
{
	char *arguments[] = { "ls", "-a1", "--", NULL, NULL };
	FILE *capture;
	int saved, result, full;
	if (argc != 2)
		return 2;
	arguments[3] = argv[1];
	saved = dup(STDOUT_FILENO);
	capture = tmpfile();
	if (saved < 0 || capture == NULL || dup2(fileno(capture), STDOUT_FILENO) < 0)
		abort();
	read_failure = 1;
	result = tested_ls_main(4, arguments);
	if (result != 1 || ftell(capture) != 0)
		abort();
	read_failure = 0;
	close_failure = 1;
	result = tested_ls_main(4, arguments);
	if (result != 1 || ftell(capture) != 0)
		abort();
	close_failure = 0;
	result = tested_ls_main(4, arguments);
	if (result != 0 || ftell(capture) == 0)
		abort();
	full = open("/dev/full", O_WRONLY);
	if (full < 0 || dup2(full, STDOUT_FILENO) < 0)
		abort();
	result = tested_ls_main(4, arguments);
	if (result != 1)
		abort();
	clearerr(stdout);
	if (dup2(saved, STDOUT_FILENO) < 0)
		abort();
	close(saved);
	close(full);
	fclose(capture);
	puts("ls completeness PASS directory-read directory-close normal output-failure");
	return 0;
}
