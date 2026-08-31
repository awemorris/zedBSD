/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD at userland command.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define AT_SPOOL "/var/spool/at"

static const char *program_name(const char *path);
static int list_jobs(void);
static int job_owned(const char *name);
static int remove_jobs(int argc, char **argv, int first);
static int parse_time(int argc, char **argv, int first, time_t *result);
static int submit_job(FILE *input, time_t when, char queue);

/*
 * Runs the at command.
 */
int
main(
	int argc,
	char **argv)
{
	const char *name, *file;
	char queue;
	FILE *input;
	time_t when;
	int first, status;

	name = program_name(argv[0]);
	file = NULL;
	queue = strcmp(name, "batch") == 0 ? 'b' : 'a';
	input = stdin;
	first = 1;

	/* Lists the caller's jobs when requested without operands. */
	if (first < argc && strcmp(argv[first], "-l") == 0) {
		/* Validates the command-line arguments. */
		if (first + 1 != argc) {
			/* Reports an invalid list request. */
			return 2;
		}

		status = list_jobs();

		/* Returns the listing status. */
		return status;
	}

	/* Removes the caller's selected jobs. */
	if (first < argc && strcmp(argv[first], "-r") == 0) {
		/* Validates the command-line arguments. */
		if (first + 1 == argc) {
			/* Reports a remove request without a job. */
			return 2;
		}

		status = remove_jobs(argc, argv, first + 1);

		/* Returns the removal status. */
		return status;
	}

	/* Selects the requested input file. */
	if (first + 1 < argc && strcmp(argv[first], "-f") == 0) {
		file = argv[first + 1];
		first += 2;
	}

	/* Parses the requested execution time. */
	if (parse_time(argc, argv, first, &when) != 0) {
		fprintf(stderr,
			"usage: %s [-f file] time | at -l | at -r job ...\n",
			name);

		/* Reports operation failure. */
		return 2;
	}

	/* Opens the requested input file. */
	if (file != NULL && (input = fopen(file, "r")) == NULL) {
		fprintf(stderr, "at: %s: %s\n", file, strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Submits the completed job. */
	status = submit_job(input, when, queue);

	/* Propagates a failed submission. */
	if (status != 0)
		return status;

	/* Reports a successful submission. */
	return 0;
}

/* Supports the program name operation. */
static const char *
program_name(
	const char *path)
{
	const char *slash;

	slash = strrchr(path, '/');

	/* Returns the computed result. */
	return slash != NULL ? slash + 1 : path;
}

/* Supports the list jobs operation. */
static int
list_jobs(
	void)
{
	DIR *directory;
	struct dirent *entry;

	directory = opendir(AT_SPOOL);

	/* Handles the directory availability. */
	if (directory == NULL)
		return errno == ENOENT ? 0 : 1;

	/* Process each directory entry. */
	while ((entry = readdir(directory)) != NULL)

		/* Handles a failed job owned operation. */
		if (entry->d_name[0] != '.' && job_owned(entry->d_name))
			puts(entry->d_name);
	closedir(directory);

	/* Reports successful completion. */
	return 0;
}

/* Supports the job owned operation. */
static int
job_owned(
	const char *name)
{
	int function_result;
	char path[320];
	struct stat status;

	/* Handles a failed strchr operation. */
	if (strchr(name, '/') != NULL ||
	    snprintf(path, sizeof(path), "%s/%s", AT_SPOOL, name) >=
		(int)sizeof(path) ||
	    stat(path, &status) != 0)

		/* Reports successful completion. */
		return 0;

	/* Computes the function result. */
	function_result = geteuid() == 0 || status.st_uid == getuid();

	/* Returns the computed result. */
	return function_result;
}

/* Supports the remove jobs operation. */
static int
remove_jobs(
	int argc,
	char **argv,
	int first)
{
	char path[320];
	int failed, index;

	/* Process each remaining command-line operand. */
	failed = 0;
	for (index = first; index < argc; index++) {
		/* Validates the command-line arguments. */
		if (!job_owned(argv[index]) ||
		    snprintf(path, sizeof(path), "%s/%s", AT_SPOOL,
			     argv[index]) >= (int)sizeof(path) ||
		    unlink(path) != 0) {
			fprintf(stderr, "at: cannot remove %s\n", argv[index]);
			failed = 1;
		}
	}

	/* Returns the computed result. */
	return failed;
}

/* Supports the parse time operation. */
static int
parse_time(
	int argc,
	char **argv,
	int first,
	time_t *result)
{
	char *end_local;
	char text_local[32], *colon_local, *end_local1;
	long count;
	unsigned long hour, minute;
	struct tm broken;
	time_t now;

	now = time(NULL);

	/* Validates the command-line arguments. */
	if (first == argc) {
		*result = now;
		/* Reports successful completion. */
		return 0;
	}

	/* Handles the selected command-line operation. */
	if (strcmp(argv[first], "now") == 0) {
		/* Validates the command-line arguments. */
		if (first + 1 == argc) {
			*result = now;
			/* Reports successful completion. */
			return 0;
		}

		/* Handles the selected command-line operation. */
		if (first + 4 == argc && strcmp(argv[first + 1], "+") == 0) {

						count = strtol(argv[first + 2], &end_local, 10);
			long scale =
			    strcmp(argv[first + 3], "hours") == 0 ||
				    strcmp(argv[first + 3], "hour") == 0
				? 3600
				: 60;

			/* Validates the command-line arguments. */
			if (*end_local == '\0' && count >= 0 &&
			    (strstr(argv[first + 3], "minute") != NULL ||
			     strstr(argv[first + 3], "hour") != NULL)) {
				*result = now + count * scale;
				/* Reports successful completion. */
				return 0;
			}
		}
	}

	/* Validates the command-line arguments. */
	if (first + 1 != argc || strlen(argv[first]) >= sizeof(text_local))
		return -1;
	strcpy(text_local, argv[first]);
	colon_local = strchr(text_local, ':');

	/* Handles the colon local availability. */
	if (colon_local == NULL)
		return -1;
	*colon_local++ = '\0';
	hour = strtoul(text_local, &end_local1, 10);

	/* Handles the text local condition. */
	if (*text_local == '\0' || *end_local1 != '\0')
		return -1;
	minute = strtoul(colon_local, &end_local1, 10);

	/* Handles a failed localtime r operation. */
	if (*colon_local == '\0' || *end_local1 != '\0' || hour > 23 ||
	    minute > 59 || localtime_r(&now, &broken) == NULL)

		/* Reports operation failure. */
		return -1;
	broken.tm_hour = (int)hour;
	broken.tm_min = (int)minute;
	broken.tm_sec = 0;
	*result = mktime(&broken);
	/* Checks the operation result. */
	if (*result < now)
		*result += 24 * 60 * 60;
	/* Returns the computed result. */
	return *result == (time_t)-1 ? -1 : 0;
}

/* Supports the submit job operation. */
static int
submit_job(
	FILE *input,
	time_t when,
	char queue)
{
	char path[320], buffer[1024];
	int descriptor;
	FILE *output;

	(void)mkdir("/var/spool", 0755);
	(void)mkdir(AT_SPOOL, 01777);

	/* Handles a failed snprintf operation. */
	if (snprintf(path, sizeof(path), "%s/%lld.%lu.%ld.%c", AT_SPOOL,
		     (long long)when, (unsigned long)getuid(), (long)getpid(),
		     queue) >= (int)sizeof(path))

		/* Reports operation failure. */
		return 1;
	descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

	/* Handles a failed fdopen operation. */
	if (descriptor < 0 || (output = fdopen(descriptor, "w")) == NULL)
		return 1;
	fprintf(output, "# zedBSD at job\n");

	/* Process input until it is exhausted. */
	while (fgets(buffer, sizeof(buffer), input) != NULL)

		/* Handles the end-of-file condition. */
		if (fputs(buffer, output) == EOF)
			break;

	/* Handles an operation failure. */
	if (ferror(input) || ferror(output) || fflush(output) != 0 ||
	    fsync(descriptor) != 0 || fclose(output) != 0) {
		unlink(path);

		/* Reports operation failure. */
		return 1;
	}
	printf("job %s at %lld\n", strrchr(path, '/') + 1, (long long)when);

	/* Reports successful completion. */
	return 0;
}
