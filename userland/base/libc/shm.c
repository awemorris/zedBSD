/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library shm support.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int shm_path(const char *name, char path[PATH_MAX]);

/*
 * Implements the shm open operation.
 */
int
shm_open(
	const char *name,
	int flags,
	mode_t mode)
{
	int function_result;
	char path[PATH_MAX];

	/* Handles a failed shm path operation. */
	if (shm_path(name, path) != 0)
		return -1;

	/* Obtains the open result. */
	function_result = open(path, flags, mode);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the shm unlink operation.
 */
int
shm_unlink(
	const char *name)
{
	int function_result;
	char path[PATH_MAX];

	/* Handles a failed shm path operation. */
	if (shm_path(name, path) != 0)
		return -1;

	/* Obtains the unlink result. */
	function_result = unlink(path);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the shm path operation. */
static int
shm_path(
	const char *name,
	char path[PATH_MAX])
{
	size_t length;

	/* Handles a failed strchr operation. */
	if (name == NULL || name[0] != '/' || name[1] == '\0' ||
	    strchr(name + 1, '/') != NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	length = strlen(name + 1);

	/* Checks the current data length. */
	if (length > PATH_MAX - sizeof("/dev/shm/")) {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}
	memcpy(path, "/dev/shm/", sizeof("/dev/shm/") - 1U);
	memcpy(path + sizeof("/dev/shm/") - 1U, name + 1, length + 1U);

	/* Reports successful completion. */
	return 0;
}
