/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The POSIX systems' Login Shell (login-shell.h).
 *
 * The shell is the one SHELL names, as a terminal program starts it, or
 * else the user's entry in the password database names; failing both,
 * /bin/sh.  It is started by libkeiland's processes on a terminal of its
 * own (the terminal is its controlling one, at the grid's size), with -l
 * so that it runs as a login shell (reads the login profile), in the
 * user's home directory, with the TERM the settings give.
 */

#include "mikuterm.h"
#include "login-shell.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* The shell when neither SHELL nor the password database names one that runs. */
#define LOGIN_SHELL_FALLBACK	"/bin/sh"

/* The switch that makes the shell a login shell (bash, zsh, fish, dash, ksh, tcsh and BusyBox's ash take it). */
#define LOGIN_SHELL_SWITCH	"-l"

/* The room the password database's entry is read into, when the system does not tell. */
#define LOGIN_SHELL_ENTRY_MAX	16384U

/* The longest TERM=... in the shell's environment, with its NUL. */
#define LOGIN_SHELL_TERM_MAX	(MIKUTERM_TERM_MAX + 8U)

static int login_shell_runs(const char *path);
static int login_shell_entry(char *shell, size_t shell_size, char *home, size_t home_size);
static int login_shell_copy(const char *text, char *out, size_t size);

/*
 * Finds the user's login shell: SHELL's, else the password database's,
 * else /bin/sh, each taken only when it is a path from the root to a file
 * the user may run.  Writes its path into path (size bytes with its NUL).
 * Returns 0, or ENOENT when none runs.
 */
int
login_shell_find(
	char *path,
	size_t size)
{
	char shell[MIKUTERM_PATH_MAX];
	char home[MIKUTERM_PATH_MAX];
	const char *named;
	int runs;
	int error;

	/* SHELL, as terminal programs take it first. */
	named = getenv("SHELL");
	if (named != NULL) {
		runs = login_shell_runs(named);
		if (runs) {
			error = login_shell_copy(named, path, size);
			if (error == 0)
				return 0;
		}
	}

	/* The user's entry in the password database. */
	error = login_shell_entry(shell, sizeof(shell), home, sizeof(home));
	if (error == 0) {
		runs = login_shell_runs(shell);
		if (runs) {
			error = login_shell_copy(shell, path, size);
			if (error == 0)
				return 0;
		}
	}

	/* The shell every POSIX system has. */
	runs = login_shell_runs(LOGIN_SHELL_FALLBACK);
	if (!runs)
		return ENOENT;
	error = login_shell_copy(LOGIN_SHELL_FALLBACK, path, size);
	if (error != 0)
		return ENOENT;

	/* Succeeded: the fallback. */
	return 0;
}

/*
 * Starts the shell of path as a login shell on a terminal of its own of
 * columns by rows, with TERM set to term, in the user's home directory
 * (HOME, else the password database's; the program's own when neither
 * names one).  Returns 0 with *process, or kl_process_start's error.
 */
int
login_shell_start(
	struct kl_app *app,
	const char *path,
	const char *term,
	unsigned columns,
	unsigned rows,
	struct kl_process **process)
{
	struct kl_process_options options;
	const char *arguments[3];
	const char *environment[2];
	char variable[LOGIN_SHELL_TERM_MAX];
	char shell[MIKUTERM_PATH_MAX];
	char home[MIKUTERM_PATH_MAX];
	const char *directory;
	int error;

	/* The shell, as a login shell. */
	arguments[0] = path;
	arguments[1] = LOGIN_SHELL_SWITCH;
	arguments[2] = NULL;

	/* The terminal's type the tab speaks. */
	(void)snprintf(variable, sizeof(variable), "TERM=%s", term);
	environment[0] = variable;
	environment[1] = NULL;

	/* The home directory, where a terminal's shell starts. */
	directory = getenv("HOME");
	if (directory == NULL || directory[0] != '/') {
		directory = NULL;
		error = login_shell_entry(shell, sizeof(shell), home, sizeof(home));
		if (error == 0 && home[0] == '/')
			directory = home;
	}

	/* On a terminal of the grid's size. */
	memset(&options, 0, sizeof(options));
	options.argv = arguments;
	options.environment = environment;
	options.directory = directory;
	options.streams = KL_PROCESS_TERMINAL;
	options.columns = columns;
	options.rows = rows;
	error = kl_process_start(app, &options, process);
	if (error != 0)
		return error;

	/* Succeeded: the shell runs. */
	return 0;
}

/* Tells whether a path is one from the root to a file the user may run (1) or not (0). */
static int
login_shell_runs(
	const char *path)
{
	struct stat status;
	int error;

	/* Only a path from the root. */
	if (path[0] != '/')
		return 0;

	/* A file, not a directory. */
	error = stat(path, &status);
	if (error != 0)
		return 0;
	if (!S_ISREG(status.st_mode))
		return 0;

	/* One the user may run. */
	error = access(path, X_OK);
	if (error != 0)
		return 0;

	/* Runs. */
	return 1;
}

/*
 * Reads the user's shell and home directory from the password database
 * (the entry of the real user).  Returns 0, ENOENT when there is no
 * entry, or ENOMEM.
 */
static int
login_shell_entry(
	char *shell,
	size_t shell_size,
	char *home,
	size_t home_size)
{
	struct passwd entry;
	struct passwd *found;
	char *buffer;
	long suggested;
	size_t room;
	int error;

	/* The room the system suggests for an entry. */
	shell[0] = '\0';
	home[0] = '\0';
	suggested = sysconf(_SC_GETPW_R_SIZE_MAX);
	room = LOGIN_SHELL_ENTRY_MAX;
	if (suggested > 0 && (size_t)suggested > room)
		room = (size_t)suggested;
	buffer = malloc(room);
	if (buffer == NULL)
		return ENOMEM;

	/* The entry (the reentrant call: a tab's thread may ask while another does). */
	found = NULL;
	error = getpwuid_r(getuid(), &entry, buffer, room, &found);
	if (error != 0 || found == NULL) {
		free(buffer);
		return ENOENT;
	}

	/* Its shell and home, when they fit. */
	if (entry.pw_shell != NULL)
		(void)login_shell_copy(entry.pw_shell, shell, shell_size);
	if (entry.pw_dir != NULL)
		(void)login_shell_copy(entry.pw_dir, home, home_size);
	free(buffer);

	/* Succeeded: the entry's fields. */
	return 0;
}

/* Copies a text into out (size bytes with its NUL) when it fits whole.  Returns 0 or ENAMETOOLONG (out empty). */
static int
login_shell_copy(
	const char *text,
	char *out,
	size_t size)
{
	size_t length;

	/* Only the whole text. */
	length = strlen(text);
	if (length >= size) {
		if (size != 0U)
			out[0] = '\0';
		return ENAMETOOLONG;
	}

	/* Succeeded: copied. */
	memcpy(out, text, length + 1U);
	return 0;
}
