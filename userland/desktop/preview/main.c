/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * keiland-preview's program (WS168 p003; preview.h): the arguments read,
 * fd 0 and fd 1 checked, the system's confinement entered (nothing on
 * zedBSD, where the process starts in its sandbox), then the preview made
 * (make.c).  The exit status tells the caller what
 * happened (PREVIEW_*); nothing is printed (fd 2 is not there).
 */

#include "preview.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv);
static int main_parse(int argc, char **argv, struct preview_request *request);
static int main_number(const char *text, int *number);

/*
 * Makes the preview of fd 0 into fd 1.
 */
int
main(
	int argc,
	char **argv)
{
	struct preview_request request;
	struct stat status;
	int regular;
	int error;

	/* The arguments. */
	error = main_parse(argc, argv, &request);
	if (error != 0)
		return PREVIEW_USAGE;

	/* The input and the output: regular files (fstat before the confinement, which may not allow it). */
	error = fstat(0, &status);
	regular = error == 0 && S_ISREG(status.st_mode);
	if (!regular)
		return PREVIEW_USAGE;
	error = fstat(1, &status);
	regular = error == 0 && S_ISREG(status.st_mode);
	if (!regular)
		return PREVIEW_NO_OUTPUT;

	/* The confinement, before a byte of the input is read. */
	error = preview_confine();
	if (error != 0)
		return PREVIEW_NO_SANDBOX;

	/* Gives libpdf the fonts carried in the program; without one a document's text is not drawn, as before. */
	(void)preview_fonts_register();

	/* The preview. */
	return preview_make(0, 1, &request);
}

/* Reads the arguments; 0, or -1 when they are wrong. */
static int
main_parse(
	int argc,
	char **argv,
	struct preview_request *request)
{
	const char *newline;
	size_t length;
	int index;
	int same;
	int error;

	/* Nothing yet. */
	memset(request, 0, sizeof(*request));

	/* Each argument. */
	for (index = 1; index < argc; index++) {
		/* The size. */
		same = strncmp(argv[index], "--width=", 8U);
		if (same == 0) {
			error = main_number(argv[index] + 8, &request->width);
			if (error != 0)
				return -1;
			continue;
		}

		/* The height. */
		same = strncmp(argv[index], "--height=", 9U);
		if (same == 0) {
			error = main_number(argv[index] + 9, &request->height);
			if (error != 0)
				return -1;
			continue;
		}

		/* The fit. */
		same = strcmp(argv[index], "--fit=cover");
		if (same == 0) {
			request->cover = 1;
			continue;
		}

		/* Contain. */
		same = strcmp(argv[index], "--fit=contain");
		if (same == 0) {
			request->cover = 0;
			continue;
		}

		/* A test build's escape. */
		same = strncmp(argv[index], "--test-escape=", 14U);
		if (same == 0 && PREVIEW_TEST_ESCAPE) {
			same = strcmp(argv[index] + 14, "open");
			if (same == 0)
				request->escape = PREVIEW_ESCAPE_OPEN;
			same = strcmp(argv[index] + 14, "socket");
			if (same == 0)
				request->escape = PREVIEW_ESCAPE_SOCKET;
			same = strcmp(argv[index] + 14, "fork");
			if (same == 0)
				request->escape = PREVIEW_ESCAPE_FORK;
			if (request->escape == PREVIEW_ESCAPE_NONE)
				return -1;
			continue;
		}

		/* The stamp: one line that fits. */
		same = strncmp(argv[index], "--stamp=", 8U);
		if (same != 0)
			return -1;
		length = strlen(argv[index] + 8);
		newline = strchr(argv[index] + 8, '\n');
		if (length >= sizeof(request->stamp) || newline != NULL)
			return -1;
		memcpy(request->stamp, argv[index] + 8, length + 1U);
	}

	/* Both sides were given. */
	if (request->width == 0 || request->height == 0)
		return -1;
	return 0;
}

/* Reads a side: 1 to PREVIEW_SIDE_MAX; 0, or -1. */
static int
main_number(
	const char *text,
	int *number)
{
	char *end;
	long value;

	/* Decimal digits only. */
	value = strtol(text, &end, 10);
	if (end == text || *end != '\0' || value < 1L || value > (long)PREVIEW_SIDE_MAX)
		return -1;
	*number = (int)value;
	return 0;
}
