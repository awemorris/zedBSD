/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Linked into the host build of zedBSD's sshd that interop-host.sh runs,
 * so that the real listener, monitor and children never touch the host
 * account: getpwnam_r answers the account the test runs as with the home
 * directory "home" and the shell "shell.sh" of the directory the sshd
 * binary is in, and no other account exists; syslog writes to the file
 * "log" there.  (A directory without shell.sh gives an account whose
 * shell cannot be executed.)  The directory is found from the program's
 * own path, since the monitor starts its children with an empty
 * environment.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

/* The room for paths and for the account's name. */
#define SHIM_PATH_MAX	1024U
#define SHIM_NAME_MAX	256U

/* The directory of the binary, and the name of the account the test runs as, found once. */
static char shim_directory[SHIM_PATH_MAX];
static char shim_user[SHIM_NAME_MAX];

/* Finds the binary's directory and the account's real name (getpwuid_r is not replaced). */
static void
shim_find(void)
{
	struct passwd account;
	struct passwd *found;
	char buffer[2048];
	char *slash;
	ssize_t got;

	if (shim_directory[0] != '\0')
		return;
	got = readlink("/proc/self/exe", shim_directory, sizeof(shim_directory) - 1U);
	if (got <= 0) {
		strcpy(shim_directory, "/nonexistent");
	} else {
		shim_directory[got] = '\0';
		slash = strrchr(shim_directory, '/');
		if (slash != NULL)
			*slash = '\0';
	}

	found = NULL;
	if (getpwuid_r(getuid(), &account, buffer, sizeof(buffer), &found) == 0 && found != NULL)
		strncpy(shim_user, account.pw_name, sizeof(shim_user) - 1U);
}

/* Answers only the test's account, with the binary directory's home and shell. */
int
getpwnam_r(
	const char *name,
	struct passwd *account,
	char *buffer,
	size_t size,
	struct passwd **result)
{
	int written;

	*result = NULL;
	shim_find();
	if (shim_user[0] == '\0' || strcmp(name, shim_user) != 0)
		return 0;
	memset(account, 0, sizeof(*account));
	written = snprintf(buffer, size, "%s%cx%c%s/home%c%s/shell.sh%c", shim_user, 0, 0, shim_directory, 0, shim_directory, 0);
	if (written < 0 || (size_t)written >= size)
		return ERANGE;
	account->pw_name = buffer;
	account->pw_passwd = buffer + strlen(shim_user) + 1U;
	account->pw_dir = account->pw_passwd + 2;
	account->pw_shell = account->pw_dir + strlen(account->pw_dir) + 1U;
	account->pw_gecos = account->pw_shell + strlen(account->pw_shell);
	account->pw_uid = getuid();
	account->pw_gid = getgid();
	*result = account;
	return 0;
}

/* The log goes to the directory's file. */
void
openlog(
	const char *identity,
	int option,
	int facility)
{
	(void)identity;
	(void)option;
	(void)facility;
}

/* Appends one line, with the process id, to the directory's log file. */
void
vsyslog(
	int priority,
	const char *format,
	va_list arguments)
{
	char line[1024];
	char path[SHIM_PATH_MAX + 8U];
	int length;
	int descriptor;
	ssize_t written;

	shim_find();
	length = snprintf(line, sizeof(line), "%ld %d ", (long)getpid(), priority & 7);
	(void)vsnprintf(line + length, sizeof(line) - (size_t)length - 1U, format, arguments);
	strcat(line, "\n");
	(void)snprintf(path, sizeof(path), "%s/log", shim_directory);
	descriptor = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	if (descriptor < 0)
		return;
	written = write(descriptor, line, strlen(line));
	(void)written;
	(void)close(descriptor);
}

/* As vsyslog. */
void
syslog(
	int priority,
	const char *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	vsyslog(priority, format, arguments);
	va_end(arguments);
}
