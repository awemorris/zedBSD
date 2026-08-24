/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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

static int
line_safe(const char *line)
{
	const char *component = line;

	if (*line == '/' || *line == '\0')
		return 0;
	for (;;) {
		const char *slash = strchr(component, '/');
		size_t length = slash == NULL ? strlen(component)
					      : (size_t)(slash - component);

		if (length == 0 || (length == 1 && component[0] == '.') ||
		    (length == 2 && component[0] == '.' && component[1] == '.'))
			return 0;
		if (slash == NULL)
			return 1;
		component = slash + 1;
	}
}

static int
terminal_open(const char *line, uid_t recipient, char *path, size_t capacity)
{
	struct stat status;
	int descriptor;
	int length;

	length = snprintf(path, capacity, "/dev/%s", line);
	if (!line_safe(line) || length < 0 || (size_t)length >= capacity) {
		errno = EINVAL;
		return -1;
	}
	if (stat(path, &status) != 0 || !S_ISCHR(status.st_mode) ||
	    status.st_uid != recipient || !(status.st_mode & S_IWGRP)) {
		errno = EACCES;
		return -1;
	}
	descriptor = open(path, O_WRONLY | O_NOCTTY | O_CLOEXEC | O_NOFOLLOW);
	if (descriptor < 0)
		return -1;
	if (!isatty(descriptor)) {
		(void)close(descriptor);
		errno = ENOTTY;
		return -1;
	}
	return descriptor;
}

static int
record_name(const char field[UT_NAMESIZE], char result[UT_NAMESIZE + 1U])
{
	size_t length = strnlen(field, UT_NAMESIZE);

	if (length == UT_NAMESIZE)
		return 0;
	memcpy(result, field, length);
	result[length] = '\0';
	return 1;
}

static int
record_line(const char field[UT_LINESIZE], char result[UT_LINESIZE + 1U])
{
	size_t length = strnlen(field, UT_LINESIZE);

	if (length == UT_LINESIZE)
		return 0;
	memcpy(result, field, length);
	result[length] = '\0';
	return 1;
}

static int
find_terminal(const char *user, const char *requested, uid_t recipient,
	      char selected[UT_LINESIZE + 1U], char path[256])
{
	struct utmpx entry;
	char name[UT_NAMESIZE + 1U];
	char line[UT_LINESIZE + 1U];
	int input;
	int found = 0;
	ssize_t count;

	input = open(WRITE_UTMP_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (input < 0)
		return -1;
	for (;;) {
		size_t offset = 0;

		while (offset < sizeof(entry)) {
			count = read(input, (char *)&entry + offset,
				     sizeof(entry) - offset);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0)
				break;
			offset += (size_t)count;
		}
		if (offset == 0)
			break;
		if (offset != sizeof(entry)) {
			errno = EINVAL;
			found = -1;
			break;
		}
		if (entry.ut_type != USER_PROCESS ||
		    !record_name(entry.ut_user, name) ||
		    !record_line(entry.ut_line, line) ||
		    strcmp(name, user) != 0 ||
		    (requested != NULL && strcmp(line, requested) != 0))
			continue;
		{
			char candidate_path[256];
			int descriptor =
			    terminal_open(line, recipient, candidate_path,
					  sizeof(candidate_path));

			if (descriptor < 0)
				continue;
			(void)close(descriptor);
			if (!found || strcmp(line, selected) < 0) {
				(void)strcpy(selected, line);
				(void)strcpy(path, candidate_path);
				found = 1;
			}
		}
	}
	if (close(input) != 0 && found >= 0)
		return -1;
	if (found <= 0) {
		if (found == 0)
			errno = ENOENT;
		return -1;
	}
	return terminal_open(selected, recipient, path, 256);
}

static const char *
sender_name(char buffer[64])
{
	const char *login = getlogin();
	struct passwd *account;

	if (login != NULL && *login != '\0')
		return login;
	account = getpwuid(getuid());
	if (account != NULL && account->pw_name != NULL)
		return account->pw_name;
	(void)snprintf(buffer, 64, "%u", (unsigned)getuid());
	return buffer;
}

static int
send_message(int terminal)
{
	char sender_buffer[64];
	char host[64] = "unknown";
	char source[64] = "unknown";
	char time_text[32] = "unknown";
	char header[320];
	const char *sender = sender_name(sender_buffer);
	char *source_name = ttyname(STDIN_FILENO);
	time_t now = time(NULL);
	struct tm *local = localtime(&now);
	int length;
	char buffer[1024];

	if (gethostname(host, sizeof(host)) != 0)
		(void)strcpy(host, "unknown");
	host[sizeof(host) - 1U] = '\0';
	if (source_name != NULL) {
		if (strncmp(source_name, "/dev/", 5) == 0)
			source_name += 5;
		(void)snprintf(source, sizeof(source), "%s", source_name);
	}
	if (local != NULL)
		(void)strftime(time_text, sizeof(time_text), "%H:%M", local);
	length = snprintf(header, sizeof(header),
			  "\r\nMessage from %s@%s on %s at %s ...\r\n", sender,
			  host, source, time_text);
	if (length < 0 || (size_t)length >= sizeof(header) ||
	    command_write_all(terminal, header, (size_t)length) != 0)
		return -1;
	for (;;) {
		ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));

		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return -1;
		if (count == 0)
			break;
		if (command_write_all(terminal, buffer, (size_t)count) != 0)
			return -1;
	}
	return command_write_all(terminal, "\r\nEOF\r\n", 7);
}

int
main(int argc, char **argv)
{
	const char *requested = NULL;
	struct passwd recipient;
	struct passwd *found;
	char passwd_buffer[2048];
	char selected[UT_LINESIZE + 1U] = "";
	char path[256];
	int lookup_error;
	int terminal;

	if (argc != 2 && argc != 3) {
		fprintf(stderr, "usage: write user [terminal]\n");
		return 2;
	}
	if (argc == 3) {
		requested = argv[2];
		if (strncmp(requested, "/dev/", 5) == 0)
			requested += 5;
	}
	lookup_error = getpwnam_r(argv[1], &recipient, passwd_buffer,
				  sizeof(passwd_buffer), &found);
	if (lookup_error != 0) {
		fprintf(stderr, "write: %s: %s\n", argv[1],
			strerror(lookup_error));
		return 1;
	}
	if (found == NULL) {
		fprintf(stderr, "write: %s: unknown user\n", argv[1]);
		return 1;
	}
	terminal =
	    find_terminal(argv[1], requested, recipient.pw_uid, selected, path);
	if (terminal < 0) {
		fprintf(stderr, "write: %s: no writable terminal: %s\n",
			argv[1], strerror(errno));
		return 1;
	}
	if (send_message(terminal) != 0) {
		int saved_errno = errno;

		(void)close(terminal);
		errno = saved_errno;
		fprintf(stderr, "write: %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (close(terminal) != 0) {
		fprintf(stderr, "write: %s: %s\n", path, strerror(errno));
		return 1;
	}
	return 0;
}
