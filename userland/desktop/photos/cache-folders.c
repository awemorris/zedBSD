/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Creates only disposable thumbnail-cache directories, independent of the media database. */
#include "photos.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Makes missing parent folders for a thumbnail-cache file.
 */
int
ph_db_folders(
	const char *path)
{
	char folder[PH_PATH_MAX];
	char *slash;
	int length;
	int status;

	/* Each folder from the top. */
	length = snprintf(folder, sizeof(folder), "%s", path);
	if (length < 0 || (size_t)length >= sizeof(folder))
		return ENAMETOOLONG;
	for (slash = strchr(folder + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
		*slash = '\0';
		status = mkdir(folder, 0755);
		if (status != 0 && errno != EEXIST)
			return errno;
		*slash = '/';
	}

	/* The folders are there. */
	return 0;
}
