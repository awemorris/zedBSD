/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD login userland command.
 */

#include <crypt.h>
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <utmpx.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int read_line(char *buffer, size_t size, int echo);
static void utmp_fill(struct utmpx *entry, int type, pid_t pid, const char *user, const char *line);

/*
 * Runs the login command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	int empty;
	const char *shell;
	char name[64], password[256], tty[64];
	struct passwd account, *found;
	struct spwd shadow, *shadow_found;
	char pwbuf[2048], spbuf[2048];
	char *hash;
	pid_t child, waited;
	int status;
	struct utmpx record;
	char *shell_argv[2];
	char *environment[6];
	char home[320], user[96], logname[96];

	status = 0;

	/* Handles a failed geteuid operation. */
	if (geteuid() != 0) {
		fprintf(stderr, "login: must be run as root\n");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed ttyname r operation. */
	if (ttyname_r(STDIN_FILENO, tty, sizeof(tty)) != 0)
		strcpy(tty, "/dev/console");

	/* Validates the command-line arguments. */
	if (argc > 1) {
		strncpy(name, argv[1], sizeof(name) - 1U);
		name[sizeof(name) - 1U] = '\0';
	} else {
		printf("login: ");
		fflush(stdout);

		/* Handles a failed read line operation. */
		if (read_line(name, sizeof(name), 1) != 0 || name[0] == '\0')
			return 1;
	}

	/* Handles a failed getpwnam r operation. */
	if (getpwnam_r(name, &account, pwbuf, sizeof(pwbuf), &found) != 0 ||
	    found == NULL ||
	    getspnam_r(name, &shadow, spbuf, sizeof(spbuf), &shadow_found) != 0 ||
	    shadow_found == NULL) {
		printf("Password: ");
		fflush(stdout);
		(void)read_line(password, sizeof(password), 0);
		puts("Login incorrect");

		/* Reports operation failure. */
		return 1;
	}

	printf("Password: ");
	fflush(stdout);

	/* Handles a failed read line operation. */
	if (read_line(password, sizeof(password), 0) != 0)
		return 1;

	/* If the login is invalidated. */
	if (shadow.sp_pwdp[0] == '!' || shadow.sp_pwdp[0] == '*') {
		memset(password, 0, sizeof(password));
		puts("Login incorrect");

		/* Reports operation failure. */
		return 1;
	}

	/* If the password is empty. */
	if (shadow.sp_pwdp[0] == '\0') {
				empty = password[0] == '\0';

		/* Shred. */
		memset(password, 0, sizeof(password));

		/* If the entered password is not empty. */
		if (!empty) {
			puts("Login incorrect");

			/* Reports operation failure. */
			return 1;
		}
	} else {
		hash = crypt(password, shadow.sp_pwdp);

		/* Shred. */
		memset(password, 0, sizeof(password));

		/* Handles the hash availability. */
		if (hash == NULL || strcmp(hash, shadow.sp_pwdp)) {
			puts("Login incorrect");

			/* Reports operation failure. */
			return 1;
		}
	}

	/* Launch a child process. */
	child = fork();

	/* Checks the child process state. */
	if (child < 0) {
		fprintf(stderr, "login: fork: %s\n", strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Checks the child process state. */
	if (child == 0) {
				shell = account.pw_shell[0] ?
			account.pw_shell :
			"/bin/sh";

		/* Handles the initgroups condition. */
		if (initgroups(account.pw_name, account.pw_gid) ||
		    setgid(account.pw_gid) ||
		    setuid(account.pw_uid))
			_exit(126);

		/* Handles a failed chdir operation. */
		if (chdir(account.pw_dir) != 0)
			(void)chdir("/");

		snprintf(home, sizeof(home), "HOME=%s", account.pw_dir);
		snprintf(user, sizeof(user), "USER=%s", account.pw_name);
		snprintf(logname, sizeof(logname), "LOGNAME=%s", account.pw_name);

		environment[0] = home;
		environment[1] = user;
		environment[2] = logname;
		environment[3] = "PATH=/bin:/sbin:/usr/bin";
		environment[4] = "SHELL=/bin/sh";
		environment[5] = NULL;

		shell_argv[0] = "-sh";
		shell_argv[1] = NULL;

		execve(shell, shell_argv, environment);

		_exit(127);
	}

	utmp_fill(&record, USER_PROCESS, child, account.pw_name, tty);

	(void)pututxline(&record);
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);

	utmp_fill(&record, DEAD_PROCESS, child, "", tty);

	(void)pututxline(&record);

	/* Handles the waited condition. */
	if (waited < 0) {
		fprintf(stderr, "login: waitpid: %s\n", strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Computes the function result. */
	function_result = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the read line operation. */
static int
read_line(
	char *buffer,
	size_t size,
	int echo)
{
	ssize_t count;
	struct termios saved, mode;
	size_t used;
	char byte;
	int changed;
	int completed;

	used = 0;
	changed = 0;
	completed = 0;

	/* Handles a failed tcgetattr operation. */
	if (!echo && tcgetattr(STDIN_FILENO, &saved) == 0) {
		mode = saved;
		mode.c_lflag &= ~ECHO;

		/* Handles a failed tcsetattr operation. */
		if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &mode) == 0)
			changed = 1;
	}

	while (used + 1U < size) {

		count = read(STDIN_FILENO, &byte, 1);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count != 1)
			break;

		/* Classifies the current byte. */
		if (byte == '\r' || byte == '\n') {
			completed = 1;
			break;
		}
		buffer[used++] = byte;
	}
	buffer[used] = '\0';

	/* Handles the changed condition. */
	if (changed) {
		(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
		puts("");
	}

	/* Returns the computed result. */
	return completed || used != 0 ? 0 : -1;
}

/* Supports the utmp fill operation. */
static void
utmp_fill(
	struct utmpx *entry,
	int type,
	pid_t pid,
	const char *user,
	const char *line)
{
	struct timespec now;
	const char *base;

	base = strrchr(line, '/');

	memset(entry, 0, sizeof(*entry));
	entry->ut_type = (int16_t)type;
	entry->ut_pid = pid;

	/* Handles the base availability. */
	if (base != NULL)
		line = base + 1;

	strncpy(entry->ut_line, line, sizeof(entry->ut_line) - 1U);
	strncpy(entry->ut_id, line, sizeof(entry->ut_id));

	/* Handles the user availability. */
	if (user != NULL)
		strncpy(entry->ut_user, user, sizeof(entry->ut_user) - 1U);

	/* Handles a failed clock gettime operation. */
	if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
		entry->ut_tv_sec = now.tv_sec;
		entry->ut_tv_usec = (int32_t)(now.tv_nsec / 1000L);
	}
}
