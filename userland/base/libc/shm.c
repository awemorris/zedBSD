/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int
shm_path(const char *name, char path[PATH_MAX])
{
	size_t length;
	if (name == NULL || name[0] != '/' || name[1] == '\0' ||
	    strchr(name + 1, '/') != NULL) {
		errno = EINVAL;
		return -1;
	}
	length = strlen(name + 1);
	if (length > PATH_MAX - sizeof("/dev/shm/")) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(path, "/dev/shm/", sizeof("/dev/shm/") - 1U);
	memcpy(path + sizeof("/dev/shm/") - 1U, name + 1, length + 1U);
	return 0;
}

int
shm_open(const char *name, int flags, mode_t mode)
{
	char path[PATH_MAX];
	if (shm_path(name, path) != 0)
		return -1;
	return open(path, flags, mode);
}

int
shm_unlink(const char *name)
{
	char path[PATH_MAX];
	if (shm_path(name, path) != 0)
		return -1;
	return unlink(path);
}
