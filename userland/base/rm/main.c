/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Removes directory entries (POSIX XCU rm).
 *
 *	rm [-fiRr] file...
 *
 * -r and -R remove a directory and everything under it, depth first; a
 * symbolic link is removed, never followed.  -f ignores missing files and
 * never asks; -i asks before each removal; the last of the two given wins.
 * Without -f, a file that cannot be written is asked about when standard
 * input is a terminal.  An operand whose last component is . or .., and
 * the root directory, are refused.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The options. */
struct options {
	int force;
	int interactive;
	int recursive;
	int terminal;
};

static int read_options(int argc, char **argv, struct options *options);
static int remove_operand(const struct options *options, const char *path);
static int remove_path(const struct options *options, const char *path);
static int remove_tree(const struct options *options, const char *path);
static int refused_name(const char *path);
static int ask(const char *question, const char *path);
static char *join_path(const char *directory, const char *name);
static void report(const char *path);
static void usage(void);

/*
 * Runs rm.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	int first;
	int index;
	int failed;
	int result;

	/* The options; no operand is an error unless -f is given. */
	memset(&options, 0, sizeof(options));
	first = read_options(argc, argv, &options);
	if (first >= argc && !options.force)
		usage();
	options.terminal = isatty(STDIN_FILENO);

	/* Each operand in turn. */
	failed = 0;
	for (index = first; index < argc; index++) {
		result = remove_operand(&options, argv[index]);
		if (result != 0)
			failed = 1;
	}

	/* A missing operand with -f, or none at all, is not a failure. */
	if (failed)
		return 1;

	/* Succeeded. */
	return 0;
}

/* Reads the options; returns the index of the first operand. */
static int
read_options(
	int argc,
	char **argv,
	struct options *options)
{
	const char *word;
	const char *letter;
	int index;

	/* Each word that starts with - and is not - alone; -- ends them. */
	for (index = 1; index < argc; index++) {
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;
		if (word[1] == '-' && word[2] == '\0')
			return index + 1;

		/* Each letter; -f and -i override each other. */
		for (letter = word + 1; *letter != '\0'; letter++) {
			switch (*letter) {
			case 'f':
				options->force = 1;
				options->interactive = 0;
				break;
			case 'i':
				options->interactive = 1;
				options->force = 0;
				break;
			case 'r':
			case 'R':
				options->recursive = 1;
				break;
			default:
				usage();
				break;
			}
		}
	}

	/* Succeeded: the first operand. */
	return index;
}

/* Removes one operand, after refusing the names rm must not touch. */
static int
remove_operand(
	const struct options *options,
	const char *path)
{
	int refused;
	int result;

	/* . and .. as the last component, and the root directory. */
	refused = refused_name(path);
	if (refused) {
		fprintf(stderr, "rm: refusing to remove '.' or '..' or '/': %s\n", path);
		return -1;
	}

	/* Succeeded: whatever removing it reports. */
	result = remove_path(options, path);
	return result;
}

/* Removes a path: a directory with -r, and anything else directly. */
static int
remove_path(
	const struct options *options,
	const char *path)
{
	struct stat status;
	int error;
	int writable;
	int directory;
	int symbolic;
	int yes;

	/* What the path is, without following a link; missing may be fine. */
	error = lstat(path, &status);
	if (error != 0) {
		if (options->force && errno == ENOENT)
			return 0;
		report(path);
		return -1;
	}

	/* A directory needs -r. */
	directory = S_ISDIR(status.st_mode);
	symbolic = S_ISLNK(status.st_mode);
	if (directory) {
		if (!options->recursive) {
			fprintf(stderr, "rm: %s: is a directory\n", path);
			return -1;
		}

		/* The directory and everything under it. */
		error = remove_tree(options, path);
		return error;
	}

	/* -i asks; without -f, so does a file that cannot be written. */
	if (options->interactive) {
		yes = ask("remove", path);
		if (!yes)
			return 0;
	} else if (!options->force && options->terminal && !symbolic) {
		writable = access(path, W_OK);
		if (writable != 0) {
			yes = ask("remove write-protected file", path);
			if (!yes)
				return 0;
		}
	}

	/* The entry. */
	error = unlink(path);
	if (error != 0) {
		report(path);
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Removes a directory and everything under it, depth first. */
static int
remove_tree(
	const struct options *options,
	const char *path)
{
	struct dirent *entry;
	DIR *directory;
	char *child;
	int failed;
	int error;
	int yes;
	int dot;
	int dot_dot;

	/* -i asks before going into it. */
	if (options->interactive) {
		yes = ask("descend into directory", path);
		if (!yes)
			return 0;
	}

	/* Its entries. */
	directory = opendir(path);
	if (directory == NULL) {
		report(path);
		return -1;
	}

	/* Each entry but . and .., removed before the directory. */
	failed = 0;
	for (;;) {
		errno = 0;
		entry = readdir(directory);
		if (entry == NULL)
			break;
		dot = strcmp(entry->d_name, ".");
		dot_dot = strcmp(entry->d_name, "..");
		if (dot == 0 || dot_dot == 0)
			continue;
		child = join_path(path, entry->d_name);
		error = remove_path(options, child);
		free(child);
		if (error != 0)
			failed = 1;
	}

	/* The listing is done with. */
	closedir(directory);
	if (failed)
		return -1;

	/* -i asks again for the directory itself. */
	if (options->interactive) {
		yes = ask("remove directory", path);
		if (!yes)
			return 0;
	}

	/* The directory, now empty. */
	error = rmdir(path);
	if (error != 0) {
		report(path);
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Returns whether a path is the root, or ends in . or .. as a component. */
static int
refused_name(
	const char *path)
{
	const char *last;
	size_t length;
	size_t index;
	int slashes;
	int dot;
	int dot_dot;

	/* The root: slashes only. */
	length = strlen(path);
	slashes = 1;
	for (index = 0; index < length; index++) {
		if (path[index] != '/') {
			slashes = 0;
			break;
		}
	}

	/* Slashes alone are the root. */
	if (slashes && length > 0)
		return 1;

	/* The last component, without the slashes after it. */
	while (length > 1 && path[length - 1U] == '/')
		length--;
	last = path;
	for (index = 0; index < length; index++) {
		if (path[index] == '/')
			last = path + index + 1;
	}

	/* . or .. */
	length -= (size_t)(last - path);
	dot = 0;
	dot_dot = 0;
	if (length == 1 && last[0] == '.')
		dot = 1;
	if (length == 2 && last[0] == '.' && last[1] == '.')
		dot_dot = 1;
	if (dot || dot_dot)
		return 1;

	/* Succeeded: any other name. */
	return 0;
}

/* Asks a question on standard error; returns whether the answer is yes. */
static int
ask(
	const char *question,
	const char *path)
{
	int first;
	int character;

	/* The question. */
	fprintf(stderr, "rm: %s '%s'? ", question, path);
	fflush(stderr);

	/* The first character of the answer, and the rest of the line. */
	first = getchar();
	character = first;
	while (character != '\n' && character != EOF)
		character = getchar();

	/* Succeeded: y or Y is yes. */
	if (first == 'y' || first == 'Y')
		return 1;
	return 0;
}

/* Joins a directory and a name with a slash. */
static char *
join_path(
	const char *directory,
	const char *name)
{
	char *path;
	size_t directory_length;
	size_t name_length;

	/* Room for both, a slash and the NUL. */
	directory_length = strlen(directory);
	name_length = strlen(name);
	path = malloc(directory_length + name_length + 2U);
	if (path == NULL) {
		fprintf(stderr, "rm: out of memory\n");
		exit(1);
	}

	/* The directory, a slash unless it ends with one, and the name. */
	memcpy(path, directory, directory_length);
	if (directory_length > 0 && directory[directory_length - 1U] != '/') {
		path[directory_length] = '/';
		directory_length++;
	}

	/* The name after it. */
	memcpy(path + directory_length, name, name_length + 1U);

	/* Succeeded. */
	return path;
}

/* Reports the error of a path. */
static void
report(
	const char *path)
{
	/* The path and why. */
	fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
}

/* Reports the usage and ends rm. */
static void
usage(
	void)
{
	/* The form. */
	fprintf(stderr, "usage: rm [-fiRr] file...\n");
	exit(1);
}
