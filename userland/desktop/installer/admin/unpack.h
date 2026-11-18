/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A unit's tarball, unpacked into the installed root: read whole from one
 * descriptor while its SHA-256 is taken, checked against the index, its
 * gzip wrapper undone, and its ustar entries made one path component at a
 * time under the root, never through a symbolic link.
 */

#ifndef USERLAND_DESKTOP_INSTALLER_ADMIN_UNPACK_H
#define USERLAND_DESKTOP_INSTALLER_ADMIN_UNPACK_H

#include "index.h"

#include <stddef.h>
#include <stdint.h>

/* The kinds of entry a unit may hold. */
enum unpack_kind {
	UNPACK_FILE,
	UNPACK_DIRECTORY,
	UNPACK_SYMLINK
};

/*
 * Told of each entry once it is made: its installed path (from the root),
 * its kind and its size.  A result other than 0 stops the unpacking with
 * that error (ECANCELED for a cancellation between files).
 */
typedef int (*unpack_entry_done)(void *context, const char *path, enum unpack_kind kind, uint64_t bytes);

int unpack_read_whole(int fd, uint64_t expected_bytes, const uint8_t expected[32], uint8_t **data, size_t *length);
int unpack_gunzip(const uint8_t *data, size_t length, uint64_t limit, uint8_t **output, size_t *output_length);
int unpack_tar(int root, const uint8_t *archive, size_t length, const struct installer_index *index, unsigned unit, unpack_entry_done done, void *context);

#endif
