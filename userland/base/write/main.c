/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD write userland command.
 */

#include "userland/base/common/command.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>

#ifndef WRITE_UTMP_PATH
#define WRITE_UTMP_PATH _PATH_UTMP
#endif

static int find_terminal(const char *user, const char *requested, uid_t recipient, char selected[UT_LINESIZE + 1U], char path[256]);
static int record_name(const char field[UT_NAMESIZE], char result[UT_NAMESIZE + 1U]);
static int record_line(const char field[UT_LINESIZE], char result[UT_LINESIZE + 1U]);
static int terminal_open(const char *line, uid_t recipient, char *path, size_t capacity);
static int line_safe(const char *line);
static int send_message(int terminal);
static const char *sender_name(char buffer[64]);

/*
 * Runs the write command.
 */
int
main(
	int argc,
	char **argv)
{
	int saved_errno;
	const char *requested;
	struct passwd recipient;
	struct passwd *found;
	char passwd_buffer[2048];
	char selected[UT_LINESIZE + 1U] = "";
	char path[256];
	int lookup_error;
	int terminal;

	requested = NULL;

	/* Validates the command-line arguments. */
	if (argc != 2 && argc != 3) {
		fprintf(stderr, "usage: write user [terminal]\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Validates the command-line arguments. */
	if (argc == 3) {
		requested = argv[2];

		/* Selects the matching prefix. */
		if (strncmp(requested, "/dev/", 5) == 0)
			requested += 5;
	}
	lookup_error = getpwnam_r(argv[1], &recipient, passwd_buffer,
				  sizeof(passwd_buffer), &found);

	/* Handles an operation failure. */
	if (lookup_error != 0) {
		fprintf(stderr, "write: %s: %s\n", argv[1],
			strerror(lookup_error));

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the found availability. */
	if (found == NULL) {
		fprintf(stderr, "write: %s: unknown user\n", argv[1]);

		/* Reports operation failure. */
		return 1;
	}
	terminal =
	    find_terminal(argv[1], requested, recipient.pw_uid, selected, path);

	/* Checks the terminal state. */
	if (terminal < 0) {
		fprintf(stderr, "write: %s: no writable terminal: %s\n",
			argv[1], strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed send message operation. */
	if (send_message(terminal) != 0) {
		saved_errno = errno;

		(void)close(terminal);
		errno = saved_errno;
		fprintf(stderr, "write: %s: %s\n", path, strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed close operation. */
	if (close(terminal) != 0) {
		fprintf(stderr, "write: %s: %s\n", path, strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the find terminal operation. */
static int
find_terminal(
	const char *user,
	const char *requested,
	uid_t recipient,
	char selected[UT_LINESIZE + 1U],
	char path[256])
{
	int function_result;
	char candidate_path[256];
	int descriptor;
	size_t offset;
	struct utmpx entry;
	char name[UT_NAMESIZE + 1U];
	char line[UT_LINESIZE + 1U];
	int input;
	int found;
	ssize_t count;

	found = 0;

	input = open(WRITE_UTMP_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	/* Validates the current input. */
	if (input < 0)
		return -1;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		offset = 0;

		/* Process each remaining element. */
		while (offset < sizeof(entry)) {
			count = read(input, (char *)&entry + offset,
				     sizeof(entry) - offset);

			/* Handles the reported system error. */
			if (count < 0 && errno == EINTR)
				continue;

			/* Checks the remaining item count. */
			if (count <= 0)
				break;
			offset += (size_t)count;
		}

		/* Checks the current offset. */
		if (offset == 0)
			break;

		/* Checks the current offset. */
		if (offset != sizeof(entry)) {
			errno = EINVAL;
			found = -1;
			break;
		}

		/* Handles a failed record name operation. */
		if (entry.ut_type != USER_PROCESS ||
		    !record_name(entry.ut_user, name) ||
		    !record_line(entry.ut_line, line) ||
		    strcmp(name, user) != 0 ||
		    (requested != NULL && strcmp(line, requested) != 0))
			continue;

		descriptor = terminal_open(line, recipient, candidate_path,
				  sizeof(candidate_path));

		/* Checks the file descriptor. */
		if (descriptor < 0)
			continue;
		(void)close(descriptor);

		/* Handles the found condition. */
		if (!found || strcmp(line, selected) < 0) {
			(void)strcpy(selected, line);
			(void)strcpy(path, candidate_path);
			found = 1;
		}
	}

	/* Handles a failed close operation. */
	if (close(input) != 0 && found >= 0)
		return -1;

	/* Handles the found condition. */
	if (found <= 0) {
		/* Handles the found condition. */
		if (found == 0)
			errno = ENOENT;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the terminal open result. */
	function_result = terminal_open(selected, recipient, path, 256);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the record name operation. */
static int
record_name(
	const char field[UT_NAMESIZE],
	char result[UT_NAMESIZE + 1U])
{
	size_t length;

	length = strnlen(field, UT_NAMESIZE);

	/* Checks the current data length. */
	if (length == UT_NAMESIZE)
		return 0;
	memcpy(result, field, length);
	result[length] = '\0';

	/* Reports operation failure. */
	return 1;
}

/* Supports the record line operation. */
static int
record_line(
	const char field[UT_LINESIZE],
	char result[UT_LINESIZE + 1U])
{
	size_t length;

	length = strnlen(field, UT_LINESIZE);

	/* Checks the current data length. */
	if (length == UT_LINESIZE)
		return 0;
	memcpy(result, field, length);
	result[length] = '\0';

	/* Reports operation failure. */
	return 1;
}

/* Supports the terminal open operation. */
static int
terminal_open(
	const char *line,
	uid_t recipient,
	char *path,
	size_t capacity)
{
	struct stat status;
	int descriptor;
	int length;

	length = snprintf(path, capacity, "/dev/%s", line);

	/* Handles a failed line safe operation. */
	if (!line_safe(line) || length < 0 || (size_t)length >= capacity) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed stat operation. */
	if (stat(path, &status) != 0 || !S_ISCHR(status.st_mode) ||
	    status.st_uid != recipient || !(status.st_mode & S_IWGRP)) {
		errno = EACCES;

		/* Reports operation failure. */
		return -1;
	}
	descriptor = open(path, O_WRONLY | O_NOCTTY | O_CLOEXEC | O_NOFOLLOW);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Handles a failed isatty operation. */
	if (!isatty(descriptor)) {
		(void)close(descriptor);
		errno = ENOTTY;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return descriptor;
}

/* Supports the line safe operation. */
static int
line_safe(
	const char *line)
{
	const char *slash;
	const char *component;
	size_t length;

	component = line;

	/* Handles the line condition. */
	if (*line == '/' || *line == '\0')
		return 0;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		slash = strchr(component, '/');
		length = slash == NULL ? strlen(component)
				       : (size_t)(slash - component);

		/* Checks the current data length. */
		if (length == 0 || (length == 1 && component[0] == '.') ||
		    (length == 2 && component[0] == '.' && component[1] == '.'))

			/* Reports successful completion. */
			return 0;

		/* Handles the slash availability. */
		if (slash == NULL)
			return 1;
		component = slash + 1;
	}
}

/* Supports the send message operation. */
static int
send_message(
	int terminal)
{
	int function_result;
	ssize_t count;
	char sender_buffer[64];
	char host[64] = "unknown";
	char source[64] = "unknown";
	char time_text[32] = "unknown";
	char header[320];
	const char *sender;
	char *source_name;
	time_t now;
	struct tm *local;
	int length;
	char buffer[1024];

	sender = sender_name(sender_buffer);
	source_name = ttyname(STDIN_FILENO);
	now = time(NULL);
	local = localtime(&now);

	/* Handles a failed gethostname operation. */
	if (gethostname(host, sizeof(host)) != 0)
		(void)strcpy(host, "unknown");
	host[sizeof(host) - 1U] = '\0';

	/* Handles the source name availability. */
	if (source_name != NULL) {
		/* Selects the matching prefix. */
		if (strncmp(source_name, "/dev/", 5) == 0)
			source_name += 5;
		(void)snprintf(source, sizeof(source), "%s", source_name);
	}

	/* Handles the local availability. */
	if (local != NULL)
		(void)strftime(time_text, sizeof(time_text), "%H:%M", local);
	length = snprintf(header, sizeof(header),
			  "\r\nMessage from %s@%s on %s at %s ...\r\n", sender,
			  host, source, time_text);

	/* Handles a failed command write all operation. */
	if (length < 0 || (size_t)length >= sizeof(header) ||
	    command_write_all(terminal, header, (size_t)length) != 0)

		/* Reports operation failure. */
		return -1;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		count = read(STDIN_FILENO, buffer, sizeof(buffer));

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count < 0)
			return -1;

		/* Checks the remaining item count. */
		if (count == 0)
			break;

		/* Handles a failed command write all operation. */
		if (command_write_all(terminal, buffer, (size_t)count) != 0)
			return -1;
	}

	/* Obtains the command write all result. */
	function_result = command_write_all(terminal, "\r\nEOF\r\n", 7);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the sender name operation. */
static const char *
sender_name(
	char buffer[64])
{
	const char *login;
	struct passwd *account;

	login = getlogin();

	/* Handles the login availability. */
	if (login != NULL && *login != '\0')
		return login;
	account = getpwuid(getuid());

	/* Handles the account availability. */
	if (account != NULL && account->pw_name != NULL)
		return account->pw_name;
	(void)snprintf(buffer, 64, "%u", (unsigned)getuid());

	/* Returns the computed result. */
	return buffer;
}
