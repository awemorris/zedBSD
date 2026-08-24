/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_ARCHIVE_H
#define ZEDBSD_USERLAND_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>

struct archive_member {
	char *name;
	uint64_t mtime;
	unsigned uid;
	unsigned gid;
	unsigned mode;
	unsigned char *data;
	size_t size;
	int special;
};

struct archive_file {
	struct archive_member *members;
	size_t count;
};

int archive_read(const char *path, struct archive_file *archive);
int archive_read_memory(const void *data, size_t size,
			struct archive_file *archive);
int archive_write_atomic(const char *path, const struct archive_file *archive);
void archive_free(struct archive_file *archive);
const char *archive_basename(const char *path);

#endif
