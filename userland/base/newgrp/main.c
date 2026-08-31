/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD newgrp userland command.
 */

#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

static void usage(void);
static int authorized(const struct group *group, const struct passwd *account);
static int member_of(const struct group *group, const struct passwd *account);
static int read_password(char *buffer, size_t capacity);

/*
 * Runs the newgrp command.
 */
int
main(
	int argc,
	char **argv)
{
	struct passwd *account;
	struct group *group;
	struct passwd account_storage;
	struct group group_storage;
	char account_buffer[2048];
	char group_buffer[2048];
	const char *group_name;
	const char *shell;
	char login_name[128];
	char *shell_argv[2];
	int login_shell;

	group_name = NULL;
	login_shell = 0;

	/* Handles the selected command-line operation. */
	if (argc > 1 && (!strcmp(argv[1], "-l") || !strcmp(argv[1], "-"))) {
		login_shell = 1;
		argv++;
		argc--;
	}

	/* Validates the command-line arguments. */
	if (argc > 2) {
		usage();

		/* Reports operation failure. */
		return 2;
	}

	/* Validates the command-line arguments. */
	if (argc == 2)
		group_name = argv[1];

	/* Handles a failed getpwuid r operation. */
	if (getpwuid_r(getuid(), &account_storage, account_buffer,
		       sizeof(account_buffer), &account) != 0 ||
	    account == NULL) {
		fprintf(stderr, "newgrp: cannot identify uid %u\n",
			(unsigned)getuid());

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed getgrnam r operation. */
	if ((group_name != NULL
		 ? getgrnam_r(group_name, &group_storage, group_buffer,
			      sizeof(group_buffer), &group)
		 : getgrgid_r(account->pw_gid, &group_storage, group_buffer,
			      sizeof(group_buffer), &group)) != 0 ||
	    group == NULL) {
		fprintf(stderr, "newgrp: unknown group: %s\n",
			group_name != NULL ? group_name : "(primary)");

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed authorized operation. */
	if (!authorized(group, account)) {
		fprintf(stderr, "newgrp: permission denied for group %s\n",
			group->gr_name);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed initgroups operation. */
	if (initgroups(account->pw_name, group->gr_gid) != 0 ||
	    setgid(group->gr_gid) != 0) {
		fprintf(stderr, "newgrp: credentials: %s\n", strerror(errno));

		/* Reports operation failure. */
		return 1;
	}
	shell = account->pw_shell != NULL && account->pw_shell[0] != '\0'
		    ? account->pw_shell
		    : "/bin/sh";

	/* Handles the login shell condition. */
	if (login_shell) {
		/* Handles a failed chdir operation. */
		if (chdir(account->pw_dir) != 0) {
			fprintf(stderr, "newgrp: %s: %s\n", account->pw_dir,
				strerror(errno));

			/* Reports operation failure. */
			return 1;
		}
		(void)clearenv();
		(void)setenv("HOME", account->pw_dir, 1);
		(void)setenv("USER", account->pw_name, 1);
		(void)setenv("LOGNAME", account->pw_name, 1);
		(void)setenv("SHELL", shell, 1);
		(void)setenv("PATH", "/bin:/usr/bin", 1);
	}
	(void)snprintf(
	    login_name, sizeof(login_name), "%s%s", login_shell ? "-" : "",
	    strrchr(shell, '/') != NULL ? strrchr(shell, '/') + 1 : shell);
	shell_argv[0] = login_name;
	shell_argv[1] = NULL;
	execve(shell, shell_argv, environ);
	fprintf(stderr, "newgrp: %s: %s\n", shell, strerror(errno));

	/* Returns the computed result. */
	return 126;
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(stderr, "usage: newgrp [-l | -] [group]\n");
}

/* Supports the authorized operation. */
static int
authorized(
	const struct group *group,
	const struct passwd *account)
{
	char password[256];
	char *encoded;
	int result;

	/* Handles a failed getuid operation. */
	if (getuid() == 0 || member_of(group, account))
		return 1;

	/* Handles the gr passwd availability. */
	if (group->gr_passwd == NULL || group->gr_passwd[0] == '\0' ||
	    group->gr_passwd[0] == 'x' || group->gr_passwd[0] == '*' ||
	    group->gr_passwd[0] == '!')

		/* Reports successful completion. */
		return 0;

	/* Handles a failed read password operation. */
	if (read_password(password, sizeof(password)) != 0)
		return 0;
	encoded = crypt(password, group->gr_passwd);
	result = encoded != NULL && strcmp(encoded, group->gr_passwd) == 0;
	memset(password, 0, sizeof(password));

	/* Returns the computed result. */
	return result;
}

/* Supports the member of operation. */
static int
member_of(
	const struct group *group,
	const struct passwd *account)
{
	char **member;

	/* Handles the account condition. */
	if (account->pw_gid == group->gr_gid)
		return 1;

	/* Process each element required by the operation. */
	for (member = group->gr_mem; member != NULL && *member != NULL;
	     member++) {
		/* Selects the matching value. */
		if (strcmp(*member, account->pw_name) == 0)
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the read password operation. */
static int
read_password(
	char *buffer,
	size_t capacity)
{
	struct termios saved, hidden;
	ssize_t count;
	size_t used;
	int descriptor;
	int changed;

	used = 0;
	descriptor = open("/dev/tty", O_RDWR);
	changed = 0;

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Handles a failed tcgetattr operation. */
	if (tcgetattr(descriptor, &saved) == 0) {
		hidden = saved;
		hidden.c_lflag &= ~ECHO;
		changed = tcsetattr(descriptor, TCSAFLUSH, &hidden) == 0;
	}
	(void)write(descriptor, "Password: ", 10);

	/* Continue while the operation condition remains true. */
	while (used + 1U < capacity) {
		count = read(descriptor, buffer + used, 1);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count != 1 || buffer[used] == '\n' || buffer[used] == '\r')
			break;
		used++;
	}
	buffer[used] = '\0';

	/* Handles the changed condition. */
	if (changed)
		(void)tcsetattr(descriptor, TCSAFLUSH, &saved);
	(void)write(descriptor, "\n", 1);
	(void)close(descriptor);

	/* Returns the computed result. */
	return count < 0 ? -1 : 0;
}
