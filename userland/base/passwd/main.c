/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * passwd: changes a password (ws160-p001).
 *
 *   passwd [user]       asks on the terminal: the current password (unless
 *                       root runs it), the new one twice
 *   passwd -s [user]    reads the passwords from standard input, one a line,
 *                       without prompts: the current and the new one for
 *                       the caller's own password (root's current one is
 *                       read but not checked), only the new one when root
 *                       sets another user's; for programs
 *                       such as Settings (ws160-p002), which run passwd
 *                       instead of holding any privilege themselves
 *
 * Only root names another user.  The program is set-user-ID root; it uses
 * its privilege only to read and replace /etc/shadow (account.c), and the
 * account it changes is the caller's own (by the real user ID) unless the
 * caller is root.  A wrong current password costs two seconds.  Every
 * change and every refusal goes to the system log (auth).
 *
 * The exit status: 0 changed, 1 failed, 2 usage, 3 the current password
 * is wrong, 4 the new password is refused (too short, and so on), 5 the
 * two new passwords differ.
 */

#include "userland/base/common/account.h"
#include "userland/base/login/verify.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <readpassphrase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

/* The exit statuses (account.h, which programs that run passwd -s read). */
#define PASSWD_OK		ACCOUNT_PASSWD_OK
#define PASSWD_FAILED		ACCOUNT_PASSWD_FAILED
#define PASSWD_USAGE		ACCOUNT_PASSWD_USAGE
#define PASSWD_WRONG		ACCOUNT_PASSWD_WRONG
#define PASSWD_REFUSED		ACCOUNT_PASSWD_REFUSED
#define PASSWD_MISMATCH		ACCOUNT_PASSWD_MISMATCH

/* The pause after a wrong current password (seconds). */
#define PASSWD_WRONG_PAUSE	2U

static int read_secret(int batch, const char *prompt, char *buffer, size_t size);
static void standard_descriptors(void);
static void wipe(char *buffer, size_t size);

int
main(
	int argc,
	char **argv)
{
	char current[ACCOUNT_PASSWORD_MAX + 2U];
	char fresh[ACCOUNT_PASSWORD_MAX + 2U];
	char again[ACCOUNT_PASSWORD_MAX + 2U];
	char hash[ACCOUNT_HASH_MAX];
	char buffer[LOGIN_VERIFY_BUFFER];
	char caller_name[64];
	struct passwd account;
	struct passwd *caller;
	struct passwd *target;
	const char *name;
	uid_t real;
	uid_t effective;
	int status;
	int by_root;
	int batch;
	int own;
	int reason;
	int option;
	int error;
	int same;

	/* Descriptors 0 to 2 open, the log, the options. */
	standard_descriptors();
	openlog("passwd", LOG_PID, LOG_AUTH);
	batch = 0;
	for (;;) {
		option = getopt(argc, argv, "s");
		if (option == -1)
			break;
		if (option != 's') {
			fprintf(stderr, "usage: passwd [-s] [user]\n");
			return PASSWD_USAGE;
		}

		/* Batch. */
		batch = 1;
	}

	/* At most one user. */
	if (argc - optind > 1) {
		fprintf(stderr, "usage: passwd [-s] [user]\n");
		return PASSWD_USAGE;
	}

	/* Installed set-user-ID root, or it can do nothing. */
	effective = geteuid();
	if (effective != 0) {
		fprintf(stderr, "passwd: not installed set-user-ID root\n");
		return PASSWD_FAILED;
	}

	/* The caller, by the real user ID. */
	real = getuid();
	caller = getpwuid(real);
	if (caller == NULL) {
		fprintf(stderr, "passwd: who are you?\n");
		return PASSWD_FAILED;
	}

	/* Its name, kept. */
	(void)snprintf(caller_name, sizeof(caller_name), "%s", caller->pw_name);
	by_root = 0;
	if (real == 0)
		by_root = 1;

	/* The account: the caller's own, or one root names. */
	name = caller_name;
	if (optind < argc)
		name = argv[optind];
	same = strcmp(name, caller_name);
	own = 0;
	if (same == 0)
		own = 1;
	if (same != 0 && !by_root) {
		syslog(LOG_NOTICE, "refused: %s tried to change the password of %s", caller_name, name);
		fprintf(stderr, "passwd: only root may change another user's password\n");
		return PASSWD_FAILED;
	}

	/* The account must exist. */
	target = getpwnam(name);
	if (target == NULL) {
		fprintf(stderr, "passwd: unknown user %s\n", name);
		return PASSWD_FAILED;
	}

	/* Said on the terminal. */
	if (!batch)
		printf("Changing the password of %s.\n", name);

	/* The current password, unless root sets it. */
	current[0] = '\0';
	if (!by_root || (batch && own)) {
		error = read_secret(batch, "Current password: ", current, sizeof(current));
		if (error != 0) {
			fprintf(stderr, "passwd: no current password\n");
			return PASSWD_FAILED;
		}

		/* login_verify erases what it is given: a copy is checked, the original kept for the rules (root's own is not checked). */
		error = 0;
		if (!by_root) {
			memcpy(again, current, sizeof(again));
			error = login_verify(name, again, &account, buffer, sizeof(buffer));
			wipe(again, sizeof(again));
		}

		/* A wrong one costs two seconds. */
		if (error != 0) {
			syslog(LOG_NOTICE, "refused: a wrong current password for %s", name);
			wipe(current, sizeof(current));
			sleep(PASSWD_WRONG_PAUSE);
			fprintf(stderr, "passwd: the current password is wrong\n");
			return PASSWD_WRONG;
		}
	}

	/* The new one, twice on the terminal. */
	error = read_secret(batch, "New password: ", fresh, sizeof(fresh));
	if (error == 0 && !batch)
		error = read_secret(batch, "Retype the new password: ", again, sizeof(again));
	if (error == 0 && batch)
		memcpy(again, fresh, sizeof(again));
	if (error != 0) {
		wipe(current, sizeof(current));
		wipe(fresh, sizeof(fresh));
		fprintf(stderr, "passwd: no new password\n");
		return PASSWD_FAILED;
	}

	/* The two must be the same. */
	same = strcmp(fresh, again);
	wipe(again, sizeof(again));
	if (same != 0) {
		wipe(current, sizeof(current));
		wipe(fresh, sizeof(fresh));
		fprintf(stderr, "passwd: the two new passwords differ\n");
		return PASSWD_MISMATCH;
	}

	/* The rules. */
	if (by_root) {
		reason = account_password_check(fresh, NULL, 1);
	} else {
		reason = account_password_check(fresh, current, 0);
	}

	/* The current one is no longer needed. */
	wipe(current, sizeof(current));
	if (reason != ACCOUNT_PASSWORD_OK) {
		wipe(fresh, sizeof(fresh));
		fprintf(stderr, "passwd: %s\n", account_password_reason(reason));
		return PASSWD_REFUSED;
	}

	/* The hash, into /etc/shadow. */
	error = account_password_hash(fresh, hash, sizeof(hash));
	wipe(fresh, sizeof(fresh));
	if (error == 0)
		error = account_shadow_set(name, hash);
	wipe(hash, sizeof(hash));
	if (error != 0) {
		syslog(LOG_ERR, "failed to change the password of %s: %s", name, strerror(error));
		fprintf(stderr, "passwd: the password could not be changed: %s\n", strerror(error));
		return PASSWD_FAILED;
	}

	/* Succeeded: said and logged. */
	syslog(LOG_NOTICE, "the password of %s was changed by %s", name, caller_name);
	if (!batch)
		printf("passwd: the password of %s is changed.\n", name);
	status = PASSWD_OK;
	return status;
}

/*
 * Reads one password: from the terminal with a prompt and without echo,
 * or (batch) one line of standard input.  Returns 0, or -1 when nothing
 * could be read.
 */
static int
read_secret(
	int batch,
	const char *prompt,
	char *buffer,
	size_t size)
{
	char *read;
	size_t length;
	int flags;

	/* The terminal, or standard input in a batch. */
	flags = RPP_ECHO_OFF | RPP_REQUIRE_TTY;
	if (batch)
		flags = RPP_ECHO_OFF | RPP_STDIN;
	read = readpassphrase(prompt, buffer, size, flags);
	if (read == NULL)
		return -1;

	/* A line that filled the buffer was too long for any password. */
	length = strlen(buffer);
	if (length + 1U >= size) {
		wipe(buffer, size);
		return -1;
	}

	/* Succeeded: the password. */
	return 0;
}

/* Opens /dev/null on any of descriptors 0 to 2 that is closed, so that nothing written later lands in a file opened there. */
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
			_exit(PASSWD_FAILED);
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
