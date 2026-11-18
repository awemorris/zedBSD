/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Owns one volatile networkd confirmed-commit rollback program. */

#include "userland/base/networkd/confirmed.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NETWORKD_ROLLBACK_PREFIX "/tmp/net.rollback."

static int load_program(int, networkd_rollback_callback, void *, int,
	char *, size_t);
static void release_program(struct networkd_confirmed *);
static void append_diagnostic(char *, size_t, size_t *, unsigned,
	const char *);

void
networkd_confirmed_init(
	struct networkd_confirmed *state)
{
	if (state == NULL)
		return;
	memset(state, 0, sizeof(*state));
	state->descriptor = -1;
	state->next_token = 1U;
}

void
networkd_confirmed_reset(
	struct networkd_confirmed *state)
{
	uint32_t next_token;

	if (state == NULL)
		return;
	next_token = state->next_token != 0U ? state->next_token : 1U;
	release_program(state);
	memset(state, 0, sizeof(*state));
	state->descriptor = -1;
	state->next_token = next_token;
}

int
networkd_confirmed_arm(
	struct networkd_confirmed *state,
	const char *path,
	uid_t owner,
	unsigned minutes,
	uint64_t now,
	networkd_rollback_callback validate,
	void *context,
	uint32_t *token,
	char *diagnostic,
	size_t capacity)
{
	struct stat before;
	struct stat after;
	size_t length;
	int descriptor;
	int saved;

	if (diagnostic != NULL && capacity != 0U)
		diagnostic[0] = '\0';
	if (state == NULL || path == NULL || validate == NULL || token == NULL ||
	    state->active || minutes == 0U ||
	    minutes > NETWORKD_CONFIRMED_MINUTES_MAX) {
		errno = state != NULL && state->active ? EBUSY : EINVAL;
		return -1;
	}
	length = strlen(path);
	if (length <= sizeof(NETWORKD_ROLLBACK_PREFIX) - 1U ||
	    length > NETWORKD_ROLLBACK_PATH_MAX ||
	    strncmp(path, NETWORKD_ROLLBACK_PREFIX,
	    sizeof(NETWORKD_ROLLBACK_PREFIX) - 1U) != 0 ||
	    strchr(path + sizeof(NETWORKD_ROLLBACK_PREFIX) - 1U, '/') != NULL) {
		errno = EINVAL;
		return -1;
	}
	if (lstat(path, &before) != 0)
		return -1;
	descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	if (fstat(descriptor, &after) != 0 || !S_ISREG(after.st_mode) ||
	    after.st_nlink != 1 || after.st_uid != owner ||
	    (after.st_mode & 07777U) != 0600U || after.st_size <= 0 ||
	    (uint64_t)after.st_size > NETWORKD_ROLLBACK_PROGRAM_MAX ||
	    before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
		saved = errno != 0 ? errno : EINVAL;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}
	if (load_program(descriptor, validate, context, 0, diagnostic,
	    capacity) != 0) {
		saved = errno != 0 ? errno : EINVAL;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}
	state->descriptor = descriptor;
	strcpy(state->path, path);
	state->device = after.st_dev;
	state->inode = after.st_ino;
	state->deadline = now + (uint64_t)minutes * 60ULL * 1000000ULL;
	state->token = state->next_token++;
	if (state->token == 0U)
		state->token = state->next_token++;
	if (state->next_token == 0U)
		state->next_token = 1U;
	state->active = 1;
	*token = state->token;
	return 0;
}

int
networkd_confirmed_disarm(
	struct networkd_confirmed *state,
	uint32_t token)
{
	if (state == NULL || !state->active) {
		errno = ENOENT;
		return -1;
	}
	if (token == 0U || token != state->token) {
		errno = ESTALE;
		return -1;
	}
	networkd_confirmed_reset(state);
	return 0;
}

int
networkd_confirmed_check(
	const struct networkd_confirmed *state,
	uint32_t token)
{
	if (token == 0U) {
		if (state != NULL && state->active) {
			errno = EBUSY;
			return -1;
		}
		return 0;
	}
	if (state == NULL || !state->active) {
		errno = ENOENT;
		return -1;
	}
	if (state->token != token) {
		errno = ESTALE;
		return -1;
	}
	return 0;
}

int
networkd_confirmed_rollback(
	struct networkd_confirmed *state,
	networkd_rollback_callback execute,
	void *context,
	char *diagnostic,
	size_t capacity)
{
	int result;
	int saved;

	if (diagnostic != NULL && capacity != 0U)
		diagnostic[0] = '\0';
	if (state == NULL || !state->active) {
		errno = ENOENT;
		return -1;
	}
	result = load_program(state->descriptor, execute, context, 1,
	    diagnostic, capacity);
	saved = result == 0 ? 0 : (errno != 0 ? errno : EIO);
	networkd_confirmed_reset(state);
	if (saved != 0) {
		errno = saved;
		return -1;
	}
	return 0;
}

int
networkd_confirmed_run_due(
	struct networkd_confirmed *state,
	uint64_t now,
	networkd_rollback_callback execute,
	void *context,
	char *diagnostic,
	size_t capacity)
{
	if (state == NULL || !state->active || now < state->deadline)
		return 0;
	return networkd_confirmed_rollback(state, execute, context, diagnostic,
	    capacity) == 0 ? 1 : -1;
}

int
networkd_confirmed_poll_timeout(
	const struct networkd_confirmed *state,
	uint64_t now)
{
	uint64_t milliseconds;

	if (state == NULL || !state->active)
		return -1;
	if (now >= state->deadline)
		return 0;
	milliseconds = (state->deadline - now + 999ULL) / 1000ULL;
	if (milliseconds > (uint64_t)INT_MAX)
		return INT_MAX;
	return (int)milliseconds;
}

int
networkd_confirmed_active(
	const struct networkd_confirmed *state)
{
	return state != NULL && state->active;
}

static int
load_program(
	int descriptor,
	networkd_rollback_callback callback,
	void *context,
	int execute,
	char *diagnostic,
	size_t capacity)
{
	char bytes[NETWORKD_ROLLBACK_PROGRAM_MAX + 1U];
	char step[NETWORKD_ROLLBACK_DIAGNOSTIC_MAX + 1U];
	char *line;
	char *newline;
	ssize_t count;
	size_t used;
	size_t output_used;
	unsigned operations;
	int failed;

	if (diagnostic != NULL && capacity != 0U)
		diagnostic[0] = '\0';
	if (descriptor < 0 || callback == NULL ||
	    lseek(descriptor, 0, SEEK_SET) < 0)
		return -1;
	used = 0U;
	while (used < NETWORKD_ROLLBACK_PROGRAM_MAX) {
		do {
			count = read(descriptor, bytes + used,
			    NETWORKD_ROLLBACK_PROGRAM_MAX - used);
		} while (count < 0 && errno == EINTR);
		if (count < 0)
			return -1;
		if (count == 0)
			break;
		used += (size_t)count;
	}
	if (used == 0U || (used == NETWORKD_ROLLBACK_PROGRAM_MAX &&
	    read(descriptor, step, 1U) != 0) || bytes[used - 1U] != '\n' ||
	    memchr(bytes, '\0', used) != NULL) {
		errno = EINVAL;
		return -1;
	}
	bytes[used] = '\0';
	operations = 0U;
	failed = 0;
	output_used = 0U;
	line = bytes;
	while (*line != '\0') {
		newline = strchr(line, '\n');
		if (newline == NULL || newline == line ||
		    (size_t)(newline - line) > NETWORKD_ROLLBACK_LINE_MAX ||
		    operations == NETWORKD_ROLLBACK_OPERATION_MAX) {
			errno = EINVAL;
			return -1;
		}
		*newline = '\0';
		memset(step, 0, sizeof(step));
		operations++;
		if (callback(line, step, sizeof(step), context) != 0) {
			if (!execute) {
				errno = EINVAL;
				return -1;
			}
			failed = 1;
			append_diagnostic(diagnostic, capacity, &output_used,
			    operations, step[0] != '\0' ? step : "operation failed");
		}
		line = newline + 1;
	}
	if (operations == 0U) {
		errno = EINVAL;
		return -1;
	}
	(void)lseek(descriptor, 0, SEEK_SET);
	if (failed) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static void
release_program(
	struct networkd_confirmed *state)
{
	struct stat status;

	if (state == NULL)
		return;
	if (state->descriptor >= 0)
		(void)close(state->descriptor);
	state->descriptor = -1;
	if (state->path[0] != '\0' && lstat(state->path, &status) == 0 &&
	    status.st_dev == state->device && status.st_ino == state->inode)
		(void)unlink(state->path);
}

static void
append_diagnostic(
	char *output,
	size_t capacity,
	size_t *used,
	unsigned operation,
	const char *message)
{
	int count;
	size_t available;

	if (output == NULL || used == NULL || *used >= capacity || capacity == 0U)
		return;
	available = capacity - *used;
	count = snprintf(output + *used, available, "rollback line %u: %.*s\n",
	    operation, (int)NETWORKD_ROLLBACK_DIAGNOSTIC_MAX, message);
	if (count < 0)
		return;
	if ((size_t)count >= available)
		*used = capacity - 1U;
	else
		*used += (size_t)count;
}
