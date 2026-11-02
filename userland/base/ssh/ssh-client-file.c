/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The small services of the ssh program: the home directory and "~/"
 * paths, reading a whole file, messages to the user, questions on the
 * terminal, and the clock.
 */

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "ssh-client-common.h"

/* The terminal of the process, where questions are asked. */
#define CLIENT_TERMINAL		"/dev/tty"

static int client_copy(char *out, size_t capacity, const char *first, const char *second);
static int client_read_line(int descriptor, char *out, size_t capacity);

/*
 * Writes the home directory into out.
 */
int
ssh_client_home(
	char *out,
	size_t capacity)
{
	const char *home;
	struct passwd *account;
	int error;

	/* Takes the home directory from the environment first. */
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0') {
		/* Falls back to the account database. */
		account = getpwuid(getuid());
		if (account == NULL)
			return ENOENT;
		home = account->pw_dir;
		if (home == NULL || home[0] == '\0')
			return ENOENT;
	}

	/* Copies it out. */
	error = client_copy(out, capacity, home, "");
	if (error != 0)
		return error;

	/* Succeeded: out holds the home directory. */
	return 0;
}

/*
 * Expands a leading "~/" to the home directory.
 */
int
ssh_client_expand_path(
	const char *path,
	char *out,
	size_t capacity)
{
	char home[SSH_CLIENT_PATH_MAX];
	int error;

	/* Copies a path that does not start with the home directory as it is. */
	if (path[0] != '~' || (path[1] != '/' && path[1] != '\0')) {
		error = client_copy(out, capacity, path, "");
		if (error != 0)
			return error;
		return 0;
	}

	/* Finds the home directory. */
	error = ssh_client_home(home, sizeof(home));
	if (error != 0)
		return error;

	/* Puts the home directory in place of the '~'. */
	error = client_copy(out, capacity, home, path + 1);
	if (error != 0)
		return error;

	/* Succeeded: out holds the expanded path. */
	return 0;
}

/*
 * Reads the whole file at path into out.
 */
int
ssh_client_read_file(
	const char *path,
	struct ssh_buf *out,
	size_t limit)
{
	uint8_t chunk[4096];
	ssize_t count;
	int descriptor;
	int error;

	/* Opens the file for reading, without making it the controlling terminal. */
	descriptor = open(path, O_RDONLY | O_NOCTTY);
	if (descriptor < 0)
		return errno;

	/* Reads until the end of the file or until a failure. */
	error = 0;
	for (;;) {
		count = read(descriptor, chunk, sizeof(chunk));

		/* Retries a read that a signal cut short, and stops on any other failure. */
		if (count < 0) {
			if (errno == EINTR)
				continue;
			error = errno;
			break;
		}

		/* Stops at the end of the file. */
		if (count == 0)
			break;

		/* Refuses a file longer than the limit. */
		if ((size_t)count > limit - out->size) {
			error = EMSGSIZE;
			break;
		}

		/* Appends what was read. */
		ssh_buf_put_bytes(out, chunk, (size_t)count);
		error = ssh_buf_error(out);
		if (error != 0)
			break;
	}

	/* Closes the file and wipes the copy on the stack (the file may hold a key). */
	close(descriptor);
	ssh_wipe(chunk, sizeof(chunk));

	/* Reports how the read ended. */
	if (error != 0)
		return error;

	/* Succeeded: out holds the whole file. */
	return 0;
}

/*
 * Prints a message to the standard error stream.
 */
void
ssh_client_log(
	const struct ssh_client *client,
	int level,
	const char *format,
	...)
{
	va_list arguments;
	int log_level;
	int raw;

	/* Takes the level and the terminal mode of the connection, or the defaults without one. */
	log_level = SSH_CLIENT_LOG_INFO;
	raw = 0;
	if (client != NULL) {
		log_level = client->log_level;
		raw = client->raw;
	}

	/* Leaves out a message more detailed than the level asks for. */
	if (level > log_level)
		return;

	/* Prints the message. */
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);

	/* Ends the line, with a carriage return while the terminal does not add one. */
	if (raw) {
		fputs("\r\n", stderr);
	} else {
		fputs("\n", stderr);
	}
	fflush(stderr);
}

/*
 * Asks the user on the controlling terminal.
 */
int
ssh_client_ask(
	const char *prompt,
	int echo,
	char *out,
	size_t capacity)
{
	struct termios saved;
	struct termios quiet;
	ssize_t written;
	int descriptor;
	int status;
	int error;

	/* Opens the terminal of the process; without one nobody can be asked. */
	descriptor = open(CLIENT_TERMINAL, O_RDWR | O_NOCTTY);
	if (descriptor < 0)
		return ENXIO;

	/* Shows the question. */
	written = write(descriptor, prompt, strlen(prompt));
	if (written < 0) {
		error = errno;
		close(descriptor);
		return error;
	}

	/* Turns the echo off for a secret answer, keeping the settings to put back. */
	if (!echo) {
		status = tcgetattr(descriptor, &saved);
		if (status != 0) {
			error = errno;
			close(descriptor);
			return error;
		}
		quiet = saved;
		quiet.c_lflag &= ~(tcflag_t)ECHO;
		status = tcsetattr(descriptor, TCSAFLUSH, &quiet);
		if (status != 0) {
			error = errno;
			close(descriptor);
			return error;
		}
	}

	/* Reads the answer. */
	error = client_read_line(descriptor, out, capacity);

	/* Puts the echo back and ends the line the user could not see. */
	if (!echo) {
		(void)tcsetattr(descriptor, TCSAFLUSH, &saved);
		written = write(descriptor, "\n", 1);
		(void)written;
	}

	/* Closes the terminal. */
	close(descriptor);

	/* Reports an answer that could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: out holds the answer. */
	return 0;
}

/*
 * Returns the time of a monotonic clock in milliseconds.
 */
uint64_t
ssh_client_now_ms(void)
{
	struct timespec now;
	uint64_t milliseconds;
	int status;

	/* Reads the clock; a clock that cannot be read counts as time 0. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0;

	/* Converts seconds and nanoseconds to milliseconds. */
	milliseconds = (uint64_t)now.tv_sec * 1000U;
	milliseconds += (uint64_t)now.tv_nsec / 1000000U;

	/* Reports the time. */
	return milliseconds;
}

/*
 * Joins two strings into out, refusing a result that does not fit.
 */
static int
client_copy(
	char *out,
	size_t capacity,
	const char *first,
	const char *second)
{
	size_t first_size;
	size_t second_size;

	/* Measures both parts. */
	first_size = strlen(first);
	second_size = strlen(second);

	/* Refuses a result that would not fit with its NUL. */
	if (first_size >= capacity)
		return ENAMETOOLONG;
	if (second_size >= capacity - first_size)
		return ENAMETOOLONG;

	/* Copies the two parts one after the other. */
	memcpy(out, first, first_size);
	memcpy(out + first_size, second, second_size);
	out[first_size + second_size] = '\0';

	/* Succeeded: out holds the joined string. */
	return 0;
}

/*
 * Reads one line from the terminal into out, without its LF; a line that
 * does not fit is read to its end and refused.
 */
static int
client_read_line(
	int descriptor,
	char *out,
	size_t capacity)
{
	char byte;
	ssize_t count;
	size_t length;
	int too_long;

	/* Reads byte by byte up to the end of the line. */
	length = 0;
	too_long = 0;
	for (;;) {
		count = read(descriptor, &byte, 1);

		/* Retries a read that a signal cut short; the end of input ends the line. */
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;

		/* Stops at the end of the line. */
		if (byte == '\n')
			break;

		/* Keeps the byte while there is room; a carriage return is not part of the answer. */
		if (byte == '\r')
			continue;
		if (length + 1 >= capacity) {
			too_long = 1;
			continue;
		}
		out[length] = byte;
		length++;
	}

	/* Ends the answer, and wipes a line that did not fit. */
	out[length] = '\0';
	byte = 0;
	if (too_long) {
		ssh_wipe(out, capacity);
		return EMSGSIZE;
	}

	/* Succeeded: out holds the line. */
	return 0;
}
