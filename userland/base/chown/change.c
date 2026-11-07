/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Changes the owner and group of files for chown and chgrp.
 *
 * Without -R a file operand that is a symbolic link changes the file it
 * refers to, or with -h the link itself.  With -R a directory and
 * everything below it change; a symbolic link that is not followed is
 * changed itself, and the -H, -L and -P policy says which links are
 * followed: those named as operands, all of them, or none (the default).
 */

#include "userland/base/chown/change.h"
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The deepest directory nesting -R follows. */
#define CHANGE_DEPTH_MAX 128

static int read_option_letters(const char *cluster, struct owner_change *change);
static int change_entry(const struct owner_change *change, const char *path, const struct stat *status_of_file, unsigned depth);
static int change_children(const struct owner_change *change, const char *path, unsigned depth);
static int parse_number(const char *text, unsigned long *value);
static int report(const struct owner_change *change, const char *path);

/*
 * Reads the options of chown and chgrp and returns the index of the first
 * operand, or -1 after an unknown option.
 */
int
owner_read_options(
	int argc,
	char **argv,
	struct owner_change *change)
{
	int index;
	int status;
	int compare;

	/* Nothing asked for yet; -R alone follows no link. */
	change->recursive = 0;
	change->follow = CHANGE_FOLLOW_NONE;
	change->no_dereference = 0;

	/* Reads options until the first operand. */
	for (index = 1; index < argc; index++) {
		if (argv[index][0] != '-' || argv[index][1] == '\0')
			break;

		/* -- ends the options. */
		compare = strcmp(argv[index], "--");
		if (compare == 0) {
			index++;
			break;
		}

		/* Reads the letters of the argument. */
		status = read_option_letters(argv[index] + 1, change);
		if (status != 0)
			return -1;
	}

	/* Reports where the operands start. */
	return index;
}

/*
 * Changes one file operand, and with -R everything below it.  Returns 0,
 * or -1 after a diagnosed failure.
 */
int
owner_change_operand(
	const struct owner_change *change,
	const char *path)
{
	struct stat status_of_file;
	int status;

	/* Without -R one call changes the file, or the link with -h. */
	if (!change->recursive) {
		if (change->no_dereference)
			status = lchown(path, change->uid, change->gid);
		else
			status = chown(path, change->uid, change->gid);
		if (status != 0) {
			report(change, path);
			return -1;
		}

		/* Succeeded: the file changed. */
		return 0;
	}

	/* With -R, -H and -L follow a link named as an operand. */
	if (change->follow == CHANGE_FOLLOW_NONE)
		status = lstat(path, &status_of_file);
	else
		status = stat(path, &status_of_file);
	if (status != 0) {
		report(change, path);
		return -1;
	}

	/* Changes the file and what is below it. */
	status = change_entry(change, path, &status_of_file, 0);
	if (status != 0)
		return -1;

	/* Succeeded: everything changed. */
	return 0;
}

/*
 * Parses the user of an owner operand: a user name, or a numeric user ID
 * when no user has that name.  Stores the user's login group, or
 * (gid_t)-1 for a numeric ID.
 */
int
owner_parse_user(
	const char *text,
	uid_t *uid,
	gid_t *login_group)
{
	struct passwd *entry;
	unsigned long value;
	int status;

	/* A user name comes first. */
	entry = getpwnam(text);
	if (entry != NULL) {
		*uid = entry->pw_uid;
		*login_group = entry->pw_gid;
		return 0;
	}

	/* Otherwise a decimal user ID. */
	status = parse_number(text, &value);
	if (status != 0)
		return -1;

	/* Succeeded: the numeric ID, with no login group known. */
	*uid = (uid_t)value;
	*login_group = (gid_t)-1;
	return 0;
}

/*
 * Parses a group operand: a group name, or a numeric group ID when no
 * group has that name.
 */
int
owner_parse_group(
	const char *text,
	gid_t *gid)
{
	struct group *entry;
	unsigned long value;
	int status;

	/* A group name comes first. */
	entry = getgrnam(text);
	if (entry != NULL) {
		*gid = entry->gr_gid;
		return 0;
	}

	/* Otherwise a decimal group ID. */
	status = parse_number(text, &value);
	if (status != 0)
		return -1;

	/* Succeeded: the numeric ID. */
	*gid = (gid_t)value;
	return 0;
}

/* Reads a cluster of option letters; returns -1 for an unknown one. */
static int
read_option_letters(
	const char *cluster,
	struct owner_change *change)
{
	const char *letter;

	/* Takes each letter in turn; of -H, -L and -P the last wins. */
	for (letter = cluster; *letter != '\0'; letter++) {
		/* Records what the letter asks for. */
		switch (*letter) {
		case 'h':
			change->no_dereference = 1;
			break;
		case 'R':
			change->recursive = 1;
			break;
		case 'H':
			change->follow = CHANGE_FOLLOW_OPERANDS;
			break;
		case 'L':
			change->follow = CHANGE_FOLLOW_ALL;
			break;
		case 'P':
			change->follow = CHANGE_FOLLOW_NONE;
			break;
		default:
			fprintf(stderr, "%s: unknown option -%c\n", change->program, *letter);
			return -1;
		}
	}

	/* Succeeded: every letter was known. */
	return 0;
}

/*
 * Changes one file reached by -R, and a directory's entries after it.  A
 * symbolic link reaching here was not followed and is changed itself.
 */
static int
change_entry(
	const struct owner_change *change,
	const char *path,
	const struct stat *status_of_file,
	unsigned depth)
{
	int symbolic;
	int directory;
	int failed;
	int status;

	/* Bounds the nesting -R follows. */
	if (depth >= CHANGE_DEPTH_MAX) {
		errno = ELOOP;
		report(change, path);
		return -1;
	}

	/* A link that is not followed is changed itself. */
	symbolic = S_ISLNK(status_of_file->st_mode);
	if (symbolic) {
		status = lchown(path, change->uid, change->gid);
		if (status != 0) {
			report(change, path);
			return -1;
		}

		/* Succeeded: the link changed. */
		return 0;
	}

	/* Changes the file, or what a followed link refers to. */
	failed = 0;
	status = chown(path, change->uid, change->gid);
	if (status != 0) {
		report(change, path);
		failed = 1;
	}

	/* Goes below a directory. */
	directory = S_ISDIR(status_of_file->st_mode);
	if (directory) {
		status = change_children(change, path, depth);
		if (status != 0)
			failed = 1;
	}

	/* Reports a failure. */
	if (failed)
		return -1;

	/* Succeeded: the file and everything below it changed. */
	return 0;
}

/*
 * Changes every entry of a directory.  -L follows the links met here; a
 * link whose target is missing is changed itself.
 */
static int
change_children(
	const struct owner_change *change,
	const char *path,
	unsigned depth)
{
	char child[PATH_MAX + 1];
	struct stat status_of_child;
	struct dirent *entry;
	DIR *stream;
	int count;
	int failed;
	int status;
	int dot;
	int dot_dot;

	/* Opens the directory. */
	stream = opendir(path);
	if (stream == NULL) {
		report(change, path);
		return -1;
	}

	/* Reads each entry, telling a read error from the end. */
	failed = 0;
	for (;;) {
		errno = 0;
		entry = readdir(stream);
		if (entry == NULL) {
			if (errno != 0) {
				report(change, path);
				failed = 1;
			}

			/* The end of the directory ends the loop. */
			break;
		}

		/* Skips the directory itself and its parent. */
		dot = strcmp(entry->d_name, ".");
		dot_dot = strcmp(entry->d_name, "..");
		if (dot == 0 || dot_dot == 0)
			continue;

		/* Names the entry. */
		count = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
		if (count < 0 || (size_t)count >= sizeof(child)) {
			errno = ENAMETOOLONG;
			report(change, path);
			failed = 1;
			continue;
		}

		/* Reads the entry; -L follows a link when its target exists. */
		status = -1;
		if (change->follow == CHANGE_FOLLOW_ALL)
			status = stat(child, &status_of_child);
		if (status != 0)
			status = lstat(child, &status_of_child);
		if (status != 0) {
			report(change, child);
			failed = 1;
			continue;
		}

		/* Changes the entry and what is below it. */
		status = change_entry(change, child, &status_of_child, depth + 1);
		if (status != 0)
			failed = 1;
	}

	/* Closes the directory. */
	closedir(stream);

	/* Reports whether any entry failed. */
	if (failed)
		return -1;

	/* Succeeded: every entry changed. */
	return 0;
}

/* Parses a whole decimal number; returns -1 for anything else. */
static int
parse_number(
	const char *text,
	unsigned long *value)
{
	const char *digit;
	unsigned long number;
	unsigned long limit;

	/* An empty text is no number. */
	if (text[0] == '\0')
		return -1;

	/* Reads each digit, refusing overflow past the largest ID. */
	number = 0;
	limit = (unsigned long)(uid_t)-1;
	for (digit = text; *digit != '\0'; digit++) {
		if (*digit < '0' || *digit > '9')
			return -1;
		number = number * 10 + (unsigned long)(*digit - '0');
		if (number >= limit)
			return -1;
	}

	/* Succeeded: the number. */
	*value = number;
	return 0;
}

/* Writes a diagnostic naming a pathname and the current error. */
static int
report(
	const struct owner_change *change,
	const char *path)
{
	/* Names the program, the pathname and the reason. */
	fprintf(stderr, "%s: %s: %s\n", change->program, path, strerror(errno));
	return -1;
}
