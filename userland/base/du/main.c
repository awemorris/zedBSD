/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Estimates file space usage (POSIX XCU du).
 *
 *	du [-a|-s] [-kx] [-H|-L] [file...]
 *
 * For each file operand (default ".") du writes the space allocated to it:
 * to a directory, the space of the whole hierarchy under it.  Each
 * directory in the hierarchy is written after what is below it; -a writes
 * every other file too, and -s only the operands.  The space is in
 * 512-byte units, or 1024-byte units with -k, rounded up.
 *
 * A file reached more than once, through hard links or through symbolic
 * links that are followed, is counted and written the first time only,
 * for all the operands together.  -x stays on the file system of each
 * operand.  Symbolic links are not followed, except those on the command
 * line with -H and all of them with -L; a directory met again inside
 * itself is reported as a loop.
 *
 * The output is "%d %s\n" with a tab as the blank, as other systems write
 * it.  du exits with 0, or 1 when some file could not be examined.
 */

#include "userland/base/common/command.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* How symbolic links are treated. */
#define DU_FOLLOW_NONE 0
#define DU_FOLLOW_OPERANDS 1
#define DU_FOLLOW_ALL 2

/*
 * A file already counted, by device and inode number.
 *
 * The set is an open-addressed table; an empty slot has used 0.
 */
struct du_file {
	dev_t device;
	ino_t inode;
	int used;
};

/*
 * A directory on the way down to the file being examined, to find loops.
 */
struct du_ancestor {
	dev_t device;
	ino_t inode;
	const struct du_ancestor *parent;
};

/*
 * The state of one run.
 *
 * seen holds the files counted so far; device is the file system of the
 * current operand, for -x.
 */
struct du_run {
	int all;
	int summary;
	int kilobytes;
	int one_file_system;
	int follow;
	struct du_file *seen;
	size_t seen_count;
	size_t seen_capacity;
	dev_t device;
	int failed;
};

static unsigned long long walk(struct du_run *run, const char *path, int operand, const struct du_ancestor *ancestors);
static unsigned long long walk_directory(struct du_run *run, const char *path, const struct stat *status, const struct du_ancestor *ancestors);
static char *join_path(const char *directory, const char *name);
static int remember(struct du_run *run, const struct stat *status);
static int grow_seen(struct du_run *run);
static void report(const struct du_run *run, unsigned long long blocks, const char *path);
static void usage(void);

/*
 * Runs du.
 */
int
main(
	int argc,
	char **argv)
{
	static char *current[] = {".", NULL};
	struct du_run run;
	struct stat status;
	char **operands;
	int option;
	int result;

	/* Reads the options. */
	memset(&run, 0, sizeof(run));
	for (;;) {
		option = getopt(argc, argv, "aHkLsx");
		if (option == -1)
			break;

		/* Records what the option asks for; the last of -H and -L wins. */
		switch (option) {
		case 'a':
			run.all = 1;
			break;
		case 'H':
			run.follow = DU_FOLLOW_OPERANDS;
			break;
		case 'k':
			run.kilobytes = 1;
			break;
		case 'L':
			run.follow = DU_FOLLOW_ALL;
			break;
		case 's':
			run.summary = 1;
			break;
		case 'x':
			run.one_file_system = 1;
			break;
		default:
			usage();
			break;
		}
	}

	/* -a and -s ask for opposite things. */
	if (run.all && run.summary) {
		fprintf(stderr, "du: -a and -s cannot be used together\n");
		usage();
	}

	/* The operands, or the current directory. */
	operands = argv + optind;
	if (optind == argc)
		operands = current;

	/* Walks each operand from its own file system. */
	for (; *operands != NULL; operands++) {
		run.device = 0;
		if (run.one_file_system) {
			result = stat(*operands, &status);
			if (run.follow == DU_FOLLOW_NONE)
				result = lstat(*operands, &status);
			if (result == 0)
				run.device = status.st_dev;
		}

		/* Walks it. */
		walk(&run, *operands, 1, NULL);
	}

	/* A failed write is a failure too. */
	result = fflush(stdout);
	if (result != 0) {
		command_error("du", "standard output");
		run.failed = 1;
	}

	/* Frees the table of files counted. */
	free(run.seen);
	if (run.failed)
		return 1;

	/* Succeeded: every file was examined. */
	return 0;
}

/*
 * Examines a file, and a directory's hierarchy, writing what is asked
 * for; returns the space counted for it in 512-byte units.
 */
static unsigned long long
walk(
	struct du_run *run,
	const char *path,
	int operand,
	const struct du_ancestor *ancestors)
{
	const struct du_ancestor *ancestor;
	struct stat status;
	int result;
	int follow;
	int counted;
	int directory;

	/* Operands follow links with -H or -L, the others with -L only. */
	follow = 0;
	if (run->follow == DU_FOLLOW_ALL)
		follow = 1;
	if (run->follow == DU_FOLLOW_OPERANDS && operand)
		follow = 1;
	if (follow)
		result = stat(path, &status);
	else
		result = lstat(path, &status);
	if (result != 0) {
		command_error("du", path);
		run->failed = 1;
		return 0;
	}

	/* -x leaves out what is on another file system. */
	if (run->one_file_system && !operand && status.st_dev != run->device)
		return 0;

	/* A directory met again inside itself is a loop. */
	directory = S_ISDIR(status.st_mode);
	if (directory) {
		for (ancestor = ancestors; ancestor != NULL; ancestor = ancestor->parent) {
			if (ancestor->device == status.st_dev && ancestor->inode == status.st_ino) {
				fprintf(stderr, "du: %s: directory loop\n", path);
				run->failed = 1;
				return 0;
			}
		}
	}

	/* A file reached before is not counted again. */
	counted = remember(run, &status);
	if (counted < 0) {
		fprintf(stderr, "du: out of memory\n");
		exit(1);
	}

	/* Counted already. */
	if (counted)
		return 0;

	/* A file other than a directory: written with -a, or as an operand. */
	if (!directory) {
		if (operand || run->all)
			report(run, (unsigned long long)status.st_blocks, path);
		return (unsigned long long)status.st_blocks;
	}

	/* A directory: its hierarchy. */
	return walk_directory(run, path, &status, ancestors);
}

/*
 * Adds up a directory and everything under it and writes the total,
 * unless -s leaves out directories below the operands.
 */
static unsigned long long
walk_directory(
	struct du_run *run,
	const char *path,
	const struct stat *status,
	const struct du_ancestor *ancestors)
{
	struct du_ancestor self;
	struct dirent *entry;
	unsigned long long total;
	char *child;
	DIR *directory;
	int compare;
	int operand;

	/* The directory's own space, and it on the way down. */
	total = (unsigned long long)status->st_blocks;
	self.device = status->st_dev;
	self.inode = status->st_ino;
	self.parent = ancestors;
	operand = 0;
	if (ancestors == NULL)
		operand = 1;

	/* Its entries; a directory that cannot be read counts itself only. */
	directory = opendir(path);
	if (directory == NULL) {
		command_error("du", path);
		run->failed = 1;
	} else {
		for (;;) {
			errno = 0;
			entry = readdir(directory);
			if (entry == NULL)
				break;

			/* Skips the directory itself and its parent. */
			compare = strcmp(entry->d_name, ".");
			if (compare == 0)
				continue;
			compare = strcmp(entry->d_name, "..");
			if (compare == 0)
				continue;

			/* Adds up the entry. */
			child = join_path(path, entry->d_name);
			if (child == NULL) {
				fprintf(stderr, "du: out of memory\n");
				exit(1);
			}

			/* Adds the entry's space; its path is done with after. */
			total += walk(run, child, 0, &self);
			free(child);
		}

		/* A failed read ends the directory early. */
		if (errno != 0) {
			command_error("du", path);
			run->failed = 1;
		}

		/* Closes the directory. */
		closedir(directory);
	}

	/* Writes the total, for an operand always. */
	if (!run->summary || operand)
		report(run, total, path);
	return total;
}

/* Makes the path of an entry in a directory; the caller frees it. */
static char *
join_path(
	const char *directory,
	const char *name)
{
	size_t directory_length;
	size_t name_length;
	char *path;

	/* The directory, a slash unless it ends in one, and the name. */
	directory_length = strlen(directory);
	name_length = strlen(name);
	path = malloc(directory_length + name_length + 2);
	if (path == NULL)
		return NULL;
	memcpy(path, directory, directory_length);
	if (directory_length == 0 || directory[directory_length - 1] != '/') {
		path[directory_length] = '/';
		directory_length++;
	}

	/* Then the name. */
	memcpy(path + directory_length, name, name_length + 1);
	return path;
}

/*
 * Remembers a file that can be reached more than once: a directory, or a
 * file with more than one link.  Returns 1 when it was counted before, 0
 * when it is new, and -1 without memory.
 */
static int
remember(
	struct du_run *run,
	const struct stat *status)
{
	size_t slot;
	int result;
	int directory;

	/* A file with a single link is reached once only. */
	directory = S_ISDIR(status->st_mode);
	if (!directory && status->st_nlink <= 1)
		return 0;

	/* Keeps the table at most half full. */
	if ((run->seen_count + 1) * 2 > run->seen_capacity) {
		result = grow_seen(run);
		if (result != 0)
			return -1;
	}

	/* Looks for the file from its slot on. */
	slot = ((size_t)status->st_ino * 31U + (size_t)status->st_dev) % run->seen_capacity;
	while (run->seen[slot].used) {
		if (run->seen[slot].device == status->st_dev && run->seen[slot].inode == status->st_ino)
			return 1;
		slot = (slot + 1) % run->seen_capacity;
	}

	/* New: remembers it. */
	run->seen[slot].device = status->st_dev;
	run->seen[slot].inode = status->st_ino;
	run->seen[slot].used = 1;
	run->seen_count++;
	return 0;
}

/* Doubles the table of files counted, moving what it holds. */
static int
grow_seen(
	struct du_run *run)
{
	struct du_file *old;
	struct du_file *grown;
	size_t old_capacity;
	size_t capacity;
	size_t index;
	size_t slot;

	/* A table twice as large. */
	capacity = run->seen_capacity * 2;
	if (capacity < 64)
		capacity = 64;
	grown = calloc(capacity, sizeof(*grown));
	if (grown == NULL)
		return -1;

	/* Moves each file to its slot in the new table. */
	old = run->seen;
	old_capacity = run->seen_capacity;
	for (index = 0; index < old_capacity; index++) {
		if (!old[index].used)
			continue;
		slot = ((size_t)old[index].inode * 31U + (size_t)old[index].device) % capacity;
		while (grown[slot].used)
			slot = (slot + 1) % capacity;
		grown[slot] = old[index];
	}

	/* Uses the new table. */
	free(old);
	run->seen = grown;
	run->seen_capacity = capacity;
	return 0;
}

/* Writes the space of a file in the units asked for, rounded up. */
static void
report(
	const struct du_run *run,
	unsigned long long blocks,
	const char *path)
{
	unsigned long long units;

	/* 1024-byte units hold two 512-byte ones. */
	units = blocks;
	if (run->kilobytes)
		units = blocks / 2 + blocks % 2;
	printf("%llu\t%s\n", units, path);
}

/* Writes the usage message and exits. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: du [-a|-s] [-kx] [-H|-L] [file...]\n");
	exit(1);
}
