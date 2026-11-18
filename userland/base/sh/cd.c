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
 *
 * pushd, popd and dirs (bash) keep a stack of directories under the
 * current one, which is always its top.
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

/* The options of dirs: -l, -p, -v and -c. */
#define DIRS_LONG	0x01
#define DIRS_LINES	0x02
#define DIRS_NUMBERED	0x04
#define DIRS_CLEAR	0x08

/*
 * The shell's logical current directory: set at startup and by cd; NULL
 * only when the current directory could not be named at all.
 */
static char *logical_cwd;

/*
 * The directory stack of pushd under the current directory: dir_stack[0]
 * is the entry after the top, which is always the current directory.
 *
 * The array and its strings belong to the stack; pushd grows it, popd and
 * dirs -c shrink it, and it lives as long as the shell.
 */
static char **dir_stack;

/* The number of entries in dir_stack, 0 when only the top is there. */
static int dir_stack_count;

static int cd_options(int argc, char **argv, int *index, int *physical, int *check);
static int cd_search_path(const char *target, int physical, int print);
static int is_dot_relative(const char *path);
static int try_chdir(const char *path, int physical, int print);
static char *logical_path(const char *base, const char *path);
static char option_letter(const char *word);
static int pushd_directory(const char *path, int no_change);
static int pushd_swap(void);
static int pushd_rotate(int position, int no_change);
static int dirs_options(int argc, char **argv, int *options, int *position);
static int dirs_letters(const char *letters, int *options);
static int is_position_word(const char *word);
static int stack_options(int argc, char **argv, const char *command, int *index, int *no_change, int *position);
static int stack_position(const char *word, const char *command, int *position);
static int stack_change(const char *path);
static void stack_insert(int at, const char *path);
static void stack_remove(int at);
static const char *stack_entry(int at);
static void stack_print_entry(int at, int long_form);
static void stack_print(int long_form, int lines, int numbered);

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
 * Implements pushd (bash).
 *
 * pushd dir changes to dir and pushes the directory it left; pushd alone
 * swaps the top two; pushd +N and -N rotate the stack so that the Nth
 * entry (from the left, or the right) is the top.  -n pushes dir without
 * changing to it.  Prints the stack.
 */
int
sh_builtin_pushd(
	int argc,
	char **argv)
{
	int position;
	int no_change;
	int index;
	int valid;
	int status;

	/* Reads the options, and an operand of +N or -N. */
	valid = stack_options(argc, argv, "pushd", &index, &no_change, &position);
	if (!valid)
		return 1;

	/* A directory is changed to (unless -n), and pushed under it. */
	if (index < argc && position < 0) {
		status = pushd_directory(argv[index], no_change);
		return status;
	}

	/* Alone, pushd swaps the top two. */
	if (position < 0) {
		status = pushd_swap();
		return status;
	}

	/* +N or -N rotates the stack, the Nth entry first. */
	status = pushd_rotate(position, no_change);

	/* Succeeded: 1 when the directory could not be changed. */
	return status;
}

/*
 * Implements popd (bash).
 *
 * popd removes the top and changes to the entry under it; popd +N and -N
 * remove the Nth entry.  -n removes the entry under the top without
 * changing directory.  Prints the stack.
 */
int
sh_builtin_popd(
	int argc,
	char **argv)
{
	int position;
	int no_change;
	int index;
	int valid;
	int status;

	/* Reads the options, and an operand of +N or -N. */
	valid = stack_options(argc, argv, "popd", &index, &no_change, &position);
	if (!valid)
		return 1;

	/* Refuses any other operand. */
	if (index < argc && position < 0) {
		fprintf(stderr, "popd: %s: invalid argument\n", argv[index]);
		return 1;
	}

	/* Refuses to pop the top alone. */
	if (dir_stack_count == 0) {
		fprintf(stderr, "popd: directory stack empty\n");
		return 1;
	}

	/* The top goes, and the directory changes to the entry under it. */
	if (position <= 0) {
		if (!no_change) {
			status = stack_change(dir_stack[0]);
			if (status != 0)
				return 1;
		}

		/* The entry under the old top is the new top. */
		stack_remove(0);
	} else {
		/* Any other entry just goes. */
		stack_remove(position - 1);
	}

	/* Prints the stack left. */
	stack_print(0, 0, 0);

	/* Succeeded: the entry is popped. */
	return 0;
}

/*
 * Implements dirs (bash).
 *
 * Prints the stack, with ~ for HOME unless -l, one to a line with -p,
 * numbered with -v; -c empties it; +N and -N print the Nth entry.
 */
int
sh_builtin_dirs(
	int argc,
	char **argv)
{
	int options;
	int position;
	int valid;
	int lines;

	/* Reads the options, joined or not, and an operand of +N or -N. */
	valid = dirs_options(argc, argv, &options, &position);
	if (!valid)
		return 1;

	/* -c empties the stack. */
	if ((options & DIRS_CLEAR) != 0) {
		while (dir_stack_count > 0)
			stack_remove(0);
		return 0;
	}

	/* +N or -N prints one entry. */
	if (position >= 0) {
		stack_print_entry(position, options & DIRS_LONG);
		putchar('\n');
		return 0;
	}

	/* Prints every entry; -v numbers them, one to a line as -p does. */
	lines = 0;
	if ((options & (DIRS_LINES | DIRS_NUMBERED)) != 0)
		lines = 1;
	stack_print(options & DIRS_LONG, lines, options & DIRS_NUMBERED);

	/* Succeeded: the stack is printed. */
	return 0;
}

/*
 * Pushes a directory for pushd dir: changes to it (unless no_change) and
 * pushes the directory it left under it.  Returns 1 when the directory
 * could not be changed.
 */
static int
pushd_directory(
	const char *path,
	int no_change)
{
	const char *entry;
	char *left;
	int status;

	/* -n pushes the directory without changing to it. */
	if (no_change) {
		stack_insert(0, path);
		stack_print(0, 0, 0);
		return 0;
	}

	/* Changes to the directory, keeping the one it leaves. */
	entry = stack_entry(0);
	left = sh_strdup(entry);
	status = stack_change(path);
	if (status != 0) {
		free(left);
		return 1;
	}

	/* Pushes the directory left under the new top. */
	stack_insert(0, left);
	free(left);

	/* Prints the stack. */
	stack_print(0, 0, 0);

	/* Succeeded: the directory is the new top. */
	return 0;
}

/*
 * Swaps the top two entries for pushd alone, changing to the second.
 * Returns 1 when there is none or it could not be changed to.
 */
static int
pushd_swap(void)
{
	const char *entry;
	char *left;
	int status;

	/* Refuses a stack of the top alone. */
	if (dir_stack_count == 0) {
		fprintf(stderr, "pushd: no other directory\n");
		return 1;
	}

	/* Changes to the entry under the top, keeping the one it leaves. */
	entry = stack_entry(0);
	left = sh_strdup(entry);
	status = stack_change(dir_stack[0]);
	if (status != 0) {
		free(left);
		return 1;
	}

	/* The directory left takes the place of the new top. */
	free(dir_stack[0]);
	dir_stack[0] = left;

	/* Prints the stack. */
	stack_print(0, 0, 0);

	/* Succeeded: the top two are swapped. */
	return 0;
}

/*
 * Rotates the stack for pushd +N or -N so that the entry at a place from
 * the top becomes the top, and changes to it.  Returns 1 when it could not
 * be changed to.
 */
static int
pushd_rotate(
	int position,
	int no_change)
{
	const char *entry;
	char **rotated;
	int total;
	int status;
	int at;

	/* The top itself, or -n, leaves the stack as it is. */
	if (position == 0 || no_change) {
		stack_print(0, 0, 0);
		return 0;
	}

	/* Copies the entries in their new order, the Nth first. */
	total = dir_stack_count + 1;
	rotated = sh_malloc((size_t)total * sizeof(*rotated));
	for (at = 0; at < total; at++) {
		entry = stack_entry((position + at) % total);
		rotated[at] = sh_strdup(entry);
	}

	/* Changes to the new top; a failure leaves the stack as it was. */
	status = stack_change(rotated[0]);
	if (status != 0) {
		for (at = 0; at < total; at++)
			free(rotated[at]);
		free(rotated);
		return 1;
	}

	/* The entries under the new top replace the old ones. */
	for (at = 0; at < dir_stack_count; at++) {
		free(dir_stack[at]);
		dir_stack[at] = rotated[at + 1];
	}

	/* The new top is the current directory, not an entry. */
	free(rotated[0]);
	free(rotated);

	/* Prints the stack. */
	stack_print(0, 0, 0);

	/* Succeeded: the stack is rotated. */
	return 0;
}

/*
 * Reads the options of dirs, joined or not, into DIRS_* bits, and an
 * operand of +N or -N into *position (-1 for none).  Returns 0 after a
 * message.
 */
static int
dirs_options(
	int argc,
	char **argv,
	int *options,
	int *position)
{
	const char *word;
	int is_position;
	int valid;
	int index;

	/* Reads each word. */
	*options = 0;
	*position = -1;
	for (index = 1; index < argc; index++) {
		/* +N or -N names an entry. */
		word = argv[index];
		is_position = is_position_word(word);
		if (is_position) {
			valid = stack_position(word, "dirs", position);
			if (!valid)
				return 0;
			continue;
		}

		/* Refuses an operand that is not an option. */
		if (word[0] != '-' || word[1] == '\0') {
			fprintf(stderr, "dirs: %s: invalid argument\n", word);
			return 0;
		}

		/* Reads each letter of the option word. */
		valid = dirs_letters(word + 1, options);
		if (!valid)
			return 0;
	}

	/* Succeeded: the options are read. */
	return 1;
}

/*
 * Reads the letters of one option word of dirs into DIRS_* bits.  Returns
 * 0 after reporting a letter it does not take.
 */
static int
dirs_letters(
	const char *letters,
	int *options)
{
	const char *cursor;

	/* Sets the bit each letter names. */
	for (cursor = letters; *cursor != '\0'; cursor++) {
		if (*cursor == 'l') {
			*options |= DIRS_LONG;
		} else if (*cursor == 'p') {
			*options |= DIRS_LINES;
		} else if (*cursor == 'v') {
			*options |= DIRS_NUMBERED;
		} else if (*cursor == 'c') {
			*options |= DIRS_CLEAR;
		} else {
			fprintf(stderr, "dirs: -%c: invalid option\n", *cursor);
			return 0;
		}
	}

	/* Succeeded: every letter is read. */
	return 1;
}

/* Reports whether a word is +N or -N, a place in the directory stack. */
static int
is_position_word(
	const char *word)
{
	/* Refuses a word without a sign. */
	if (word[0] != '+' && word[0] != '-')
		return 0;

	/* Refuses a sign without a digit after it. */
	if (word[1] < '0' || word[1] > '9')
		return 0;

	/* Succeeded: a place in the stack. */
	return 1;
}

/*
 * Reads the options of pushd and popd (-n) and an operand of +N or -N,
 * which *position is set to as a place from the top (-1 for none).
 * *index is left at the operand.  Returns 0 after a message.
 */
static int
stack_options(
	int argc,
	char **argv,
	const char *command,
	int *index,
	int *no_change,
	int *position)
{
	const char *word;
	int is_position;
	int compare;
	int valid;
	int at;

	/* Reads -n, and +N or -N, up to -- or the first other operand. */
	*no_change = 0;
	*position = -1;
	for (at = 1; at < argc; at++) {
		/* -n keeps the current directory. */
		word = argv[at];
		compare = strcmp(word, "-n");
		if (compare == 0) {
			*no_change = 1;
			continue;
		}

		/* -- ends the options. */
		compare = strcmp(word, "--");
		if (compare == 0) {
			at++;
			break;
		}

		/* +N or -N names an entry. */
		is_position = is_position_word(word);
		if (is_position) {
			valid = stack_position(word, command, position);
			if (!valid)
				return 0;
			continue;
		}

		/* Refuses any other option. */
		if (word[0] == '-' && word[1] != '\0') {
			fprintf(stderr, "%s: %s: invalid option\n", command, word);
			return 0;
		}

		/* Anything else is the operand. */
		break;
	}

	/* The operand, if any, is where the options end. */
	*index = at;

	/* Succeeded: the options are read. */
	return 1;
}

/*
 * Reads +N (from the top) or -N (from the bottom) as a place from the top.
 * Returns 0 after a message when it is not in the stack.
 */
static int
stack_position(
	const char *word,
	const char *command,
	int *position)
{
	char *end;
	long value;
	int total;

	/* Refuses anything but a number of an entry after the sign. */
	value = strtol(word + 1, &end, 10);
	total = dir_stack_count + 1;
	if (*end != '\0' || value >= total) {
		fprintf(stderr,
			"%s: %s: directory stack index out of range\n",
			command,
			word);
		return 0;
	}

	/* Counts -N from the bottom, so that the place is from the top. */
	*position = (int)value;
	if (word[0] == '-')
		*position = total - 1 - (int)value;

	/* Succeeded: the place is stored. */
	return 1;
}

/* Changes to a directory of the stack as cd does; returns 0 when it did. */
static int
stack_change(
	const char *path)
{
	char *arguments[4];
	int status;

	/* Runs cd -- path. */
	arguments[0] = "cd";
	arguments[1] = "--";
	arguments[2] = (char *)path;
	arguments[3] = NULL;
	status = sh_builtin_cd(3, arguments);

	/* Reports that cd failed; it has written why. */
	if (status != 0)
		return status;

	/* Succeeded: the shell is in the directory. */
	return 0;
}

/* Puts a copy of a path into the stack at a place under the top. */
static void
stack_insert(
	int at,
	const char *path)
{
	size_t size;

	/* Makes room for one more entry, the ones from the place on moved down. */
	size = ((size_t)dir_stack_count + 1U) * sizeof(*dir_stack);
	dir_stack = sh_realloc(dir_stack, size);
	memmove(dir_stack + at + 1,
		dir_stack + at,
		(size_t)(dir_stack_count - at) * sizeof(*dir_stack));

	/* Stores the copy; the stack owns it. */
	dir_stack[at] = sh_strdup(path);
	dir_stack_count++;
}

/* Takes an entry under the top out of the stack. */
static void
stack_remove(
	int at)
{
	/* Frees the entry and moves the ones after it up. */
	free(dir_stack[at]);
	memmove(dir_stack + at,
		dir_stack + at + 1,
		(size_t)(dir_stack_count - at - 1) * sizeof(*dir_stack));
	dir_stack_count--;
}

/* Returns the entry at a place from the top (0 is the current directory). */
static const char *
stack_entry(
	int at)
{
	/* The top is where the shell is; . before the path is known. */
	if (at == 0) {
		if (logical_cwd == NULL)
			return ".";
		return logical_cwd;
	}

	/* Succeeded: an entry under the top. */
	return dir_stack[at - 1];
}

/* Prints an entry of the stack, with ~ for HOME unless in long form. */
static void
stack_print_entry(
	int at,
	int long_form)
{
	const char *entry;
	const char *home;
	size_t length;
	int compare;

	/* HOME at the start becomes ~. */
	entry = stack_entry(at);
	home = sh_var_get("HOME");
	if (!long_form && home != NULL && home[0] != '\0') {
		length = strlen(home);
		compare = strncmp(entry, home, length);
		if (compare == 0 &&
		    (entry[length] == '/' || entry[length] == '\0')) {
			printf("~%s", entry + length);
			return;
		}
	}

	/* Prints the entry as it is. */
	fputs(entry, stdout);
}

/*
 * Prints the stack from the top: on a line, one to a line, or one to a
 * line numbered.
 */
static void
stack_print(
	int long_form,
	int lines,
	int numbered)
{
	int at;

	/* Prints each entry, separated as asked. */
	for (at = 0; at <= dir_stack_count; at++) {
		if (numbered)
			printf("%2d  ", at);
		stack_print_entry(at, long_form);

		/* Ends the entry's line, or puts a space before the next entry. */
		if (lines)
			putchar('\n');
		else if (at < dir_stack_count)
			putchar(' ');
	}

	/* Ends the one line, unless each entry had its own. */
	if (!lines)
		putchar('\n');
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
