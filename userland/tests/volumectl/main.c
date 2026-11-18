/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The volumed probe (ws132-p004): lists the volumes, or asks for a mount
 * or an eject, printing volumed's lines as they come.
 *
 *   volumectl list            the VOLUME lines and DONE
 *   volumectl mount ID        ... and the RESULT of the mount
 *   volumectl eject ID        ... and the RESULT of the eject
 *
 * The exit status is 0, or the errno of the RESULT (1 when volumed cannot
 * be reached).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* volumed's socket and the longest line. */
#define VOLUMECTL_SOCKET	"/run/volumed.sock"
#define VOLUMECTL_LINE_MAX	512U

static int volumectl_line(int descriptor, char *line, size_t size);

int
main(
	int argc,
	char **argv)
{
	struct sockaddr_un address;
	char line[VOLUMECTL_LINE_MAX];
	char request[128];
	const char *word;
	int descriptor;
	int status;
	int result;
	int same;

	/* The command. */
	word = NULL;
	if (argc == 2) {
		same = strcmp(argv[1], "list");
		if (same == 0)
			word = "list";
	}

	/* A request names the volume. */
	if (argc == 3) {
		same = strcmp(argv[1], "mount");
		if (same == 0)
			word = "MOUNT";
		same = strcmp(argv[1], "eject");
		if (same == 0)
			word = "EJECT";
	}

	/* Anything else. */
	if (word == NULL) {
		fprintf(stderr, "usage: volumectl list | mount ID | eject ID\n");
		return 2;
	}

	/* volumed. */
	descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
	if (descriptor < 0)
		return 1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", VOLUMECTL_SOCKET);
	status = connect(descriptor, (struct sockaddr *)&address, sizeof(address));
	if (status != 0) {
		printf("VOLUMECTL error=%d step=connect\n", errno);
		return 1;
	}

	/* The greeting, and the volumes until DONE. */
	(void)write(descriptor, "HELLO 1\n", 8U);
	for (;;) {
		status = volumectl_line(descriptor, line, sizeof(line));
		if (status != 0)
			return 1;
		printf("%s\n", line);
		same = strcmp(line, "DONE");
		if (same == 0)
			break;
	}

	/* A list ends there. */
	same = strcmp(word, "list");
	if (same == 0)
		return 0;

	/* The request, and the lines until its RESULT. */
	status = snprintf(request, sizeof(request), "%s 1 %s\n", word, argv[2]);
	(void)write(descriptor, request, (size_t)status);
	for (;;) {
		status = volumectl_line(descriptor, line, sizeof(line));
		if (status != 0)
			return 1;
		printf("%s\n", line);
		same = strncmp(line, "RESULT ", 7U);
		if (same == 0)
			break;
	}

	/* The RESULT's errno is the exit status. */
	result = 0;
	(void)sscanf(line, "RESULT %*u %d", &result);
	(void)close(descriptor);
	return result;
}

/* Reads one line (without its newline); 0, or 1 when the connection ended. */
static int
volumectl_line(
	int descriptor,
	char *line,
	size_t size)
{
	size_t used;
	ssize_t count;
	char character;

	/* Byte by byte up to the newline. */
	used = 0U;
	for (;;) {
		count = read(descriptor, &character, 1U);
		if (count != 1)
			return 1;
		if (character == '\n')
			break;
		if (used + 1U < size) {
			line[used] = character;
			used++;
		}
	}

	/* Succeeded. */
	line[used] = '\0';
	return 0;
}
