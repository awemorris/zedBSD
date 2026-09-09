/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Compares physical trees independently of copy completion records. */
#include "userland/base/common/command.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Captures original attributes before comparison reads can advance atime. */
struct pair {
	struct pair *next;
	struct stat left, right;
};

/* Owns one comparison's hard-link bijection and output policy. */
struct comparison {
	struct pair *pairs;
	int recursive, metadata, brief;
	int (*text)(const char *, const char *);
};

/* Owns a sorted, complete directory census. */
struct names {
	char **items;
	size_t count;
};

static int error(const char *path);
static int different(const char *left, const char *right, const char *reason);
static int identity(const struct stat *left, const struct stat *right);
static int stable(const struct stat *before, const struct stat *after);
static int metadata_equal(const struct stat *left, const struct stat *right);
static int remember(struct comparison *context, struct stat *left, struct stat *right);
static int order(const void *left, const void *right);
static void names_free(struct names *names);
static int names_read(const char *path, struct names *names);
static char *join(const char *parent, const char *name);
static int compare_node(struct comparison *context, const char *left, const char *right, unsigned depth);
static int compare_directory(struct comparison *context, const char *left, const char *right, unsigned depth);
static int read_chunk(int fd, unsigned char *buffer, size_t *length);
static int compare_file(const char *left, const char *right, const struct stat *a, const struct stat *b, int *binary);

/* Reports an inspection failure, distinct from a proven difference. */
static int
error(const char *path)
{
	command_error("diff", path);
	return 2;
}

/* Reports the reason a pair does not match. */
static int
different(const char *left, const char *right, const char *reason)
{
	printf("%s and %s differ: %s\n", left, right, reason);
	return 1;
}

/* Tests object identity only within the same filesystem. */
static int
identity(const struct stat *left, const struct stat *right)
{
	return left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}

/* Detects replacement or metadata/content changes during a file read. */
static int
stable(const struct stat *before, const struct stat *after)
{
	return identity(before, after) && before->st_mode == after->st_mode &&
	    before->st_size == after->st_size && before->st_uid == after->st_uid &&
	    before->st_gid == after->st_gid &&
	    before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
	    before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
	    before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
	    before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

/* Excludes ctime, inode numbers and filesystem-dependent allocation sizes. */
static int
metadata_equal(const struct stat *left, const struct stat *right)
{
	return left->st_mode == right->st_mode &&
	    left->st_uid == right->st_uid && left->st_gid == right->st_gid &&
	    left->st_atim.tv_sec == right->st_atim.tv_sec &&
	    left->st_atim.tv_nsec == right->st_atim.tv_nsec &&
	    left->st_mtim.tv_sec == right->st_mtim.tv_sec &&
	    left->st_mtim.tv_nsec == right->st_mtim.tv_nsec;
}

/* Detects both split link groups and accidentally merged independent files. */
static int
remember(struct comparison *context, struct stat *left, struct stat *right)
{
	struct pair *entry;
	int same_left, same_right;

	/* Repeated aliases use the attributes observed before our first read. */
	for (entry = context->pairs; entry != NULL; entry = entry->next) {
		same_left = identity(left, &entry->left);
		same_right = identity(right, &entry->right);
		if (same_left != same_right)
			return 1;
		if (same_left) {
			left->st_atim = entry->left.st_atim;
			right->st_atim = entry->right.st_atim;
			return 0;
		}
	}

	/* The registry belongs to this invocation, including single-link objects. */
	entry = malloc(sizeof(*entry));
	if (entry == NULL)
		return 2;
	entry->left = *left;
	entry->right = *right;
	entry->next = context->pairs;
	context->pairs = entry;
	return 0;
}

/* Orders directory entry bytes without locale-dependent transformations. */
static int
order(const void *left, const void *right)
{
	return strcmp(*(char *const *)left, *(char *const *)right);
}

/* Releases all entries acquired by a complete or failed enumeration. */
static void
names_free(struct names *names)
{
	size_t index;

	for (index = 0; index < names->count; index++)
		free(names->items[index]);
	free(names->items);
}

/* Reads the entire entry set and refuses partial readdir or close results. */
static int
names_read(const char *path, struct names *names)
{
	DIR *stream;
	struct dirent *entry;
	char **items;
	char *name;
	int failed;

	memset(names, 0, sizeof(*names));
	stream = opendir(path);
	if (stream == NULL)
		return error(path);
	failed = 0;
	/* A cleared errno distinguishes the final entry from a traversal failure. */
	for (;;) {
		errno = 0;
		entry = readdir(stream);
		if (entry == NULL) {
			if (errno != 0)
				failed = error(path);
			break;
		}
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		name = strdup(entry->d_name);
		if (name == NULL) {
			failed = error(path);
			break;
		}
		items = realloc(names->items, (names->count + 1) * sizeof(*items));
		if (items == NULL) {
			free(name);
			failed = error(path);
			break;
		}
		names->items = items;
		names->items[names->count++] = name;
	}

	if (closedir(stream) != 0)
		failed = error(path);
	if (failed != 0)
		return failed;
	if (names->count > 1)
		qsort(names->items, names->count, sizeof(*names->items), order);
	return 0;
}

/* Builds a bounded path without putting PATH_MAX arrays on each stack frame. */
static char *
join(const char *parent, const char *name)
{
	char *path;
	size_t length;

	length = strlen(parent) + strlen(name) + 2;
	if (length > PATH_MAX + 1U) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	path = malloc(length);
	if (path != NULL)
		snprintf(path, length, "%s/%s", parent, name);
	return path;
}

/* Visits matching names and detects entries present on either side only. */
static int
compare_directory(struct comparison *context, const char *left, const char *right, unsigned depth)
{
	struct names a, b;
	char *path_left, *path_right;
	size_t i, j;
	int status, failed, compared;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	failed = names_read(left, &a);
	status = names_read(right, &b);
	if (status > failed)
		failed = status;
	i = j = 0;
	/* Never regard an incomplete entry set as a successful comparison. */
	while (failed != 2 && (i < a.count || j < b.count)) {
		compared = 0;
		if (i == a.count)
			compared = 1;
		else if (j == b.count)
			compared = -1;
		else
			compared = strcmp(a.items[i], b.items[j]);
		if (compared != 0) {
			if (compared < 0)
				printf("Only in %s: %s\n", left, a.items[i++]);
			else
				printf("Only in %s: %s\n", right, b.items[j++]);
			failed = 1;
			continue;
		}
		path_left = join(left, a.items[i++]);
		path_right = join(right, b.items[j++]);
		if (path_left == NULL || path_right == NULL)
			status = error("pathname");
		else
			status = compare_node(context, path_left, path_right, depth + 1);
		free(path_left);
		free(path_right);
		if (status > failed)
			failed = status;
	}

	names_free(&a);
	names_free(&b);
	return failed;
}

/* Fills one comparison chunk even when a read returns a short prefix. */
static int
read_chunk(int fd, unsigned char *buffer, size_t *length)
{
	ssize_t count;

	*length = 0;
	while (*length < 8192) {
		count = read(fd, buffer + *length, 8192 - *length);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			return 2;
		}
		if (count == 0)
			break;
		*length += (size_t)count;
	}
	return 0;
}

/* Compares bytes through checked handles and rejects changed path identities. */
static int
compare_file(const char *left, const char *right, const struct stat *a, const struct stat *b, int *binary)
{
	unsigned char first[8192], second[8192];
	struct stat opened;
	int fd_left, fd_right, failed, status;
	size_t length_left, length_right;
	off_t total_left, total_right;

	fd_left = open(left, O_RDONLY | O_NOFOLLOW);
	if (fd_left < 0)
		return error(left);
	fd_right = open(right, O_RDONLY | O_NOFOLLOW);
	failed = 0;
	total_left = total_right = 0;
	if (fd_right < 0)
		failed = error(right);
	if (failed == 0 && (fstat(fd_left, &opened) != 0 || !stable(a, &opened)))
		failed = 2;
	if (failed == 0 && (fstat(fd_right, &opened) != 0 || !stable(b, &opened)))
		failed = 2;
	/* Continue after differing bytes to detect late read/close failures. */
	while (failed != 2) {
		status = read_chunk(fd_left, first, &length_left);
		if (status != 0) {
			failed = error(left);
			break;
		}
		status = read_chunk(fd_right, second, &length_right);
		if (status != 0) {
			failed = error(right);
			break;
		}
		if (memchr(first, 0, length_left) != NULL || memchr(second, 0, length_right) != NULL)
			*binary = 1;
		if (length_left != length_right || memcmp(first, second, length_left) != 0)
			failed = 1;
		/* A reported EOF must cover the extent admitted by the initial stat. */
		if (length_left > (uint64_t)a->st_size - (uint64_t)total_left ||
		    length_right > (uint64_t)b->st_size - (uint64_t)total_right) {
			failed = 2;
			break;
		}
		total_left += (off_t)length_left;
		total_right += (off_t)length_right;
		if (length_left == 0 && length_right == 0)
			break;
	}

	if (failed != 2 && (total_left != a->st_size || total_right != b->st_size))
		failed = 2;
	if (fstat(fd_left, &opened) != 0 || !stable(a, &opened))
		failed = 2;
	if (fd_right >= 0 && (fstat(fd_right, &opened) != 0 || !stable(b, &opened)))
		failed = 2;
	if (failed == 2)
		fprintf(stderr, "diff: could not verify stable file contents: %s, %s\n", left, right);
	if (close(fd_left) != 0)
		failed = error(left);
	if (fd_right >= 0 && close(fd_right) != 0)
		failed = error(right);
	return failed;
}

/* Dispatches by lstat type; links and special nodes are never opened as files. */
static int
compare_node(struct comparison *context, const char *left, const char *right, unsigned depth)
{
	struct stat a, b;
	char *target_left, *target_right;
	ssize_t length_left, length_right;
	int failed, status, binary;

	if (depth >= 128) {
		errno = ELOOP;
		return error(left);
	}
	if (lstat(left, &a) != 0)
		return error(left);
	if (lstat(right, &b) != 0)
		return error(right);
	if ((a.st_mode & S_IFMT) != (b.st_mode & S_IFMT))
		return different(left, right, "type");
	failed = 0;
	if (context->metadata && !S_ISDIR(a.st_mode)) {
		status = remember(context, &a, &b);
		if (status == 2)
			return error("hard-link registry");
		if (status == 1)
			failed = different(left, right, "hard-link relationship");
	}
	if (context->metadata && !metadata_equal(&a, &b))
		failed = different(left, right, "attributes");
	status = 0;
	if (S_ISDIR(a.st_mode)) {
		if (depth == 0 || context->recursive)
			status = compare_directory(context, left, right, depth);
		else
			printf("Common subdirectories: %s and %s\n", left, right);
	} else if (S_ISREG(a.st_mode)) {
		binary = 0;
		status = compare_file(left, right, &a, &b, &binary);
		if (status == 1) {
			if (binary || context->brief || context->metadata)
				status = different(left, right, "contents");
			else {
				status = context->text(left, right);
				/* Two reads that disagree about equality cannot prove a match. */
				if (status == 0)
					status = 2;
			}
		}
	} else if (S_ISLNK(a.st_mode)) {
		/* Allocate link buffers only for links, not every recursive directory frame. */
		target_left = malloc(PATH_MAX + 1U);
		target_right = malloc(PATH_MAX + 1U);
		if (target_left == NULL || target_right == NULL) {
			free(target_left);
			free(target_right);
			return error("symbolic link buffer");
		}
		length_left = readlink(left, target_left, PATH_MAX + 1U);
		length_right = readlink(right, target_right, PATH_MAX + 1U);
		if (length_left < 0 || length_right < 0)
			status = error("symbolic link");
		else if (length_left == PATH_MAX + 1U || length_right == PATH_MAX + 1U) {
			errno = ENAMETOOLONG;
			status = error("symbolic link");
		} else if (length_left != length_right || memcmp(target_left, target_right, (size_t)length_left) != 0)
			status = different(left, right, "symbolic link target");
		free(target_left);
		free(target_right);
	} else if (S_ISCHR(a.st_mode) || S_ISBLK(a.st_mode)) {
		if (a.st_rdev != b.st_rdev)
			status = different(left, right, "device identity");
	} else if (!S_ISFIFO(a.st_mode)) {
		errno = EOPNOTSUPP;
		status = error(left);
	}
	if (status > failed)
		failed = status;
	return failed;
}

/* Compares two operands and releases all invocation-owned inode mappings. */
int
diff_tree(const char *left, const char *right, int recursive, int metadata, int brief, int (*text)(const char *, const char *))
{
	struct comparison context;
	struct pair *next;
	int status;

	memset(&context, 0, sizeof(context));
	context.recursive = recursive;
	context.metadata = metadata;
	context.brief = brief;
	context.text = text;
	status = compare_node(&context, left, right, 0);
	while (context.pairs != NULL) {
		next = context.pairs->next;
		free(context.pairs);
		context.pairs = next;
	}
	return status;
}
