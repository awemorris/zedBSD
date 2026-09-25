/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The cd and pwd builtins (POSIX XCU cd, pwd), and the shell's logical
 * current directory.
 *
 * The logical directory is the path cd changed to, with the symbolic links
 * it went through; pwd prints it and PWD holds it, both exported.  cd -L
 * (the default) resolves . and .. by their names in that path, as dash
 * does; cd -P and pwd -P use the system's physical path.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/vars.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The shell's logical current directory: set at startup and by cd; NULL
 * only when the current directory could not be named at all.
 */
static char *logical_cwd;

static int cd_options(int argc, char **argv, int *index, int *physical, int *check);
static int cd_search_path(const char *target, int physical, int print);
static int is_dot_relative(const char *path);
static int try_chdir(const char *path, int physical, int print);
static char *logical_path(const char *base, const char *path);
static char option_letter(const char *word);

/*
 * Implements pwd: -L (the default) prints the shell's logical directory;
 * -P prints the physical path.  Operands are ignored, as dash does.
 */
int
sh_builtin_pwd(
	int argc,
	char **argv)
{
	char path[PATH_MAX];
	char *found;
	int physical;
	int index;
	char letter;

	/* -L and -P; the last one counts, and -- ends them. */
	physical = 0;
	for (index = 1; index < argc && argv[index][0] == '-'; index++) {
		letter = option_letter(argv[index]);
		if (letter == '-')
			break;
		switch (letter) {
		case 'P':
			physical = 1;
			break;
		case 'L':
			physical = 0;
			break;
		default:
			fprintf(stderr, "pwd: Illegal option %s\n",
				argv[index]);
			return 2;
		}
	}

	/* The directory the shell keeps. */
	if (!physical && logical_cwd != NULL) {
		printf("%s\n", logical_cwd);
		return 0;
	}

	/* The system's answer. */
	found = getcwd(path, sizeof(path));
	if (found == NULL) {
		fprintf(stderr, "pwd: %s\n", strerror(errno));
		return 1;
	}

	/* The directory the system reports. */
	printf("%s\n", path);

	/* Succeeded. */
	return 0;
}

/*
 * Implements cd: cd [-L|-P [-e]] [directory], cd -.  CDPATH is searched for
 * a relative directory; PWD and OLDPWD follow, exported.  With -P and -e,
 * a directory that cannot be named afterwards makes the status 1 (POSIX
 * 2024).  Operands after the first are ignored, as dash does.
 */
int
sh_builtin_cd(
	int argc,
	char **argv)
{
	char current[PATH_MAX];
	const char *target;
	char *found;
	int physical;
	int check;
	int index;
	int print;
	int valid;
	int changed;

	/* -L, -P and -e. */
	valid = cd_options(argc, argv, &index, &physical, &check);
	if (!valid)
		return 2;

	/* The directory: the operand, HOME, or OLDPWD for -. */
	print = 0;
	if (index >= argc) {
		/* No operand: HOME. */
		target = sh_var_get("HOME");
		if (target == NULL || target[0] == '\0') {
			fprintf(stderr, "cd: HOME not set\n");
			return 2;
		}
	} else if (argv[index][0] == '-' && argv[index][1] == '\0') {
		/* -: OLDPWD, printed; without one, where the shell is. */
		target = sh_var_get("OLDPWD");
		if (target == NULL || target[0] == '\0')
			target = logical_cwd;
		if (target == NULL)
			target = ".";
		print = 1;
	} else {
		/* The operand. */
		target = argv[index];
	}

	/* A relative name is looked for on CDPATH first. */
	changed = cd_search_path(target, physical, print);
	if (!changed) {
		/* Then the directory itself. */
		changed = try_chdir(target, physical, print);
		if (changed != 0) {
			fprintf(stderr, "cd: can't cd to %s\n", target);
			return 2;
		}
	}

	/* -P -e: the new directory must have a name. */
	if (physical && check) {
		found = getcwd(current, sizeof(current));
		if (found == NULL)
			return 1;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Sets the shell's logical directory at startup: PWD when it names the
 * current directory, the physical path otherwise.  PWD is exported.
 */
void
sh_cwd_init(
	void)
{
	char path[PATH_MAX];
	const char *pwd;
	char *found;
	int valid;

	/* A valid inherited PWD is kept. */
	pwd = sh_var_get("PWD");
	valid = sh_pwd_valid(pwd);
	if (valid) {
		logical_cwd = sh_strdup(pwd);
	} else {
		/* Otherwise the physical path, if it can be found. */
		found = getcwd(path, sizeof(path));
		if (found == NULL)
			return;
		logical_cwd = sh_strdup(path);
	}

	/* PWD is exported. */
	(void)sh_var_set("PWD", logical_cwd, SH_VAR_EXPORT);
}

/*
 * Reports whether a path is a valid PWD: absolute, without . or ..
 * components, and naming the current directory.
 */
int
sh_pwd_valid(
	const char *path)
{
	struct stat named;
	struct stat current;
	const char *cursor;
	int error;

	/* Absolute. */
	if (path == NULL || path[0] != '/')
		return 0;

	/* No . or .. component. */
	for (cursor = path; *cursor != '\0'; cursor++) {
		if (cursor[0] != '/' || cursor[1] != '.')
			continue;
		if (cursor[2] == '/' || cursor[2] == '\0')
			return 0;
		if (cursor[2] == '.' && (cursor[3] == '/' || cursor[3] == '\0'))
			return 0;
	}

	/* The same directory as ".". */
	error = stat(path, &named);
	if (error != 0)
		return 0;
	error = stat(".", &current);
	if (error != 0)
		return 0;
	if (named.st_dev != current.st_dev)
		return 0;

	/* Succeeded: whether it is. */
	return named.st_ino == current.st_ino;
}

/*
 * Reads cd's options, which may be joined in one word (-Pe); *index is left
 * at the first operand.  Returns 0 (after a message) for an unknown option.
 */
static int
cd_options(
	int argc,
	char **argv,
	int *index,
	int *physical,
	int *check)
{
	const char *word;
	const char *letter;
	char dashes;

	/* Logical and unchecked unless an option says otherwise. */
	*physical = 0;
	*check = 0;
	for (*index = 1; *index < argc; (*index)++) {
		/* An operand, or - alone, ends the options. */
		word = argv[*index];
		if (word[0] != '-' || word[1] == '\0')
			break;

		/* -- ends them too, and is not an operand. */
		dashes = option_letter(word);
		if (dashes == '-') {
			(*index)++;
			break;
		}

		/* -P and -L (the last one counts), and -e. */
		for (letter = word + 1; *letter != '\0'; letter++) {
			switch (*letter) {
			case 'P':
				*physical = 1;
				break;
			case 'L':
				*physical = 0;
				break;
			case 'e':
				*check = 1;
				break;
			default:
				fprintf(stderr, "cd: Illegal option -%c\n",
					*letter);
				return 0;
			}
		}
	}

	/* Succeeded. */
	return 1;
}

/*
 * Looks for a relative directory on CDPATH, and changes to the first found.
 * An empty entry is the current directory; the new directory is printed
 * when it was found through a non-empty entry.  Returns 1 when it changed.
 */
static int
cd_search_path(
	const char *target,
	int physical,
	int print)
{
	char candidate[PATH_MAX];
	struct stat status;
	const char *cdpath;
	const char *start;
	const char *end;
	size_t length;
	int dot;
	int announce;
	int directory;
	int error;

	/* Only a relative name not starting with . or .. is looked for. */
	if (target[0] == '/')
		return 0;
	dot = is_dot_relative(target);
	if (dot)
		return 0;
	cdpath = sh_var_get("CDPATH");
	if (cdpath == NULL || cdpath[0] == '\0')
		return 0;

	/* Each entry of CDPATH, in order. */
	for (start = cdpath;
	     ;
	     start = end + 1) {
		end = strchr(start, ':');
		if (end == NULL)
			end = start + strlen(start);
		length = (size_t)(end - start);

		/* The entry joined to the name; an empty entry is the current directory. */
		if (length == 0) {
			snprintf(candidate, sizeof(candidate), "%s", target);
		} else {
			snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)length, start, target);
		}

		/* The new directory is printed when it came from a CDPATH entry, or on request. */
		announce = print;
		if (length != 0)
			announce = 1;

		/* A directory there is changed to. */
		error = stat(candidate, &status);
		directory = 0;
		if (error == 0)
			directory = S_ISDIR(status.st_mode);
		if (directory) {
			error = try_chdir(candidate, physical, announce);
			if (error == 0)
				return 1;
		}

		/* The last entry. */
		if (*end == '\0')
			break;
	}

	/* Not found on CDPATH. */
	return 0;
}

/* Reports whether a relative path starts with a . or .. component. */
static int
is_dot_relative(
	const char *path)
{
	/* Not a . at all. */
	if (path[0] != '.')
		return 0;

	/* . alone or ./ */
	if (path[1] == '\0' || path[1] == '/')
		return 1;

	/* .. alone or ../ */
	if (path[1] == '.' && (path[2] == '\0' || path[2] == '/'))
		return 1;

	/* A name that starts with a dot. */
	return 0;
}

/*
 * Changes to a directory, setting the logical directory, PWD and OLDPWD.
 * Returns 0, or -1 with errno.
 */
static int
try_chdir(
	const char *path,
	int physical,
	int print)
{
	char current[PATH_MAX];
	char *logical;
	char *found;
	int error;

	/* The logical path: the operand joined to the logical directory. */
	logical = NULL;
	if (!physical && path[0] == '/')
		logical = logical_path(NULL, path);
	else if (!physical && logical_cwd != NULL)
		logical = logical_path(logical_cwd, path);

	/* Changes there. */
	if (logical != NULL) {
		error = chdir(logical);
		if (error != 0) {
			free(logical);
			logical = NULL;
		}
	}

	/* Or, failing that, to the operand as written, named physically. */
	if (logical == NULL) {
		error = chdir(path);
		if (error != 0)
			return -1;
		found = getcwd(current, sizeof(current));
		if (found != NULL)
			logical = sh_strdup(current);
	}

	/* OLDPWD is where the shell was. */
	if (logical_cwd != NULL)
		(void)sh_var_set("OLDPWD", logical_cwd, SH_VAR_EXPORT);

	/* PWD and the shell's copy follow. */
	if (logical != NULL) {
		free(logical_cwd);
		logical_cwd = logical;
		(void)sh_var_set("PWD", logical_cwd, SH_VAR_EXPORT);
		if (print)
			printf("%s\n", logical_cwd);
	}

	/* Succeeded. */
	return 0;
}

/*
 * Joins a base and a path and resolves . and .. by their names, as a
 * logical cd does.  base NULL means path is absolute.
 */
static char *
logical_path(
	const char *base,
	const char *path)
{
	char *joined;
	char *result;
	char *component;
	size_t length;
	size_t out;

	/* The path to resolve, absolute. */
	if (base == NULL) {
		joined = sh_strdup(path);
	} else {
		length = strlen(base) + strlen(path) + 2U;
		joined = sh_malloc(length);
		snprintf(joined, length, "%s/%s", base, path);
	}

	/* Walks the components, dropping . and backing up for .. */
	result = sh_malloc(strlen(joined) + 2U);
	out = 0;
	component = joined;
	while (*component != '\0') {
		/* The slashes before a component. */
		while (*component == '/')
			component++;
		if (*component == '\0')
			break;
		length = strcspn(component, "/");

		/* . is dropped. */
		if (length == 1 && component[0] == '.') {
			component += length;
			continue;
		}

		/* .. takes off the component before it. */
		if (length == 2 && component[0] == '.' && component[1] == '.') {
			while (out > 0 && result[out - 1] != '/')
				out--;
			if (out > 0)
				out--;
			component += length;
			continue;
		}

		/* Anything else is kept. */
		result[out++] = '/';
		memcpy(result + out, component, length);
		out += length;
		component += length;
	}

	/* The root when nothing is left. */
	if (out == 0)
		result[out++] = '/';
	result[out] = '\0';
	free(joined);

	/* Succeeded: the resolved path, which the caller frees. */
	return result;
}

/*
 * Returns the letter of a one-letter option word such as -P (- for --), or
 * 0 when the word is not one.
 */
static char
option_letter(
	const char *word)
{
	/* A -, a character, and nothing more. */
	if (word[0] != '-' || word[1] == '\0' || word[2] != '\0')
		return 0;

	/* Succeeded: the character. */
	return word[1];
}
