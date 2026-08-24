/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
write_all(int fd, const void *buffer, size_t size)
{
	const unsigned char *p = buffer;
	while (size) {
		ssize_t n = write(fd, p, size);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += (size_t)n;
		size -= (size_t)n;
	}
	return 0;
}

static int
parse_number(const unsigned char *field, size_t width, unsigned base,
	     uint64_t *value)
{
	size_t i = 0;
	uint64_t result = 0;
	while (i < width && field[i] == ' ')
		i++;
	if (i == width) {
		*value = 0;
		return 0;
	}
	for (; i < width && field[i] != ' '; i++) {
		unsigned digit;
		if (field[i] < '0' || field[i] > '9')
			return -1;
		digit = field[i] - '0';
		if (digit >= base || result > (UINT64_MAX - digit) / base)
			return -1;
		result = result * base + digit;
	}
	while (i < width)
		if (field[i++] != ' ')
			return -1;
	*value = result;
	return 0;
}

static char *
copy_name(const unsigned char *name, size_t length)
{
	char *result;
	while (length && (name[length - 1] == ' ' || name[length - 1] == '/'))
		length--;
	result = malloc(length + 1);
	if (!result)
		return NULL;
	memcpy(result, name, length);
	result[length] = '\0';
	return result;
}

static int
append_member(struct archive_file *archive, struct archive_member *member)
{
	struct archive_member *members;
	if (archive->count == SIZE_MAX / sizeof(*members)) {
		errno = EOVERFLOW;
		return -1;
	}
	members =
	    realloc(archive->members, (archive->count + 1) * sizeof(*members));
	if (!members)
		return -1;
	archive->members = members;
	archive->members[archive->count++] = *member;
	return 0;
}

static char *
gnu_name(const unsigned char *table, size_t table_size, uint64_t offset)
{
	size_t end;
	if (offset >= table_size)
		return NULL;
	end = (size_t)offset;
	while (end < table_size && table[end] != '\n' && table[end] != '\0')
		end++;
	return copy_name(table + (size_t)offset, end - (size_t)offset);
}

int
archive_read_memory(const void *buffer, size_t size,
		    struct archive_file *archive)
{
	const unsigned char *data = buffer;
	const unsigned char *long_names = NULL;
	size_t long_names_size = 0;
	size_t offset = 8;

	memset(archive, 0, sizeof(*archive));
	if (size < 8 || memcmp(data, ARCHIVE_MAGIC, 8)) {
		errno = EFTYPE;
		return -1;
	}
	while (offset < size) {
		struct archive_member member;
		const unsigned char *header;
		const unsigned char *contents;
		uint64_t raw_size, value;
		size_t name_length = 0;
		char field[17];

		memset(&member, 0, sizeof(member));
		if (size - offset < ARCHIVE_HEADER_SIZE) {
			errno = EFTYPE;
			goto fail;
		}
		header = data + offset;
		if (header[58] != '`' || header[59] != '\n' ||
		    parse_number(header + 48, 10, 10, &raw_size) ||
		    raw_size > SIZE_MAX) {
			errno = EFTYPE;
			goto fail;
		}
		offset += ARCHIVE_HEADER_SIZE;
		if ((size_t)raw_size > size - offset) {
			errno = EFTYPE;
			goto fail;
		}
		contents = data + offset;
		memcpy(field, header, 16);
		field[16] = '\0';
		if (!strcmp(field, "/               ")) {
			member.name = strdup("/");
		} else if (!strcmp(field, "//              ")) {
			member.name = strdup("//");
		} else if (!memcmp(field, "#1/", 3)) {
			char *end;
			unsigned long n = strtoul(field + 3, &end, 10);
			while (*end == ' ')
				end++;
			if (*end || n > raw_size) {
				errno = EFTYPE;
				goto fail;
			}
			name_length = (size_t)n;
			member.name = copy_name(contents, name_length);
		} else if (field[0] == '/' && field[1] >= '0' &&
			   field[1] <= '9') {
			char *end;
			unsigned long n = strtoul(field + 1, &end, 10);
			while (*end == ' ')
				end++;
			if (*end)
				member.name = NULL;
			else
				member.name =
				    gnu_name(long_names, long_names_size, n);
		} else {
			member.name = copy_name(header, 16);
		}
		if (!member.name) {
			if (!errno)
				errno = EFTYPE;
			goto fail;
		}
		member.special = field[0] == '/' ||
				 !strcmp(member.name, "__.SYMDEF") ||
				 !strcmp(member.name, "__.SYMDEF SORTED");
		if (parse_number(header + 16, 12, 10, &member.mtime) ||
		    parse_number(header + 28, 6, 10, &value) ||
		    value > UINT_MAX) {
			errno = EFTYPE;
			free(member.name);
			goto fail;
		}
		member.uid = (unsigned)value;
		if (parse_number(header + 34, 6, 10, &value) ||
		    value > UINT_MAX) {
			errno = EFTYPE;
			free(member.name);
			goto fail;
		}
		member.gid = (unsigned)value;
		if (parse_number(header + 40, 8, 8, &value) ||
		    value > UINT_MAX) {
			errno = EFTYPE;
			free(member.name);
			goto fail;
		}
		member.mode = (unsigned)value;
		member.size = (size_t)raw_size - name_length;
		member.data = malloc(member.size ? member.size : 1);
		if (!member.data) {
			free(member.name);
			goto fail;
		}
		memcpy(member.data, contents + name_length, member.size);
		if (!strcmp(field, "//              ")) {
			long_names = member.data;
			long_names_size = member.size;
		}
		if (append_member(archive, &member)) {
			free(member.name);
			free(member.data);
			goto fail;
		}
		offset += (size_t)raw_size;
		if (offset & 1) {
			if (offset == size || data[offset] != '\n') {
				errno = EFTYPE;
				goto fail;
			}
			offset++;
		}
	}
	return 0;
fail:
	archive_free(archive);
	return -1;
}

int
archive_read(const char *path, struct archive_file *archive)
{
	struct stat st;
	unsigned char *data;
	size_t done = 0;
	int fd, result;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || st.st_size < 0 ||
	    (uintmax_t)st.st_size > SIZE_MAX) {
		close(fd);
		if (!errno)
			errno = EOVERFLOW;
		return -1;
	}
	data = malloc(st.st_size ? (size_t)st.st_size : 1);
	if (!data) {
		close(fd);
		return -1;
	}
	while (done < (size_t)st.st_size) {
		ssize_t n = read(fd, data + done, (size_t)st.st_size - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			if (!n)
				errno = EFTYPE;
			free(data);
			close(fd);
			return -1;
		}
		done += (size_t)n;
	}
	close(fd);
	result = archive_read_memory(data, done, archive);
	free(data);
	return result;
}

static int
put_field(char *field, size_t width, uint64_t value, unsigned base)
{
	char number[32];
	int length;
	if (base == 8)
		length = snprintf(number, sizeof(number), "%llo",
				  (unsigned long long)value);
	else
		length = snprintf(number, sizeof(number), "%llu",
				  (unsigned long long)value);
	if (length < 0 || (size_t)length > width)
		return -1;
	memcpy(field, number, (size_t)length);
	memset(field + length, ' ', width - (size_t)length);
	return 0;
}

static int
write_member(int fd, const struct archive_member *member)
{
	char header[ARCHIVE_HEADER_SIZE];
	size_t name_size = 0;
	size_t name_length = strlen(member->name);
	uint64_t stored_size = member->size;
	int extended = name_length > 15 || strchr(member->name, ' ');

	memset(header, ' ', sizeof(header));
	if (member->special && name_length <= 16) {
		memcpy(header, member->name, name_length);
	} else if (extended) {
		int n = snprintf(header, 16, "#1/%zu", name_length);
		if (n < 0 || n > 15 || stored_size > SIZE_MAX - name_length) {
			errno = EOVERFLOW;
			return -1;
		}
		memset(header + n, ' ', 16 - (size_t)n);
		name_size = name_length;
		stored_size += name_length;
	} else {
		memcpy(header, member->name, name_length);
		header[name_length] = '/';
	}
	if (put_field(header + 16, 12, member->mtime, 10) ||
	    put_field(header + 28, 6, member->uid, 10) ||
	    put_field(header + 34, 6, member->gid, 10) ||
	    put_field(header + 40, 8, member->mode, 8) ||
	    put_field(header + 48, 10, stored_size, 10)) {
		errno = EOVERFLOW;
		return -1;
	}
	header[58] = '`';
	header[59] = '\n';
	if (write_all(fd, header, sizeof(header)) ||
	    (name_size && write_all(fd, member->name, name_size)) ||
	    (member->size && write_all(fd, member->data, member->size)) ||
	    ((stored_size & 1) && write_all(fd, "\n", 1)))
		return -1;
	return 0;
}

int
archive_write_atomic(const char *path, const struct archive_file *archive)
{
	char *temporary;
	size_t length = strlen(path);
	int fd = -1;
	int saved;

	if (length > SIZE_MAX - 16) {
		errno = ENAMETOOLONG;
		return -1;
	}
	temporary = malloc(length + 16);
	if (!temporary)
		return -1;
	snprintf(temporary, length + 16, "%s.tmp.XXXXXX", path);
	fd = mkstemp(temporary);
	if (fd < 0)
		goto fail;
	if (fchmod(fd, 0666) || write_all(fd, ARCHIVE_MAGIC, 8))
		goto fail;
	for (size_t i = 0; i < archive->count; i++)
		if (write_member(fd, &archive->members[i]))
			goto fail;
	if (fsync(fd) || close(fd)) {
		fd = -1;
		goto fail;
	}
	fd = -1;
	if (rename(temporary, path))
		goto fail;
	free(temporary);
	return 0;
fail:
	saved = errno;
	if (fd >= 0)
		close(fd);
	unlink(temporary);
	free(temporary);
	errno = saved;
	return -1;
}

void
archive_free(struct archive_file *archive)
{
	for (size_t i = 0; i < archive->count; i++) {
		free(archive->members[i].name);
		free(archive->members[i].data);
	}
	free(archive->members);
	memset(archive, 0, sizeof(*archive));
}

const char *
archive_basename(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}
