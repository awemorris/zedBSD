/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland archive support.
 */

#include "userland/base/common/archive.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ARCHIVE_MAGIC "!<arch>\n"
#define ARCHIVE_HEADER_SIZE 60
#ifndef EFTYPE
#define EFTYPE EINVAL
#endif

static int parse_number(const unsigned char *field, size_t width, unsigned base, uint64_t *value);
static char *copy_name(const unsigned char *name, size_t length);
static char *gnu_name(const unsigned char *table, size_t table_size, uint64_t offset);
static int append_member(struct archive_file *archive, struct archive_member *member);
static int write_all(int fd, const void *buffer, size_t size);
static int write_member(int fd, const struct archive_member *member);
static int put_field(char *field, size_t width, uint64_t value, unsigned base);

/*
 * Implements the archive read memory operation.
 */
int
archive_read_memory(
	const void *buffer,
	size_t size,
	struct archive_file *archive)
{
	char *end_local;
	unsigned long n_local;
	char *end_local1;
	unsigned long n_local2;
	struct archive_member member;
	const unsigned char *header;
	const unsigned char *contents;
	uint64_t raw_size, value;
	size_t name_length;
	char field[17];
	const unsigned char *data;
	const unsigned char *long_names;
	size_t long_names_size;
	size_t offset;

	data = buffer;
	long_names = NULL;
	long_names_size = 0;
	offset = 8;

	memset(archive, 0, sizeof(*archive));

	/* Checks the current data size. */
	if (size < 8 || memcmp(data, ARCHIVE_MAGIC, 8)) {
		errno = EFTYPE;

		/* Reports operation failure. */
		return -1;
	}
	while (offset < size) {

		name_length = 0;

		memset(&member, 0, sizeof(member));

		/* Checks the current data size. */
		if (size - offset < ARCHIVE_HEADER_SIZE) {
			errno = EFTYPE;
			goto fail;
		}
		header = data + offset;

		/* Handles a failed parse number operation. */
		if (header[58] != '`' || header[59] != '\n' ||
		    parse_number(header + 48, 10, 10, &raw_size) ||
		    raw_size > SIZE_MAX) {
			errno = EFTYPE;
			goto fail;
		}
		offset += ARCHIVE_HEADER_SIZE;

		/* Handles the raw size condition. */
		if ((size_t)raw_size > size - offset) {
			errno = EFTYPE;
			goto fail;
		}
		contents = data + offset;
		memcpy(field, header, 16);
		field[16] = '\0';

		/* Selects the matching value. */
		if (!strcmp(field, "/               ")) {
			member.name = strdup("/");
		} else if (!strcmp(field, "//              ")) {
			member.name = strdup("//");
		} else if (!memcmp(field, "#1/", 3)) {
			/* Continue while the operation condition remains true. */
						n_local = strtoul(field + 3, &end_local, 10);
			while (*end_local == ' ')
				end_local++;

			/* Handles the end local condition. */
			if (*end_local || n_local > raw_size) {
				errno = EFTYPE;
				goto fail;
			}
			name_length = (size_t)n_local;
			member.name = copy_name(contents, name_length);
		} else if (field[0] == '/' && field[1] >= '0' &&
			   field[1] <= '9') {
			/* Continue while the operation condition remains true. */
						n_local2 = strtoul(field + 1, &end_local1, 10);
			while (*end_local1 == ' ')
				end_local1++;

			/* Handles the end local1 condition. */
			if (*end_local1)
				member.name = NULL;
			else
				member.name =
				    gnu_name(long_names, long_names_size, n_local2);
		} else {
			member.name = copy_name(header, 16);
		}

		/* Handles the member condition. */
		if (!member.name) {
			/* Handles the reported system error. */
			if (!errno)
				errno = EFTYPE;
			goto fail;
		}
		member.special = field[0] == '/' ||
				 !strcmp(member.name, "__.SYMDEF") ||
				 !strcmp(member.name, "__.SYMDEF SORTED");

		/* Handles a failed parse number operation. */
		if (parse_number(header + 16, 12, 10, &member.mtime) ||
		    parse_number(header + 28, 6, 10, &value) ||
		    value > UINT_MAX) {
			errno = EFTYPE;
			free(member.name);
			goto fail;
		}
		member.uid = (unsigned)value;

		/* Handles a failed parse number operation. */
		if (parse_number(header + 34, 6, 10, &value) ||
		    value > UINT_MAX) {
			errno = EFTYPE;
			free(member.name);
			goto fail;
		}
		member.gid = (unsigned)value;

		/* Handles a failed parse number operation. */
		if (parse_number(header + 40, 8, 8, &value) ||
		    value > UINT_MAX) {
			errno = EFTYPE;
			free(member.name);
			goto fail;
		}
		member.mode = (unsigned)value;
		member.size = (size_t)raw_size - name_length;
		member.data = malloc(member.size ? member.size : 1);

		/* Handles the member condition. */
		if (!member.data) {
			free(member.name);
			goto fail;
		}
		memcpy(member.data, contents + name_length, member.size);

		/* Selects the matching value. */
		if (!strcmp(field, "//              ")) {
			long_names = member.data;
			long_names_size = member.size;
		}

		/* Handles the append member condition. */
		if (append_member(archive, &member)) {
			free(member.name);
			free(member.data);
			goto fail;
		}
		offset += (size_t)raw_size;

		/* Checks the current offset. */
		if (offset & 1) {
			/* Checks the current offset. */
			if (offset == size || data[offset] != '\n') {
				errno = EFTYPE;
				goto fail;
			}
			offset++;
		}
	}

	/* Reports successful completion. */
	return 0;
fail:
	archive_free(archive);

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the archive read operation.
 */
int
archive_read(
	const char *path,
	struct archive_file *archive)
{
	ssize_t n;
	struct stat st;
	unsigned char *data;
	size_t done;
	int fd, result;

	done = 0;

	fd = open(path, O_RDONLY);

	/* Checks the file descriptor. */
	if (fd < 0)
		return -1;

	/* Handles a failed fstat operation. */
	if (fstat(fd, &st) || st.st_size < 0 ||
	    (uintmax_t)st.st_size > SIZE_MAX) {
		close(fd);

		/* Handles the reported system error. */
		if (!errno)
			errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	data = malloc(st.st_size ? (size_t)st.st_size : 1);

	/* Handles the data condition. */
	if (!data) {
		close(fd);

		/* Reports operation failure. */
		return -1;
	}
	while (done < (size_t)st.st_size) {

		n = read(fd, data + done, (size_t)st.st_size - done);

		/* Handles the reported system error. */
		if (n < 0 && errno == EINTR)
			continue;

		/* Checks the current item count. */
		if (n <= 0) {
			/* Checks the current item count. */
			if (!n)
				errno = EFTYPE;
			free(data);
			close(fd);

			/* Reports operation failure. */
			return -1;
		}
		done += (size_t)n;
	}
	close(fd);
	result = archive_read_memory(data, done, archive);
	free(data);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the archive write atomic operation.
 */
int
archive_write_atomic(
	const char *path,
	const struct archive_file *archive)
{
	size_t i_index_for;
	char *temporary;
	size_t length;
	int fd;
	int saved;

	length = strlen(path);
	fd = -1;

	/* Checks the current data length. */
	if (length > SIZE_MAX - 16) {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}
	temporary = malloc(length + 16);

	/* Handles the temporary condition. */
	if (!temporary)
		return -1;
	snprintf(temporary, length + 16, "%s.tmp.XXXXXX", path);
	fd = mkstemp(temporary);

	/* Checks the file descriptor. */
	if (fd < 0)
		goto fail;

	/* Handles the fchmod condition. */
	if (fchmod(fd, 0666) || write_all(fd, ARCHIVE_MAGIC, 8))
		goto fail;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < archive->count; i_index_for++)

		/* Handles a failed write member operation. */
		if (write_member(fd, &archive->members[i_index_for]))
			goto fail;

	/* Handles the fsync condition. */
	if (fsync(fd) || close(fd)) {
		fd = -1;
		goto fail;
	}
	fd = -1;

	/* Handles the rename condition. */
	if (rename(temporary, path))
		goto fail;
	free(temporary);

	/* Reports successful completion. */
	return 0;
fail:
	saved = errno;

	/* Checks the file descriptor. */
	if (fd >= 0)
		close(fd);
	unlink(temporary);
	free(temporary);
	errno = saved;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the archive free operation.
 */
void
archive_free(
	struct archive_file *archive)
{
	size_t i_index_for;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < archive->count; i_index_for++) {
		free(archive->members[i_index_for].name);
		free(archive->members[i_index_for].data);
	}
	free(archive->members);
	memset(archive, 0, sizeof(*archive));
}

/*
 * Implements the archive basename operation.
 */
const char *
archive_basename(
	const char *path)
{
	const char *slash;

	slash = strrchr(path, '/');

	/* Returns the computed result. */
	return slash ? slash + 1 : path;
}

/* Supports the parse number operation. */
static int
parse_number(
	const unsigned char *field,
	size_t width,
	unsigned base,
	uint64_t *value)
{
	unsigned digit;
	size_t i;
	uint64_t result;

	/* Continue while the operation condition remains true. */
	i = 0;
	result = 0;
	while (i < width && field[i] == ' ')
		i++;

	/* Checks the current index. */
	if (i == width) {
		*value = 0;
		/* Reports successful completion. */
		return 0;
	}

	/* Process each element required by the operation. */
	for (; i < width && field[i] != ' '; i++) {
		/* Handles the field condition. */
		if (field[i] < '0' || field[i] > '9')
			return -1;
		digit = field[i] - '0';

		/* Handles the digit condition. */
		if (digit >= base || result > (UINT64_MAX - digit) / base)
			return -1;
		result = result * base + digit;
	}
	while (i < width)

		/* Handles the field condition. */
		if (field[i++] != ' ')
			return -1;
	*value = result;
	/* Reports successful completion. */
	return 0;
}

/* Supports the copy name operation. */
static char *
copy_name(
	const unsigned char *name,
	size_t length)
{
	char *result;

	/* Process each remaining element. */
	while (length && (name[length - 1] == ' ' || name[length - 1] == '/'))
		length--;
	result = malloc(length + 1);

	/* Checks the operation result. */
	if (!result)
		return NULL;
	memcpy(result, name, length);
	result[length] = '\0';

	/* Returns the computed result. */
	return result;
}

/* Supports the gnu name operation. */
static char *
gnu_name(
	const unsigned char *table,
	size_t table_size,
	uint64_t offset)
{
	char *function_result;
	size_t end;

	/* Checks the current offset. */
	if (offset >= table_size)
		return NULL;

	/* Process each remaining element. */
	end = (size_t)offset;
	while (end < table_size && table[end] != '\n' && table[end] != '\0')
		end++;

	/* Obtains the copy name result. */
	function_result = copy_name(table + (size_t)offset, end - (size_t)offset);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the append member operation. */
static int
append_member(
	struct archive_file *archive,
	struct archive_member *member)
{
	struct archive_member *members;

	/* Handles the archive condition. */
	if (archive->count == SIZE_MAX / sizeof(*members)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	members =
	    realloc(archive->members, (archive->count + 1) * sizeof(*members));

	/* Handles the members condition. */
	if (!members)
		return -1;
	archive->members = members;
	archive->members[archive->count++] = *member;

	/* Reports successful completion. */
	return 0;
}

/* Supports the write all operation. */
static int
write_all(
	int fd,
	const void *buffer,
	size_t size)
{
	ssize_t n;
	const unsigned char *p;

	/* Process each remaining element. */
	p = buffer;
	while (size) {

		n = write(fd, p, size);

		/* Checks the current item count. */
		if (n < 0) {
			/* Handles the reported system error. */
			if (errno == EINTR)
				continue;

			/* Reports operation failure. */
			return -1;
		}
		p += (size_t)n;
		size -= (size_t)n;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the write member operation. */
static int
write_member(
	int fd,
	const struct archive_member *member)
{
	char header[ARCHIVE_HEADER_SIZE];
	size_t name_size;
	size_t name_length;
	uint64_t stored_size;
	int extended;
	int extended_name_length;

	name_size = 0;
	name_length = strlen(member->name);
	stored_size = member->size;
	extended = name_length > 15 || strchr(member->name, ' ');

	memset(header, ' ', sizeof(header));

	/* Handles the member condition. */
	if (member->special && name_length <= 16) {
		memcpy(header, member->name, name_length);
	} else if (extended) {
		extended_name_length = snprintf(header, 16, "#1/%zu", name_length);

		/* Handles the extended name length condition. */
		if (extended_name_length < 0 || extended_name_length > 15 ||
		    stored_size > SIZE_MAX - name_length) {
			errno = EOVERFLOW;

			/* Reports operation failure. */
			return -1;
		}
		memset(header + extended_name_length, ' ',
		       16 - (size_t)extended_name_length);
		name_size = name_length;
		stored_size += name_length;
	} else {
		memcpy(header, member->name, name_length);
		header[name_length] = '/';
	}

	/* Handles a failed put field operation. */
	if (put_field(header + 16, 12, member->mtime, 10) ||
	    put_field(header + 28, 6, member->uid, 10) ||
	    put_field(header + 34, 6, member->gid, 10) ||
	    put_field(header + 40, 8, member->mode, 8) ||
	    put_field(header + 48, 10, stored_size, 10)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	header[58] = '`';
	header[59] = '\n';

	/* Handles a failed write all operation. */
	if (write_all(fd, header, sizeof(header)) ||
	    (name_size && write_all(fd, member->name, name_size)) ||
	    (member->size && write_all(fd, member->data, member->size)) ||
	    ((stored_size & 1) && write_all(fd, "\n", 1)))

		/* Reports operation failure. */
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the put field operation. */
static int
put_field(
	char *field,
	size_t width,
	uint64_t value,
	unsigned base)
{
	char number[32];
	int length;

	/* Handles the base condition. */
	if (base == 8)
		length = snprintf(number, sizeof(number), "%llo",
				  (unsigned long long)value);
	else
		length = snprintf(number, sizeof(number), "%llu",
				  (unsigned long long)value);

	/* Checks the current data length. */
	if (length < 0 || (size_t)length > width)
		return -1;
	memcpy(field, number, (size_t)length);
	memset(field + length, ' ', width - (size_t)length);

	/* Reports successful completion. */
	return 0;
}
