/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * sudo: runs a command as root (or another user) for a member of the wheel
 * group, with the member's own password (ws160-p001).
 *
 *   sudo [-S] [-u user] command [argument ...]
 *   sudo [-S] [-u user] -s      the target's shell
 *   sudo [-S] [-u user] -i      the target's login shell, in its home
 *
 * The rule is the one of this release, without a sudoers file: root, and
 * the members of the group wheel (by name, as their primary group or named
 * in it), may run any command as any user; nobody else may.  A member
 * gives the member's own password, from the terminal, or from standard
 * input with -S; three tries on the terminal.  Nothing is remembered: each
 * run asks again.  A refusal costs two seconds; every run and refusal
 * goes to the system log (auth) with the terminal, the user and the
 * command.
 *
 * The command runs with the target's groups, group and user IDs, and an
 * environment made anew (account.c): the terminal's and the locale's
 * variables of the caller, the target's HOME, SHELL, USER and LOGNAME, the
 * secure PATH, and SUDO_USER, SUDO_UID, SUDO_GID and SUDO_COMMAND.  A
 * command without a slash is looked for in the secure PATH, never in the
 * caller's.
 *
 * The program is set-user-ID root.  The exit status is the command's, or 1
 * when sudo refuses or fails, 2 for a wrong use.
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
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

/* The exit statuses of sudo itself. */
#define SUDO_FAILED		1
#define SUDO_USAGE		2

/* The tries on the terminal, and the pause after a refusal (seconds). */
#define SUDO_TRIES		3
#define SUDO_FAILURE_PAUSE	2U

/* The environment's room, and the logged command's. */
#define SUDO_STORAGE		8192U
#define SUDO_COMMAND_MAX	512U

extern char **environ;

static int authenticate(const char *name, int from_stdin);
static int find_command(const char *command, char *path, size_t size);
static int executable(const char *path);
static void command_text(char **argv, char *text, size_t size);
static void standard_descriptors(void);
static void usage(void);

int
main(
	int argc,
	char **argv)
{
	char storage[SUDO_STORAGE];
	char text[SUDO_COMMAND_MAX];
	char path[1024];
	char shell_name[64];
	char caller_name[64];
	char *variables[ACCOUNT_ENVIRONMENT_MAX];
	char *shell_argv[2];
	struct account_environment_input input;
	struct passwd account;
	struct passwd *caller;
	struct passwd *target;
	const char *target_name;
	const char *shell;
	const char *base;
	const char *terminal;
	char **command_argv;
	size_t count;
	uid_t real;
	uid_t effective;
	gid_t real_group;
	int from_stdin;
	int shell_mode;
	int login_mode;
	int member;
	int option;
	int error;

	/* Descriptors 0 to 2 open, the log, the options (up to the command). */
	standard_descriptors();
	openlog("sudo", LOG_PID, LOG_AUTH);
	from_stdin = 0;
	shell_mode = 0;
	login_mode = 0;
	target_name = "root";
	for (;;) {
		option = getopt(argc, argv, "Su:si");
		if (option == -1)
			break;
		switch (option) {
		case 'S':
			from_stdin = 1;
			break;
		case 'u':
			target_name = optarg;
			break;
		case 's':
			shell_mode = 1;
			break;
		case 'i':
			login_mode = 1;
			break;
		default:
			usage();
			return SUDO_USAGE;
		}
	}

	/* The command, and the forms that are wrong. */
	command_argv = argv + optind;
	if (shell_mode + login_mode > 1 || (optind >= argc && !shell_mode && !login_mode) ||
	    (optind < argc && (shell_mode || login_mode))) {
		usage();
		return SUDO_USAGE;
	}

	/* Installed set-user-ID root, or it can do nothing. */
	effective = geteuid();
	if (effective != 0) {
		fprintf(stderr, "sudo: not installed set-user-ID root\n");
		return SUDO_FAILED;
	}

	/* The caller, by the real user ID, and the terminal for the log. */
	real = getuid();
	real_group = getgid();
	caller = getpwuid(real);
	if (caller == NULL) {
		fprintf(stderr, "sudo: who are you?\n");
		return SUDO_FAILED;
	}

	/* Its name, kept. */
	(void)snprintf(caller_name, sizeof(caller_name), "%s", caller->pw_name);
	terminal = ttyname(STDIN_FILENO);
	if (terminal == NULL)
		terminal = "none";
	text[0] = '\0';
	if (optind < argc)
		command_text(command_argv, text, sizeof(text));

	/* root, or a member of wheel. */
	member = 1;
	if (real != 0)
		member = account_in_wheel(caller_name, caller->pw_gid);
	if (!member) {
		syslog(LOG_NOTICE, "%s : refused (not in wheel) ; TTY=%s ; USER=%s ; COMMAND=%s", caller_name, terminal, target_name, text);
		sleep(SUDO_FAILURE_PAUSE);
		fprintf(stderr, "sudo: %s is not in the wheel group; this is logged\n", caller_name);
		return SUDO_FAILED;
	}

	/* The member's own password, unless root asks. */
	if (real != 0) {
		error = authenticate(caller_name, from_stdin);
		if (error != 0) {
			syslog(LOG_NOTICE, "%s : refused (wrong password) ; TTY=%s ; USER=%s ; COMMAND=%s", caller_name, terminal, target_name, text);
			fprintf(stderr, "sudo: authentication failed\n");
			return SUDO_FAILED;
		}
	}

	/* The target. */
	target = getpwnam(target_name);
	if (target == NULL) {
		fprintf(stderr, "sudo: unknown user %s\n", target_name);
		return SUDO_FAILED;
	}

	/* Its entry, copied; its shell. */
	account = *target;
	shell = account.pw_shell;
	if (shell == NULL || shell[0] == '\0')
		shell = "/bin/sh";

	/* The command's path, from the secure PATH; a shell when asked. */
	if (shell_mode || login_mode) {
		(void)snprintf(path, sizeof(path), "%s", shell);
		(void)snprintf(text, sizeof(text), "%s", shell);
	} else {
		error = find_command(command_argv[0], path, sizeof(path));
		if (error != 0) {
			fprintf(stderr, "sudo: %s: command not found\n", command_argv[0]);
			return SUDO_FAILED;
		}
	}

	/* Logged. */
	syslog(LOG_NOTICE, "%s : TTY=%s ; USER=%s ; COMMAND=%s", caller_name, terminal, target_name, text);

	/* The environment, made anew. */
	memset(&input, 0, sizeof(input));
	input.caller = environ;
	input.home = account.pw_dir;
	input.shell = shell;
	input.user = account.pw_name;
	input.sudo_user = caller_name;
	input.sudo_uid = real;
	input.sudo_gid = real_group;
	input.sudo_command = text;
	count = account_environment(&input, storage, sizeof(storage), variables, ACCOUNT_ENVIRONMENT_MAX);
	if (count == 0U) {
		fprintf(stderr, "sudo: the environment does not fit\n");
		return SUDO_FAILED;
	}

	/* The target's groups, group and user. */
	error = initgroups(account.pw_name, account.pw_gid);
	if (error == 0)
		error = setgid(account.pw_gid);
	if (error == 0)
		error = setuid(account.pw_uid);
	if (error != 0) {
		fprintf(stderr, "sudo: cannot become %s: %s\n", target_name, strerror(errno));
		return SUDO_FAILED;
	}

	/* A shell: "-sh" in the home for a login shell. */
	if (shell_mode || login_mode) {
		base = strrchr(shell, '/');
		if (base == NULL)
			base = shell;
		else
			base++;
		(void)snprintf(shell_name, sizeof(shell_name), "%s", base);
		if (login_mode) {
			(void)snprintf(shell_name, sizeof(shell_name), "-%s", base);
			error = chdir(account.pw_dir);
			if (error != 0)
				(void)chdir("/");
		}

		/* Its arguments. */
		shell_argv[0] = shell_name;
		shell_argv[1] = NULL;
		execve(path, shell_argv, variables);
	} else {
		execve(path, command_argv, variables);
	}

	/* Not reached unless the command could not run. */
	fprintf(stderr, "sudo: %s: %s\n", path, strerror(errno));
	return SUDO_FAILED;
}

/*
 * Asks for and checks the member's own password: three tries on the
 * terminal, one from standard input.  Returns 0 when it matched.
 */
static int
authenticate(
	const char *name,
	int from_stdin)
{
	char password[ACCOUNT_PASSWORD_MAX + 2U];
	char buffer[LOGIN_VERIFY_BUFFER];
	char prompt[96];
	struct passwd account;
	char *read;
	int tries;
	int flags;
	int index;
	int error;

	/* The prompt, and where the password is read from. */
	(void)snprintf(prompt, sizeof(prompt), "[sudo] password for %s: ", name);
	tries = SUDO_TRIES;
	flags = RPP_ECHO_OFF | RPP_REQUIRE_TTY;
	if (from_stdin) {
		tries = 1;
		flags = RPP_ECHO_OFF | RPP_STDIN;
	}

	/* Each try; login_verify erases the password whatever it answers. */
	for (index = 0; index < tries; index++) {
		read = readpassphrase(prompt, password, sizeof(password), flags);
		if (read == NULL)
			return -1;
		error = login_verify(name, password, &account, buffer, sizeof(buffer));
		memset(buffer, 0, sizeof(buffer));
		if (error == 0)
			return 0;

		/* A wrong one: a pause, and the next try. */
		sleep(SUDO_FAILURE_PAUSE);
		if (index + 1 < tries)
			fprintf(stderr, "Sorry, try again.\n");
	}

	/* No try matched. */
	return -1;
}

/*
 * Finds a command: as given when it has a slash, otherwise in the secure
 * PATH.  Returns 0 with its path, or -1.
 */
static int
find_command(
	const char *command,
	char *path,
	size_t size)
{
	const char *directory;
	const char *end;
	const char *slash;
	size_t length;
	int usable;

	/* A path is taken as it is, when it is an executable file. */
	slash = strchr(command, '/');
	if (slash != NULL) {
		usable = executable(command);
		if (!usable)
			return -1;
		(void)snprintf(path, size, "%s", command);
		return 0;
	}

	/* Each directory of the secure PATH. */
	directory = ACCOUNT_SECURE_PATH;
	while (*directory != '\0') {
		/* The directory, up to the next colon. */
		end = strchr(directory, ':');
		if (end == NULL)
			end = directory + strlen(directory);
		length = (size_t)(end - directory);
		(void)snprintf(path, size, "%.*s/%s", (int)length, directory, command);

		/* An executable file there. */
		usable = executable(path);
		if (usable)
			return 0;

		/* The next one. */
		directory = end;
		if (*directory == ':')
			directory++;
	}

	/* Not found. */
	return -1;
}

/* Tells whether a path is a regular file someone may execute. */
static int
executable(
	const char *path)
{
	struct stat status;
	int error;

	/* Its status. */
	error = stat(path, &status);
	if (error != 0)
		return 0;

	/* A regular file with an execute bit. */
	if ((status.st_mode & S_IFMT) != S_IFREG)
		return 0;
	if ((status.st_mode & 0111) == 0)
		return 0;

	/* Succeeded: it may run. */
	return 1;
}

/* Writes the command and its arguments as one line for the log and SUDO_COMMAND. */
static void
command_text(
	char **argv,
	char *text,
	size_t size)
{
	size_t used;
	size_t index;
	int put;

	/* Each word, a space between. */
	used = 0;
	text[0] = '\0';
	for (index = 0; argv[index] != NULL && used < size; index++) {
		if (index == 0U) {
			put = snprintf(text + used, size - used, "%s", argv[index]);
		} else {
			put = snprintf(text + used, size - used, " %s", argv[index]);
		}

		/* Stops when nothing more fits. */
		if (put < 0)
			break;
		used += (size_t)put;
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
			_exit(SUDO_FAILED);
	}
}

/* Says how sudo is used. */
static void
usage(void)
{
	/* On standard error. */
	fprintf(stderr, "usage: sudo [-S] [-u user] command [argument ...]\n"
		"       sudo [-S] [-u user] -s | -i\n");
}
