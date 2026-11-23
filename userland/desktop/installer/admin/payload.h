/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system the helper installs, as it downloads and keeps it: one
 * tarball of the release (release.h), checked whole against the SHA-256
 * GitHub gives for it before anything of it is read, and kept in the
 * helper's own directory.  It is ustar, not compressed, and holds what
 * the installer installs from:
 *
 *   index                the index (index.h)
 *   boot/NAME            the boot files the index names
 *   units/NAME.tar.gz    the units the index names
 *
 * It is never unpacked as a whole: a member is read where it lies in the
 * tarball, so the system takes the room of one copy.  The index is also
 * copied beside it for the installer, which reads it to offer the choice
 * of what to install, and so are the licences the index names (the
 * firmware's), taken out of their units, for the installer to show:
 * the ones of the system downloaded, not of the system running.
 */

#ifndef USERLAND_DESKTOP_INSTALLER_ADMIN_PAYLOAD_H
#define USERLAND_DESKTOP_INSTALLER_ADMIN_PAYLOAD_H

#include "index.h"
#include "release.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* The helper's directory (root's, mode 0700), the tarball, its download and the installer's copy of the index. */
#ifndef PAYLOAD_DIRECTORY
#define PAYLOAD_DIRECTORY	"/run/installer-helper"
#endif
#define PAYLOAD_TARBALL		PAYLOAD_DIRECTORY "/system.tar"
#define PAYLOAD_PARTIAL		PAYLOAD_DIRECTORY "/system.part"
#define PAYLOAD_INDEX		PAYLOAD_DIRECTORY "/index"
#define PAYLOAD_INDEX_NEW	PAYLOAD_DIRECTORY "/index.new"

/*
 * The licences the index names, for the installer: the directory, and in
 * it one file a licence, named by its place among the index's licence
 * records (0, 1, ...).  An index with more is refused.
 */
#define PAYLOAD_LICENSES	PAYLOAD_DIRECTORY "/licenses"
#define PAYLOAD_LICENSES_MAX	32U

/* The most members the tarball may have, and the longest name of one (with its NUL). */
#define PAYLOAD_MEMBERS_MAX	512U
#define PAYLOAD_NAME_MAX	(155U + 1U + 100U + 1U)

/* One member of the tarball: its name and where its bytes lie. */
struct payload_member {
	char name[PAYLOAD_NAME_MAX];
	uint64_t offset;
	uint64_t bytes;
};

/* The members of the tarball, as one reading of its headers found them. */
struct payload {
	struct payload_member member[PAYLOAD_MEMBERS_MAX];
	unsigned count;
	uint64_t size;
};

int payload_scan(int fd, uint64_t size, struct payload *payload);
int payload_find(const struct payload *payload, const char *name, const struct payload_member **member);
int payload_load(struct payload *payload, struct installer_index *index, char *text, size_t capacity);
int payload_open(const struct payload *payload, const char *name, uint64_t expected_bytes, int *fd);
int payload_releases(FILE *output, int input);
int payload_download(FILE *output, int input, const char *tag);

#endif
