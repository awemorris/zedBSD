/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * su: runs a shell as another user, with that user's password (ws160-p001).
 *
 *   su [-] [-l] [user] [-c command]
 *
 * The user is root unless named.  root runs su without a password; anyone
 * else gives the target's password, read from the terminal (or from
 * standard input when there is no terminal).  A locked account (its shadow
 * entry starts with "!" or "*", as root's on a release) is refused like a
 * wrong password, by login's check (userland/base/login/verify.c).  A
 * failure costs two seconds; every success and failure goes to the system
 * log (auth).
 *
 * The shell is the target's (/bin/sh when it has none) with its groups,
 * group and user IDs.  "-" or -l makes a login shell: an empty environment
 * but for the terminal's and the locale's variables (account.c), the
 * target's HOME, SHELL, USER, LOGNAME and the secure PATH, the working
 * directory the target's home, and "-sh" as the shell's name.  Without it
 * the caller's environment is kept but for the variables that change how
 * programs load or run (LD_*, IFS, ENV, BASH_ENV, CDPATH, PS4, SHELLOPTS),
 * and HOME, SHELL, USER and LOGNAME are the target's.
 *
 * The program is set-user-ID root.  The exit status is the shell's, or 1
 * when su fails, 2 for a wrong use.
 */

#include "userland/base/common/account.h"
#include "userland/base/login/verify.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <readpassphrase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

/* The exit statuses of su itself. */
#define SU_FAILED		1
#define SU_USAGE		2

/* The pause after a failure (seconds). */
#define SU_FAILURE_PAUSE	2U

/* The environment's room. */
#define SU_STORAGE		8192U

/* The caller's variables that never come along. */
static const char *const su_dropped[] = { "LD_", "IFS=", "ENV=", "BASH_ENV=", "CDPATH=", "PS4=", "SHELLOPTS=" };

extern char **environ;

static void clean_environment(void);
static void standard_descriptors(void);
static void wipe(char *buffer, size_t size);

int
main(
	int argc,
	char **argv)
{
	char password[ACCOUNT_PASSWORD_MAX + 2U];
	char buffer[LOGIN_VERIFY_BUFFER];
	char storage[SU_STORAGE];
	char shell_name[64];
	char caller_name[64];
	char *variables[ACCOUNT_ENVIRONMENT_MAX];
	char *shell_argv[4];
	struct account_environment_input input;
	struct passwd account;
	struct passwd *caller;
	struct passwd *target;
	const char *name;
	const char *command;
	const char *shell;
	const char *base;
	char *read;
	size_t count;
	uid_t real;
	uid_t effective;
	int login_shell;
	int dash;
	int dash_l;
	int dash_c;
	int index;
	int error;

	/* Descriptors 0 to 2 open, the log. */
	standard_descriptors();
	openlog("su", LOG_PID, LOG_AUTH);

	/* The arguments: "-" or -l, the user, -c and its command. */
	login_shell = 0;
	name = "root";
	command = NULL;
	for (index = 1; index < argc; index++) {
		dash = strcmp(argv[index], "-");
		dash_l = strcmp(argv[index], "-l");
		dash_c = strcmp(argv[index], "-c");
		if (dash == 0 || dash_l == 0) {
			login_shell = 1;
		} else if (dash_c == 0 && index + 1 < argc) {
			command = argv[index + 1];
			index++;
		} else if (argv[index][0] != '-') {
			name = argv[index];
		} else {
			fprintf(stderr, "usage: su [-] [-l] [user] [-c command]\n");
			return SU_USAGE;
		}
	}

	/* Installed set-user-ID root, or it can do nothing. */
	effective = geteuid();
	if (effective != 0) {
		fprintf(stderr, "su: not installed set-user-ID root\n");
		return SU_FAILED;
	}

	/* The caller, by the real user ID. */
	real = getuid();
	caller = getpwuid(real);
	if (caller == NULL) {
		fprintf(stderr, "su: who are you?\n");
		return SU_FAILED;
	}

	/* Its name, kept. */
	(void)snprintf(caller_name, sizeof(caller_name), "%s", caller->pw_name);

	/* The target, and its password unless root asks. */
	if (real == 0) {
		target = getpwnam(name);
		if (target == NULL) {
			fprintf(stderr, "su: unknown user %s\n", name);
			return SU_FAILED;
		}

		/* Its entry, copied. */
		account = *target;
	} else {
		read = readpassphrase("Password: ", password, sizeof(password), RPP_ECHO_OFF);
		if (read == NULL) {
			fprintf(stderr, "su: no password\n");
			return SU_FAILED;
		}

		/* Checked, the password erased. */
		error = login_verify(name, password, &account, buffer, sizeof(buffer));
		wipe(password, sizeof(password));
		if (error != 0) {
			syslog(LOG_NOTICE, "failed: %s to %s", caller_name, name);
			sleep(SU_FAILURE_PAUSE);
			fprintf(stderr, "su: authentication failed\n");
			return SU_FAILED;
		}
	}

	/* Logged. */
	syslog(LOG_NOTICE, "%s to %s", caller_name, name);

	/* The target's shell. */
	shell = account.pw_shell;
	if (shell == NULL || shell[0] == '\0')
		shell = "/bin/sh";

	/* The target's groups, group and user. */
	error = initgroups(account.pw_name, account.pw_gid);
	if (error == 0)
		error = setgid(account.pw_gid);
	if (error == 0)
		error = setuid(account.pw_uid);
	if (error != 0) {
		fprintf(stderr, "su: cannot become %s: %s\n", name, strerror(errno));
		return SU_FAILED;
	}

	/* The environment: a login shell's anew, otherwise the caller's cleaned. */
	if (login_shell) {
		memset(&input, 0, sizeof(input));
		input.caller = environ;
		input.home = account.pw_dir;
		input.shell = shell;
		input.user = account.pw_name;
		count = account_environment(&input, storage, sizeof(storage), variables, ACCOUNT_ENVIRONMENT_MAX);
		if (count == 0U) {
			fprintf(stderr, "su: the environment does not fit\n");
			return SU_FAILED;
		}

		/* Installed, in the target home. */
		environ = variables;
		error = chdir(account.pw_dir);
		if (error != 0)
			(void)chdir("/");
	} else {
		clean_environment();
		(void)setenv("HOME", account.pw_dir, 1);
		(void)setenv("SHELL", shell, 1);
		(void)setenv("USER", account.pw_name, 1);
		(void)setenv("LOGNAME", account.pw_name, 1);
	}

	/* The shell's name: "-sh" for a login shell. */
	base = strrchr(shell, '/');
	if (base == NULL)
		base = shell;
	else
		base++;
	if (login_shell) {
		(void)snprintf(shell_name, sizeof(shell_name), "-%s", base);
	} else {
		(void)snprintf(shell_name, sizeof(shell_name), "%s", base);
	}

	/* The shell, with the command when there is one. */
	shell_argv[0] = shell_name;
	shell_argv[1] = NULL;
	if (command != NULL) {
		shell_argv[1] = "-c";
		shell_argv[2] = (char *)command;
		shell_argv[3] = NULL;
	}

	/* Run. */
	execv(shell, shell_argv);

	/* Not reached unless the shell could not run. */
	fprintf(stderr, "su: %s: %s\n", shell, strerror(errno));
	return SU_FAILED;
}

/* Removes the caller's variables that change how programs load or run. */
static void
clean_environment(void)
{
	char name[128];
	size_t index;
	size_t kind;
	size_t length;
	int dropped;
	int same;

	/* Each variable, from the start again after a removal. */
	index = 0;
	while (environ != NULL && environ[index] != NULL) {
		/* One of the dropped kinds. */
		dropped = 0;
		for (kind = 0; kind < sizeof(su_dropped) / sizeof(su_dropped[0]); kind++) {
			length = strlen(su_dropped[kind]);
			same = strncmp(environ[index], su_dropped[kind], length);
			if (same == 0)
				dropped = 1;
		}

		/* A kept variable: the next one. */
		if (!dropped) {
			index++;
			continue;
		}

		/* Removed by its name. */
		length = strcspn(environ[index], "=");
		if (length >= sizeof(name)) {
			index++;
			continue;
		}

		/* Its name. */
		memcpy(name, environ[index], length);
		name[length] = '\0';
		(void)unsetenv(name);
		index = 0;
	}
}

/* Opens /dev/null on any of descriptors 0 to 2 that is closed. */
static void
standard_descriptors(void)
{
	int descriptor;
	int flags;
	int index;

	/* Each of the three. */
	for (index = 0; index < 3; index++) {
		flags = fcntl(index, F_GETFD);
		if (flags != -1)
			continue;
		descriptor = open("/dev/null", O_RDWR);
		if (descriptor < 0)
			_exit(SU_FAILED);
	}
}

/* Overwrites a buffer that held a secret. */
static void
wipe(
	char *buffer,
	size_t size)
{
	volatile char *byte;
	size_t index;

	/* Byte by byte, so that the compiler keeps the writes. */
	byte = buffer;
	for (index = 0; index < size; index++)
		byte[index] = '\0';
}
