/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * laytool: lays a root tree's bin, sbin, lib and usr over the running
 * system's root, in one statically linked process (the system's own cp,
 * mv and shell would be replaced under the copy: a new program with the old
 * libraries does not start).  Each file is written beside its place and
 * renamed over it (a running program keeps its old file), each link is made
 * beside and renamed over, each directory made when missing.  A set-user or
 * set-group program of the system is kept (its owner and mode are the
 * image's).  A file is executable (0755) when its source has an execute
 * bit, else 0644.  Then each laid file is read back and compared.
 *
 *   laytool SOURCE-ROOT [TARGET-ROOT]      (TARGET-ROOT, default "", for a host test)
 * Prints "laytool: laid N files, M links, K kept, D directories" and
 * "laytool: checked N files" or the first failures, and exits nonzero on one.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned laid;
static unsigned linked;
static unsigned kept;
static unsigned made;
static unsigned checked;
static unsigned failures;

static void
failed(
	const char *what,
	const char *path)
{
	if (failures < 20U)
		printf("laytool: FAIL %s %s errno=%d\n", what, path, errno);
	failures++;
}

static int
copy_file(
	const char *source,
	const char *target,
	mode_t mode)
{
	char buffer[65536];
	ssize_t got;
	ssize_t put;
	ssize_t done;
	int in;
	int out;

	in = open(source, O_RDONLY);
	if (in < 0)
		return -1;
	out = open(target, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (out < 0) {
		(void)close(in);
		return -1;
	}
	for (;;) {
		got = read(in, buffer, sizeof(buffer));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		for (done = 0; done < got; done += put) {
			put = write(out, buffer + done, (size_t)(got - done));
			if (put < 0 && errno == EINTR) {
				put = 0;
				continue;
			}
			if (put <= 0) {
				(void)close(in);
				(void)close(out);
				return -1;
			}
		}
	}
	(void)close(in);
	if (got < 0 || fchmod(out, mode) != 0 || fsync(out) != 0) {
		(void)close(out);
		return -1;
	}
	return close(out);
}

static int
same_file(
	const char *left,
	const char *right)
{
	char a[65536];
	char b[65536];
	ssize_t got_a;
	ssize_t got_b;
	int fa;
	int fb;
	int same;

	fa = open(left, O_RDONLY);
	fb = open(right, O_RDONLY);
	same = fa >= 0 && fb >= 0;
	while (same) {
		got_a = read(fa, a, sizeof(a));
		got_b = read(fb, b, sizeof(b));
		if (got_a != got_b || got_a < 0) {
			same = 0;
			break;
		}
		if (got_a == 0)
			break;
		if (memcmp(a, b, (size_t)got_a) != 0)
			same = 0;
	}
	if (fa >= 0)
		(void)close(fa);
	if (fb >= 0)
		(void)close(fb);
	return same;
}

static void
lay_entry(
	const char *source,
	const char *target)
{
	char beside[PATH_MAX + 16];
	char link[PATH_MAX];
	struct stat from;
	struct stat now;
	mode_t mode;
	ssize_t length;

	if (lstat(source, &from) != 0) {
		failed("lstat", source);
		return;
	}
	(void)snprintf(beside, sizeof(beside), "%s.ws352-new", target);

	if (S_ISLNK(from.st_mode)) {
		length = readlink(source, link, sizeof(link) - 1U);
		if (length < 0) {
			failed("readlink", source);
			return;
		}
		link[length] = '\0';
		(void)unlink(beside);
		if (symlink(link, beside) != 0 || rename(beside, target) != 0) {
			failed("link", target);
			return;
		}
		linked++;
		return;
	}
	if (!S_ISREG(from.st_mode))
		return;

	/* A set-user or set-group program of the system stays. */
	if (lstat(target, &now) == 0 && (now.st_mode & (S_ISUID | S_ISGID)) != 0) {
		printf("laytool: kept %s\n", target);
		kept++;
		return;
	}
	mode = (from.st_mode & 0111) != 0 ? 0755 : 0644;
	if (copy_file(source, beside, mode) != 0) {
		failed("copy", target);
		return;
	}
	if (rename(beside, target) != 0) {
		failed("rename", target);
		return;
	}
	laid++;
}

static void
walk(
	const char *source,
	const char *target,
	int check)
{
	char from[PATH_MAX];
	char to[PATH_MAX];
	struct dirent *entry;
	struct stat status;
	DIR *directory;

	directory = opendir(source);
	if (directory == NULL) {
		failed("opendir", source);
		return;
	}
	if (!check && mkdir(target, 0755) == 0)
		made++;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		(void)snprintf(from, sizeof(from), "%s/%s", source, entry->d_name);
		(void)snprintf(to, sizeof(to), "%s/%s", target, entry->d_name);
		if (lstat(from, &status) != 0) {
			failed("lstat", from);
			continue;
		}
		if (S_ISDIR(status.st_mode)) {
			walk(from, to, check);
		} else if (!check) {
			lay_entry(from, to);
		} else if (S_ISREG(status.st_mode)) {
			if (lstat(to, &status) == 0 && (status.st_mode & (S_ISUID | S_ISGID)) != 0)
				continue;
			if (!same_file(from, to))
				failed("differs", to);
			else
				checked++;
		}
	}
	(void)closedir(directory);
}

int
main(
	int count,
	char **arguments)
{
	static const char *const tops[] = { "bin", "sbin", "lib", "usr" };
	char source[PATH_MAX];
	char target[PATH_MAX];
	const char *root;
	unsigned index;

	if (count != 2 && count != 3) {
		printf("usage: laytool SOURCE-ROOT [TARGET-ROOT]\n");
		return 2;
	}
	root = count == 3 ? arguments[2] : "";
	for (index = 0; index < 4U; index++) {
		(void)snprintf(source, sizeof(source), "%s/%s", arguments[1], tops[index]);
		(void)snprintf(target, sizeof(target), "%s/%s", root, tops[index]);
		walk(source, target, 0);
	}
	sync();
	printf("laytool: laid %u files, %u links, %u kept, %u directories\n", laid, linked, kept, made);
	for (index = 0; index < 4U; index++) {
		(void)snprintf(source, sizeof(source), "%s/%s", arguments[1], tops[index]);
		(void)snprintf(target, sizeof(target), "%s/%s", root, tops[index]);
		walk(source, target, 1);
	}
	printf("laytool: checked %u files\n", checked);
	if (failures != 0) {
		printf("laytool: FAIL %u\n", failures);
		return 1;
	}
	return 0;
}
