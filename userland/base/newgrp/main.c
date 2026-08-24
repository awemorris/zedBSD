/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static int
member_of(const struct group *group, const struct passwd *account)
{
	char **member;

	if (account->pw_gid == group->gr_gid)
		return 1;
	for (member = group->gr_mem; member != NULL && *member != NULL;
	     member++)
		if (strcmp(*member, account->pw_name) == 0)
			return 1;
	return 0;
}

static int
read_password(char *buffer, size_t capacity)
{
	struct termios saved, hidden;
	ssize_t count;
	size_t used = 0;
	int descriptor = open("/dev/tty", O_RDWR);
	int changed = 0;

	if (descriptor < 0)
		return -1;
	if (tcgetattr(descriptor, &saved) == 0) {
		hidden = saved;
		hidden.c_lflag &= ~ECHO;
		changed = tcsetattr(descriptor, TCSAFLUSH, &hidden) == 0;
	}
	(void)write(descriptor, "Password: ", 10);
	while (used + 1U < capacity) {
		count = read(descriptor, buffer + used, 1);
		if (count < 0 && errno == EINTR)
			continue;
		if (count != 1 || buffer[used] == '\n' || buffer[used] == '\r')
			break;
		used++;
	}
	buffer[used] = '\0';
	if (changed)
		(void)tcsetattr(descriptor, TCSAFLUSH, &saved);
	(void)write(descriptor, "\n", 1);
	(void)close(descriptor);
	return count < 0 ? -1 : 0;
}

static int
authorized(const struct group *group, const struct passwd *account)
{
	char password[256];
	char *encoded;
	int result;

	if (getuid() == 0 || member_of(group, account))
		return 1;
	if (group->gr_passwd == NULL || group->gr_passwd[0] == '\0' ||
	    group->gr_passwd[0] == 'x' || group->gr_passwd[0] == '*' ||
	    group->gr_passwd[0] == '!')
		return 0;
	if (read_password(password, sizeof(password)) != 0)
		return 0;
	encoded = crypt(password, group->gr_passwd);
	result = encoded != NULL && strcmp(encoded, group->gr_passwd) == 0;
	memset(password, 0, sizeof(password));
	return result;
}

static void
usage(void)
{
	fprintf(stderr, "usage: newgrp [-l | -] [group]\n");
}

int
main(int argc, char **argv)
{
	struct passwd *account;
	struct group *group;
	struct passwd account_storage;
	struct group group_storage;
	char account_buffer[2048];
	char group_buffer[2048];
	const char *group_name = NULL;
	const char *shell;
	char login_name[128];
	char *shell_argv[2];
	int login_shell = 0;

	if (argc > 1 && (!strcmp(argv[1], "-l") || !strcmp(argv[1], "-"))) {
		login_shell = 1;
		argv++;
		argc--;
	}
	if (argc > 2) {
		usage();
		return 2;
	}
	if (argc == 2)
		group_name = argv[1];
	if (getpwuid_r(getuid(), &account_storage, account_buffer,
		       sizeof(account_buffer), &account) != 0 ||
	    account == NULL) {
		fprintf(stderr, "newgrp: cannot identify uid %u\n",
			(unsigned)getuid());
		return 1;
	}
	if ((group_name != NULL
		 ? getgrnam_r(group_name, &group_storage, group_buffer,
			      sizeof(group_buffer), &group)
		 : getgrgid_r(account->pw_gid, &group_storage, group_buffer,
			      sizeof(group_buffer), &group)) != 0 ||
	    group == NULL) {
		fprintf(stderr, "newgrp: unknown group: %s\n",
			group_name != NULL ? group_name : "(primary)");
		return 1;
	}
	if (!authorized(group, account)) {
		fprintf(stderr, "newgrp: permission denied for group %s\n",
			group->gr_name);
		return 1;
	}
	if (initgroups(account->pw_name, group->gr_gid) != 0 ||
	    setgid(group->gr_gid) != 0) {
		fprintf(stderr, "newgrp: credentials: %s\n", strerror(errno));
		return 1;
	}
	shell = account->pw_shell != NULL && account->pw_shell[0] != '\0'
		    ? account->pw_shell
		    : "/bin/sh";
	if (login_shell) {
		if (chdir(account->pw_dir) != 0) {
			fprintf(stderr, "newgrp: %s: %s\n", account->pw_dir,
				strerror(errno));
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
	return 126;
}
