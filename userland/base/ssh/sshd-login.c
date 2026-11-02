/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The account checks of sshd: who may log in, with which key or password,
 * and the login records.  The monitor calls these as root; nothing here
 * is reached by the unprivileged processes except through the monitor's
 * requests.
 *
 * The authorized_keys file is read by a child the monitor forks, which
 * takes the account's ids before it opens anything, because the path is
 * the user's to choose and root must not follow it.  The child passes the
 * bytes back over a pipe and exits; it parses nothing.  With StrictModes
 * it opens the path one directory at a time from the home directory (or
 * from the root when the path is elsewhere), each below the first with
 * O_NOFOLLOW, and checks each directory and the file with fstat on the
 * descriptor it opened, so that what was checked is what is read.
 */

#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>

#include "userland/base/login/verify.h"

#include "ssh-hostfile.h"
#include "sshd-monitor.h"

/*
 * The shadow hash crypt runs on for an account that does not exist, is
 * locked, or cannot use a password here: SHA-512 crypt ("$6$", the kind
 * the system's accounts are given), a fixed salt of 16 characters and a
 * hash part of 86 characters, so the work is that of a real account.  No
 * password is ever compared with it.
 */
#define LOGIN_DUMMY_HASH	"$6$zedbsdsshddummy0$" \
	"dHm47Ai5P29rp6S9q5DQ5m4Q3FZpGDbLBMjA65OzqcvuiaTLT8azftY7DpJfHyp37cfgzu69Ww65btYlg0vhJC"

/* The shells that say an account may not log in. */
#define LOGIN_NOLOGIN		"/sbin/nologin"
#define LOGIN_NOLOGIN_USR	"/usr/sbin/nologin"

/* The path AuthorizedKeysFile none stands for: no file, no key. */
#define LOGIN_NO_FILE		"/dev/null"

/* The part of a terminal's path that its record's line leaves out. */
#define LOGIN_DEV_PREFIX	"/dev/"

/* The longest name of one directory or file in an authorized_keys path, with its NUL. */
#define LOGIN_NAME_MAX		256U

/* The bytes the child that reads an authorized_keys file copies at a time. */
#define LOGIN_COPY_CHUNK	4096U

/* The mode bits that would let others than the owner change a file or a directory. */
#define LOGIN_UNSAFE_MODE	(S_IWGRP | S_IWOTH)

/*
 * How the child that reads an authorized_keys file ended (its exit
 * status): the whole file followed on the pipe, or why not.
 */
#define LOGIN_KEYS_READ		0
#define LOGIN_KEYS_UNSAFE	1
#define LOGIN_KEYS_ABSENT	2
#define LOGIN_KEYS_FAILED	3
#define LOGIN_KEYS_TOO_LARGE	4
#define LOGIN_KEYS_NOT_USER	5
#define LOGIN_KEYS_IDS		6

static void login_dummy_crypt(char *password);
static int login_lookup(const char *user, struct passwd *account, char *buffer, size_t size, char *hash, size_t hash_size, int *have_hash);
static int login_tty_number(const char *tty, const char **number);
static int login_append(char *out, size_t capacity, size_t *used, const char *text, size_t size);
static int login_keys_verdict(const struct passwd *account, int status, size_t size);
static void login_keys_child(const struct sshd_config *config, const struct passwd *account, const char *path, int output);
static int login_keys_take_ids(const struct passwd *account);
static int login_keys_open(const struct sshd_config *config, const struct passwd *account, const char *path, int *descriptor);
static int login_keys_open_strict(const char *path, const char *home, uid_t uid, int *descriptor);
static int login_keys_safe(int descriptor, uid_t uid, int directory);
static int login_keys_copy(int input, int output);
static int login_keys_collect(pid_t child, int input, char *text, size_t capacity, size_t *size, int *status);
static long login_elapsed_ms(const struct timespec *start);
static int login_write_all(int descriptor, const char *data, size_t size);
static pid_t login_wait(pid_t child, int *status);

/*
 * Checks that user may log in at all with method (SSH_AUTH_PUBLICKEY or
 * SSH_AUTH_PASSWORD of ssh-auth.h): the account exists (getpwnam_r into
 * account, its strings in buffer of size bytes), is not locked, has a
 * shell other than /sbin/nologin, and is root only as PermitRootLogin
 * allows for the method.
 *
 * As root the shadow entry is read for the lock; without root it is not
 * readable, and only the account sshd runs as is served.
 */
int
sshd_account_allowed(
	const struct sshd_config *config,
	const char *user,
	int method,
	struct passwd *account,
	char *buffer,
	size_t size)
{
	char hash[LOGIN_VERIFY_BUFFER];
	const char *shadow_hash;
	uid_t self;
	int have_hash;
	int privileged;
	int found;
	int allowed;

	/* Refuses a call without its arguments as a refusal, never as a grant. */
	if (config == NULL || user == NULL || account == NULL || buffer == NULL)
		return EACCES;

	/* Learns whether sshd runs as root. */
	self = geteuid();
	privileged = 0;
	if (self == 0)
		privileged = 1;

	/* Finds the account in passwd, and in shadow when sshd can read it. */
	found = login_lookup(user, account, buffer, size, hash, sizeof(hash), &have_hash);
	if (found != 0) {
		syslog(LOG_AUTH | LOG_NOTICE, "invalid user %.64s", user);
		return ENOENT;
	}

	/* Without root, the shadow entry is not consulted. */
	shadow_hash = NULL;
	if (privileged && have_hash)
		shadow_hash = hash;

	/* Applies the rules to what the files hold, then forgets the hash. */
	allowed = sshd_account_policy(config, account, shadow_hash, method, privileged, self);
	ssh_wipe(hash, sizeof(hash));
	if (allowed != 0) {
		syslog(LOG_AUTH | LOG_NOTICE, "user %.64s not allowed", user);
		return EACCES;
	}

	/* Succeeded: the account may log in with the method. */
	return 0;
}

/*
 * Decides whether an account may log in with method, given what passwd
 * and shadow hold.
 */
int
sshd_account_policy(
	const struct sshd_config *config,
	const struct passwd *account,
	const char *shadow_hash,
	int method,
	int privileged,
	uid_t self)
{
	int same;

	/* Refuses a call without its arguments. */
	if (config == NULL || account == NULL)
		return EACCES;

	/* Only the two methods that log in are considered at all. */
	if (method != SSH_AUTH_PUBLICKEY && method != SSH_AUTH_PASSWORD)
		return EACCES;

	if (privileged) {
		/* As root, an account without a shadow entry cannot log in, as at the console. */
		if (shadow_hash == NULL)
			return EACCES;

		/* A locked account ("!" or "*" in front of its hash) logs in with nothing, not even a key. */
		if (shadow_hash[0] == '!' || shadow_hash[0] == '*')
			return EACCES;
	} else {
		/* Without root, sshd can serve only the account it runs as. */
		if (account->pw_uid != self)
			return EACCES;
	}

	/* An account whose shell is nologin has no session to start. */
	if (account->pw_shell != NULL) {
		same = strcmp(account->pw_shell, LOGIN_NOLOGIN);
		if (same == 0)
			return EACCES;
		same = strcmp(account->pw_shell, LOGIN_NOLOGIN_USR);
		if (same == 0)
			return EACCES;
	}

	/* root logs in only as PermitRootLogin allows the method. */
	if (account->pw_uid == 0) {
		if (config->permit_root == SSHD_PERMIT_ROOT_YES)
			return 0;
		if (config->permit_root == SSHD_PERMIT_ROOT_PROHIBIT_PASSWORD && method == SSH_AUTH_PUBLICKEY)
			return 0;
		return EACCES;
	}

	/* Succeeded: the account may log in with the method. */
	return 0;
}

/*
 * Reports whether public_key is in the account's authorized_keys file
 * (AuthorizedKeysFile, expanded): a child with the account's ids reads
 * the file as described above and passes at most SSH_HOSTFILE_MAX bytes
 * back over a pipe, and the monitor looks for the key with
 * ssh_authorized_keys_find.
 *
 * A child that does not finish within SSHD_KEYS_TIMEOUT_MS is killed
 * (the user can stop a process of their own), and the key is refused.
 */
int
sshd_key_authorized(
	const struct sshd_config *config,
	const struct passwd *account,
	const uint8_t *public_key)
{
	char path[SSHD_KEYS_PATH_MAX];
	int pipe_ends[2];
	char *text;
	size_t size;
	pid_t child;
	int status;
	int expanded;
	int same;
	int opened;
	int collected;
	int verdict;
	int lookup;
	int found;
	unsigned skipped;

	/* Refuses a call without its arguments. */
	if (config == NULL || account == NULL || public_key == NULL)
		return EACCES;

	/* Expands the configured path for the account. */
	expanded = sshd_authorized_path(config->authorized_keys_file, account, path, sizeof(path));
	if (expanded != 0) {
		syslog(LOG_AUTH | LOG_NOTICE, "authorized keys path of %.64s cannot be expanded", account->pw_name);
		return EACCES;
	}

	/* AuthorizedKeysFile none: no key is authorized, and nothing is opened. */
	same = strcmp(path, LOGIN_NO_FILE);
	if (same == 0)
		return EACCES;

	/* Makes the room for the file's bytes, and one more to see a file that is too large. */
	text = malloc((size_t)SSH_HOSTFILE_MAX + 1U);
	if (text == NULL)
		return EIO;

	/* Opens the pipe the child passes the bytes over; neither end reaches a program started later. */
	opened = pipe(pipe_ends);
	if (opened != 0) {
		free(text);
		return EIO;
	}
	(void)fcntl(pipe_ends[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(pipe_ends[1], F_SETFD, FD_CLOEXEC);

	/* Starts the child that reads the file with the account's ids. */
	child = fork();
	if (child < 0) {
		(void)close(pipe_ends[0]);
		(void)close(pipe_ends[1]);
		free(text);
		return EIO;
	}
	if (child == 0) {
		(void)close(pipe_ends[0]);
		login_keys_child(config, account, path, pipe_ends[1]);
	}

	/* Takes the bytes and the child's end. */
	(void)close(pipe_ends[1]);
	collected = login_keys_collect(child, pipe_ends[0], text, (size_t)SSH_HOSTFILE_MAX + 1U, &size, &status);
	(void)close(pipe_ends[0]);
	if (collected != 0) {
		free(text);
		syslog(LOG_AUTH | LOG_NOTICE, "authorized keys of %.64s: the reader did not finish", account->pw_name);
		return EACCES;
	}

	/* Refuses every way the child can end other than with the whole file. */
	verdict = login_keys_verdict(account, status, size);
	if (verdict != 0) {
		free(text);
		return verdict;
	}

	/* Looks for the key among the file's lines. */
	found = 0;
	skipped = 0;
	lookup = ssh_authorized_keys_find(text, size, public_key, &found, &skipped);
	free(text);
	if (lookup != 0)
		return EACCES;

	/* Refuses a key the file does not list. */
	if (!found)
		return EACCES;

	/* Succeeded: the file authorizes the key. */
	return 0;
}

/*
 * Checks a password with the console login's rule (login_verify),
 * refusing an empty one unless PermitEmptyPasswords; for an account that
 * does not exist or is locked, crypt still runs on a dummy hash.
 *
 * Whenever password authentication is on, crypt runs once (in
 * login_verify, or on LOGIN_DUMMY_HASH), whether the account exists, is
 * locked, the password is empty, or sshd runs without root (which serves
 * no password at all); the only exception is an account whose stored
 * password is empty, where there is nothing to hash.
 */
int
sshd_password_ok(
	const struct sshd_config *config,
	const char *user,
	char *password)
{
	struct passwd account;
	char buffer[SSHD_ACCOUNT_BUFFER];
	char hash[LOGIN_VERIFY_BUFFER];
	size_t length;
	uid_t self;
	int have_hash;
	int found;
	int verified;

	/* Refuses a call without a password to check. */
	if (password == NULL)
		return EACCES;

	/* Refuses every password when password authentication is off (this depends on no account). */
	if (config == NULL || user == NULL || !config->password) {
		length = strlen(password);
		ssh_wipe(password, length);
		return EACCES;
	}

	/* Refuses an empty password unless PermitEmptyPasswords, after the same work as any other. */
	if (password[0] == '\0' && !config->permit_empty) {
		login_dummy_crypt(password);
		return EACCES;
	}

	/* Without root there is no shadow to check against: the work is done, the password refused. */
	self = geteuid();
	if (self != 0) {
		login_dummy_crypt(password);
		return EACCES;
	}

	/* Finds whether the account exists and is unlocked; one that is not gets the same work. */
	found = login_lookup(user, &account, buffer, sizeof(buffer), hash, sizeof(hash), &have_hash);
	if (found != 0 ||
	    !have_hash ||
	    hash[0] == '!' ||
	    hash[0] == '*') {
		ssh_wipe(hash, sizeof(hash));
		login_dummy_crypt(password);
		return EACCES;
	}
	ssh_wipe(hash, sizeof(hash));

	/* Checks the password as the console login does, which erases it. */
	verified = login_verify(user, password, &account, buffer, sizeof(buffer));
	length = strlen(password);
	ssh_wipe(password, length);
	if (verified != 0)
		return EACCES;

	/* Succeeded: the password is the account's. */
	return 0;
}

/*
 * Writes the login record (login 1: USER_PROCESS) or the logout record
 * (login 0: DEAD_PROCESS) of a session on tty (a /dev/pts path) for pid,
 * with pututxline as login does.
 *
 * The line is the path below /dev ("pts/3") and the id the number of the
 * terminal, so that a pseudo-terminal's record never takes the place of
 * a console's (whose id is its device name).  The logout record keeps
 * the line and the id and empties the user, as login writes it.
 */
int
sshd_login_record(
	const char *user,
	const char *tty,
	pid_t pid,
	int login)
{
	struct utmpx record;
	struct timeval now;
	struct utmpx *written;
	const char *number;
	int shaped;

	/* Refuses a missing user and a path that is not a pseudo-terminal. */
	if (user == NULL)
		return EIO;
	shaped = login_tty_number(tty, &number);
	if (shaped != 0)
		return EIO;

	/* Fills the record: its kind, its process, its terminal and, for a login, its user. */
	memset(&record, 0, sizeof(record));
	record.ut_pid = pid;
	if (login) {
		record.ut_type = USER_PROCESS;
		strncpy(record.ut_user, user, sizeof(record.ut_user) - 1U);
	} else {
		record.ut_type = DEAD_PROCESS;
	}
	strncpy(record.ut_line, tty + sizeof(LOGIN_DEV_PREFIX) - 1U, sizeof(record.ut_line) - 1U);
	strncpy(record.ut_id, number, sizeof(record.ut_id));

	/* Stamps the record with the time of day. */
	(void)gettimeofday(&now, NULL);
	record.ut_tv.tv_sec = now.tv_sec;
	record.ut_tv.tv_usec = now.tv_usec;

	/* Writes the record in the place of the terminal's earlier one. */
	written = pututxline(&record);
	if (written == NULL)
		return EIO;

	/* Succeeded: the record is written. */
	return 0;
}

/*
 * Expands the AuthorizedKeysFile pattern for account into out (capacity
 * bytes with the NUL).
 */
int
sshd_authorized_path(
	const char *pattern,
	const struct passwd *account,
	char *out,
	size_t capacity)
{
	size_t used;
	size_t index;
	size_t home_size;
	size_t name_size;
	char next;
	int added;

	/* Refuses a call without its arguments, and a home directory that is not absolute. */
	if (pattern == NULL || account == NULL || out == NULL || capacity == 0)
		return EINVAL;
	out[0] = '\0';
	if (account->pw_dir == NULL || account->pw_dir[0] != '/' || account->pw_name == NULL)
		return EINVAL;
	home_size = strlen(account->pw_dir);
	name_size = strlen(account->pw_name);

	/* A path that is not absolute after its expansion starts at the home directory (only %h at the front makes one absolute). */
	used = 0;
	if (pattern[0] != '/' && !(pattern[0] == '%' && pattern[1] == 'h')) {
		added = login_append(out, capacity, &used, account->pw_dir, home_size);
		if (added != 0)
			return EINVAL;
		added = login_append(out, capacity, &used, "/", 1);
		if (added != 0) {
			out[0] = '\0';
			return EINVAL;
		}
	}

	/* Copies the pattern, expanding each token. */
	for (index = 0; pattern[index] != '\0'; index++) {
		if (pattern[index] != '%') {
			added = login_append(out, capacity, &used, pattern + index, 1);
			if (added != 0) {
				out[0] = '\0';
				return EINVAL;
			}
			continue;
		}

		/* A token: the byte after the '%' says which. */
		next = pattern[index + 1];
		if (next == 'h') {
			added = login_append(out, capacity, &used, account->pw_dir, home_size);
		} else if (next == 'u') {
			added = login_append(out, capacity, &used, account->pw_name, name_size);
		} else if (next == '%') {
			added = login_append(out, capacity, &used, "%", 1);
		} else {
			added = EINVAL;
		}
		if (added != 0) {
			out[0] = '\0';
			return EINVAL;
		}
		index++;
	}

	/* A path that names no file at all is refused. */
	if (used == 0)
		return EINVAL;

	/* Succeeded: out holds the account's path. */
	return 0;
}

/*
 * Checks that tty names a pseudo-terminal and that it is a character
 * device owned by uid.
 */
int
sshd_tty_check(
	const char *tty,
	uid_t uid)
{
	struct stat status;
	const char *number;
	int shaped;
	int looked;

	/* Refuses a path that is not a pseudo-terminal's. */
	shaped = login_tty_number(tty, &number);
	if (shaped != 0)
		return EACCES;

	/* Looks at the terminal itself, not at what a link would name. */
	looked = lstat(tty, &status);
	if (looked != 0)
		return EACCES;

	/* Refuses anything but a character device. */
	if (!S_ISCHR(status.st_mode))
		return EACCES;

	/* Refuses a terminal of another user. */
	if (status.st_uid != uid)
		return EACCES;

	/* Succeeded: the terminal is the user's. */
	return 0;
}

/* Runs crypt on the dummy hash for a password that cannot log in, then wipes the password and crypt's answer. */
static void
login_dummy_crypt(
	char *password)
{
	char *hash;
	size_t length;

	/* Spends the work a real check spends. */
	hash = crypt(password, LOGIN_DUMMY_HASH);

	/* Wipes the password. */
	length = strlen(password);
	ssh_wipe(password, length);

	/* Wipes the hash of it that crypt keeps. */
	if (hash != NULL) {
		length = strlen(hash);
		ssh_wipe(hash, length);
	}
}

/*
 * Finds user in passwd (into account, its strings in buffer) and, when
 * readable, its shadow hash (copied into hash; *have_hash 1).  Returns 0,
 * or ENOENT when passwd has no such user.
 */
static int
login_lookup(
	const char *user,
	struct passwd *account,
	char *buffer,
	size_t size,
	char *hash,
	size_t hash_size,
	int *have_hash)
{
	struct passwd *found;
	struct spwd shadow;
	struct spwd *shadow_found;
	char shadow_buffer[LOGIN_VERIFY_BUFFER];
	size_t user_size;
	size_t hash_length;
	int error;

	/* Starts with no hash. */
	*have_hash = 0;
	hash[0] = '\0';

	/* Refuses a name that cannot be an account's. */
	user_size = strlen(user);
	if (user_size == 0 || user_size > SSH_USER_MAX)
		return ENOENT;

	/* Looks the account up in passwd. */
	found = NULL;
	error = getpwnam_r(user, account, buffer, size, &found);
	if (error != 0 || found == NULL)
		return ENOENT;

	/* Looks its password up in shadow, which only root can read. */
	shadow_found = NULL;
	error = getspnam_r(user, &shadow, shadow_buffer, sizeof(shadow_buffer), &shadow_found);
	if (error == 0 && shadow_found != NULL && shadow.sp_pwdp != NULL) {
		hash_length = strlen(shadow.sp_pwdp);
		if (hash_length < hash_size) {
			memcpy(hash, shadow.sp_pwdp, hash_length + 1U);
			*have_hash = 1;
		}
	}
	ssh_wipe(shadow_buffer, sizeof(shadow_buffer));

	/* Succeeded: the account exists. */
	return 0;
}

/*
 * Checks that tty is "/dev/pts/" followed by 1 to SSHD_PTS_DIGITS_MAX
 * digits and nothing else; *number points at the digits.  Returns 0 or
 * EINVAL.
 */
static int
login_tty_number(
	const char *tty,
	const char **number)
{
	size_t prefix_size;
	size_t index;
	int same;

	/* Refuses a missing path. */
	*number = NULL;
	if (tty == NULL)
		return EINVAL;

	/* Needs the directory of the pseudo-terminals in front. */
	prefix_size = sizeof(SSHD_PTS_DIRECTORY) - 1U;
	same = strncmp(tty, SSHD_PTS_DIRECTORY, prefix_size);
	if (same != 0)
		return EINVAL;

	/* Needs digits to the end, at least one and at most SSHD_PTS_DIGITS_MAX. */
	for (index = 0; tty[prefix_size + index] != '\0'; index++) {
		if (index >= SSHD_PTS_DIGITS_MAX)
			return EINVAL;
		if (tty[prefix_size + index] < '0' || tty[prefix_size + index] > '9')
			return EINVAL;
	}

	/* Refuses the directory without a number. */
	if (index == 0)
		return EINVAL;

	/* Succeeded: the path is a pseudo-terminal's. */
	*number = tty + prefix_size;
	return 0;
}

/* Appends size bytes of text to out (capacity with the NUL, *used so far); 0 or ENAMETOOLONG. */
static int
login_append(
	char *out,
	size_t capacity,
	size_t *used,
	const char *text,
	size_t size)
{
	/* Refuses what does not fit with the NUL. */
	if (size >= capacity - *used)
		return ENAMETOOLONG;

	/* Copies the bytes and ends the string after them. */
	memcpy(out + *used, text, size);
	*used += size;
	out[*used] = '\0';

	/* Succeeded: the text is appended. */
	return 0;
}

/*
 * Turns how the reading child ended (status, and the size bytes it sent)
 * into the answer of sshd_key_authorized, logging the reason of a
 * refusal: 0 when the whole file came, EIO when the child could not take
 * the account's ids as root, EACCES otherwise.
 */
static int
login_keys_verdict(
	const struct passwd *account,
	int status,
	size_t size)
{
	int code;

	/* A child killed by a signal sent nothing usable. */
	code = -1;
	if (WIFEXITED(status))
		code = WEXITSTATUS(status);

	/* Says why the file is not used, by how the child ended. */
	switch (code) {
	case LOGIN_KEYS_READ:
		break;
	case LOGIN_KEYS_UNSAFE:
		syslog(LOG_AUTH | LOG_NOTICE, "authorized keys of %.64s: bad ownership or modes, or not a regular file", account->pw_name);
		return EACCES;
	case LOGIN_KEYS_ABSENT:
		syslog(LOG_AUTH | LOG_INFO, "authorized keys of %.64s: no file", account->pw_name);
		return EACCES;
	case LOGIN_KEYS_TOO_LARGE:
		syslog(LOG_AUTH | LOG_NOTICE, "authorized keys of %.64s: file too large", account->pw_name);
		return EACCES;
	case LOGIN_KEYS_NOT_USER:
		syslog(LOG_AUTH | LOG_NOTICE, "authorized keys of %.64s: not the user sshd runs as", account->pw_name);
		return EACCES;
	case LOGIN_KEYS_IDS:
		syslog(LOG_AUTH | LOG_ERR, "authorized keys of %.64s: cannot take the user's ids", account->pw_name);
		return EIO;
	default:
		syslog(LOG_AUTH | LOG_NOTICE, "authorized keys of %.64s: not read", account->pw_name);
		return EACCES;
	}

	/* Refuses more bytes than a file may hold, whatever the child said. */
	if (size > SSH_HOSTFILE_MAX)
		return EACCES;

	/* Succeeded: the whole file came. */
	return 0;
}

/*
 * Runs in the child that reads the authorized_keys file: takes the
 * account's ids, opens and checks the file, copies it to output, and
 * exits with what happened (never returns).
 */
static void
login_keys_child(
	const struct sshd_config *config,
	const struct passwd *account,
	const char *path,
	int output)
{
	int taken;
	int opened;
	int copied;
	int descriptor;

	/* Becomes the user before anything of the user's is touched. */
	taken = login_keys_take_ids(account);
	if (taken != LOGIN_KEYS_READ)
		_exit(taken);

	/* Opens the file and checks it and the directories above it. */
	opened = login_keys_open(config, account, path, &descriptor);
	if (opened != LOGIN_KEYS_READ)
		_exit(opened);

	/* Passes the bytes on and reports how the copy ended. */
	copied = login_keys_copy(descriptor, output);
	(void)close(descriptor);
	(void)close(output);
	_exit(copied);
}

/*
 * Gives the child the account's groups and ids when sshd runs as root,
 * and checks that they cannot be undone; without root, checks that the
 * process already is the account.  Returns LOGIN_KEYS_READ (0),
 * LOGIN_KEYS_NOT_USER or LOGIN_KEYS_IDS.
 */
static int
login_keys_take_ids(
	const struct passwd *account)
{
	uid_t self;
	int changed;

	/* Without root, the account must be the one sshd runs as. */
	self = geteuid();
	if (self != 0) {
		if (self != account->pw_uid)
			return LOGIN_KEYS_NOT_USER;
		return LOGIN_KEYS_READ;
	}

	/* Takes the account's groups. */
	changed = initgroups(account->pw_name, account->pw_gid);
	if (changed != 0)
		return LOGIN_KEYS_IDS;

	/* Takes its group, which as root sets the real, effective and saved group alike. */
	changed = setgid(account->pw_gid);
	if (changed != 0)
		return LOGIN_KEYS_IDS;

	/* Takes its user, which as root sets the real, effective and saved user alike. */
	changed = setuid(account->pw_uid);
	if (changed != 0)
		return LOGIN_KEYS_IDS;

	/* Checks that the ids took. */
	if (getuid() != account->pw_uid || geteuid() != account->pw_uid)
		return LOGIN_KEYS_IDS;
	if (getgid() != account->pw_gid || getegid() != account->pw_gid)
		return LOGIN_KEYS_IDS;

	/* Checks that an account other than root cannot take root back. */
	if (account->pw_uid != 0) {
		changed = setuid(0);
		if (changed == 0)
			return LOGIN_KEYS_IDS;
	}

	/* Succeeded: the child is the account. */
	return LOGIN_KEYS_READ;
}

/*
 * Opens the authorized_keys file at path (without following a link at
 * its end, without blocking on a FIFO) and checks it: a regular file
 * always, and with StrictModes owned by the account or root, not
 * writable by group or others, and the same for the directories above it
 * (login_keys_open_strict).  Returns LOGIN_KEYS_READ with *descriptor
 * open, or why not.
 */
static int
login_keys_open(
	const struct sshd_config *config,
	const struct passwd *account,
	const char *path,
	int *descriptor)
{
	struct stat status;
	int opened;
	int looked;
	int saved;

	/* With StrictModes, walks the path a directory at a time. */
	if (config->strict_modes) {
		opened = login_keys_open_strict(path, account->pw_dir, account->pw_uid, descriptor);
		return opened;
	}

	/* Without, opens the path in one step, still refusing a link at its end. */
	*descriptor = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
	if (*descriptor < 0) {
		saved = errno;
		if (saved == ENOENT)
			return LOGIN_KEYS_ABSENT;
		return LOGIN_KEYS_UNSAFE;
	}

	/* Refuses anything but a regular file: a FIFO, a device, a directory. */
	looked = fstat(*descriptor, &status);
	if (looked != 0 || !S_ISREG(status.st_mode)) {
		(void)close(*descriptor);
		*descriptor = -1;
		return LOGIN_KEYS_UNSAFE;
	}

	/* Succeeded: the file is open. */
	return LOGIN_KEYS_READ;
}

/*
 * Opens path with StrictModes: from the home directory when path is
 * below it (the home directory itself is opened as named, so a link to
 * it is followed), otherwise from the root; then each directory below
 * with O_NOFOLLOW and O_DIRECTORY, refusing a "..", and the file with
 * O_NOFOLLOW.  Every directory opened and the file are checked with
 * login_keys_safe.  Returns LOGIN_KEYS_READ with *descriptor open, or
 * why not.
 */
static int
login_keys_open_strict(
	const char *path,
	const char *home,
	uid_t uid,
	int *descriptor)
{
	char start[SSHD_KEYS_PATH_MAX];
	char name[LOGIN_NAME_MAX];
	const char *rest;
	const char *end;
	size_t home_size;
	size_t name_size;
	int directory;
	int next;
	int safe;
	int saved;
	int same;

	/* Starts without a file. */
	*descriptor = -1;

	/* Finds where the walk starts: the home directory when the path is below it, otherwise the root. */
	home_size = strlen(home);
	while (home_size > 1 && home[home_size - 1] == '/')
		home_size--;
	same = strncmp(path, home, home_size);
	if (home_size > 1 && home_size < sizeof(start) && same == 0 && path[home_size] == '/') {
		memcpy(start, home, home_size);
		start[home_size] = '\0';
		rest = path + home_size + 1;
	} else {
		start[0] = '/';
		start[1] = '\0';
		rest = path + 1;
	}

	/* Opens and checks the directory the walk starts at. */
	directory = open(start, O_RDONLY | O_DIRECTORY | O_NOCTTY | O_CLOEXEC);
	if (directory < 0)
		return LOGIN_KEYS_ABSENT;
	safe = login_keys_safe(directory, uid, 1);
	if (safe != 0) {
		(void)close(directory);
		return LOGIN_KEYS_UNSAFE;
	}

	/* Takes the path a name at a time; each name before the last is a directory. */
	for (;;) {
		end = strchr(rest, '/');
		if (end == NULL)
			end = rest + strlen(rest);
		name_size = (size_t)(end - rest);

		/* Passes over an empty name (a doubled '/') and "." before the last name. */
		if (*end == '/' && (name_size == 0 || (name_size == 1 && rest[0] == '.'))) {
			rest = end + 1;
			continue;
		}

		/* Refuses a name that climbs out, an empty last name, and a name that does not fit. */
		if (name_size == 0 ||
		    name_size >= sizeof(name) ||
		    (name_size == 2 && rest[0] == '.' && rest[1] == '.')) {
			(void)close(directory);
			return LOGIN_KEYS_UNSAFE;
		}
		memcpy(name, rest, name_size);
		name[name_size] = '\0';

		/* The last name is the file. */
		if (*end == '\0')
			break;

		/* Opens the next directory without following a link. */
		next = openat(directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
		saved = errno;
		(void)close(directory);
		if (next < 0) {
			if (saved == ENOENT)
				return LOGIN_KEYS_ABSENT;
			return LOGIN_KEYS_UNSAFE;
		}
		directory = next;

		/* Checks it as the first. */
		safe = login_keys_safe(directory, uid, 1);
		if (safe != 0) {
			(void)close(directory);
			return LOGIN_KEYS_UNSAFE;
		}
		rest = end + 1;
	}

	/* Opens the file without following a link and without blocking on a FIFO. */
	*descriptor = openat(directory, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
	saved = errno;
	(void)close(directory);
	if (*descriptor < 0) {
		if (saved == ENOENT)
			return LOGIN_KEYS_ABSENT;
		return LOGIN_KEYS_UNSAFE;
	}

	/* Checks the file. */
	safe = login_keys_safe(*descriptor, uid, 0);
	if (safe != 0) {
		(void)close(*descriptor);
		*descriptor = -1;
		return LOGIN_KEYS_UNSAFE;
	}

	/* Succeeded: the file is open and safe. */
	return LOGIN_KEYS_READ;
}

/*
 * Checks the open descriptor with fstat: a directory (directory 1) or a
 * regular file (0), owned by uid or root, not writable by group or
 * others.  Returns 0 or EACCES.
 */
static int
login_keys_safe(
	int descriptor,
	uid_t uid,
	int directory)
{
	struct stat status;
	int looked;

	/* Reads what the descriptor is. */
	looked = fstat(descriptor, &status);
	if (looked != 0)
		return EACCES;

	/* Refuses another kind than the one the walk expects here. */
	if (directory && !S_ISDIR(status.st_mode))
		return EACCES;
	if (!directory && !S_ISREG(status.st_mode))
		return EACCES;

	/* Refuses an owner other than the account and root. */
	if (status.st_uid != uid && status.st_uid != 0)
		return EACCES;

	/* Refuses what others than the owner could change. */
	if ((status.st_mode & LOGIN_UNSAFE_MODE) != 0)
		return EACCES;

	/* Succeeded: only the owner can change it. */
	return 0;
}

/*
 * Copies the open file to output, at most SSH_HOSTFILE_MAX bytes.
 * Returns LOGIN_KEYS_READ, LOGIN_KEYS_TOO_LARGE or LOGIN_KEYS_FAILED.
 */
static int
login_keys_copy(
	int input,
	int output)
{
	char chunk[LOGIN_COPY_CHUNK];
	size_t total;
	ssize_t got;
	int written;

	/* Copies until the end of the file, stopping at the first byte over the limit. */
	total = 0;
	for (;;) {
		got = read(input, chunk, sizeof(chunk));
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
			return LOGIN_KEYS_FAILED;
		if (got == 0)
			break;
		if ((size_t)got > SSH_HOSTFILE_MAX - total)
			return LOGIN_KEYS_TOO_LARGE;
		total += (size_t)got;

		/* Passes the bytes on. */
		written = login_write_all(output, chunk, (size_t)got);
		if (written != 0)
			return LOGIN_KEYS_FAILED;
	}

	/* Succeeded: the whole file went to the monitor. */
	return LOGIN_KEYS_READ;
}

/*
 * Reads what the child sends on input into text (at most capacity bytes:
 * one more than a file may hold, so a child that sends too much is seen)
 * until the end, then reaps the child into *status.  A child that takes
 * longer than SSHD_KEYS_TIMEOUT_MS or sends more than capacity is killed.
 * Returns 0, or ETIMEDOUT or EIO (the child is reaped either way).
 */
static int
login_keys_collect(
	pid_t child,
	int input,
	char *text,
	size_t capacity,
	size_t *size,
	int *status)
{
	struct pollfd waiting;
	struct timespec start;
	long elapsed;
	ssize_t got;
	int ready;
	int failure;
	pid_t reaped;

	/* Reads until the end of the pipe, the time limit, or one byte too many. */
	*size = 0;
	*status = 0;
	failure = 0;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		elapsed = login_elapsed_ms(&start);
		if (elapsed >= (long)SSHD_KEYS_TIMEOUT_MS) {
			failure = ETIMEDOUT;
			break;
		}

		/* Waits for bytes no longer than the time left. */
		waiting.fd = input;
		waiting.events = POLLIN;
		waiting.revents = 0;
		ready = poll(&waiting, 1, (int)((long)SSHD_KEYS_TIMEOUT_MS - elapsed));
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0) {
			failure = EIO;
			break;
		}
		if (ready == 0)
			continue;

		/* Refuses to hold more than capacity. */
		if (*size >= capacity) {
			failure = EIO;
			break;
		}

		/* Takes what has come. */
		got = read(input, text + *size, capacity - *size);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			failure = EIO;
			break;
		}
		if (got == 0)
			break;
		*size += (size_t)got;
	}

	/* Ends a child that has not finished, then reaps it. */
	if (failure != 0)
		(void)kill(child, SIGKILL);
	reaped = login_wait(child, status);
	if (reaped != child && failure == 0)
		failure = EIO;

	/* Reports why the bytes cannot be used. */
	if (failure != 0)
		return failure;

	/* Succeeded: text holds what the child sent and *status how it ended. */
	return 0;
}

/* Returns the milliseconds since start (CLOCK_MONOTONIC), at least 0. */
static long
login_elapsed_ms(
	const struct timespec *start)
{
	struct timespec now;
	long elapsed;

	/* Measures the time since start. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	elapsed = (long)(now.tv_sec - start->tv_sec) * 1000L + (now.tv_nsec - start->tv_nsec) / 1000000L;
	if (elapsed < 0)
		elapsed = 0;

	/* Reports the time passed. */
	return elapsed;
}

/* Writes all size bytes to descriptor; 0 or EIO. */
static int
login_write_all(
	int descriptor,
	const char *data,
	size_t size)
{
	size_t done;
	ssize_t written;

	/* Writes until every byte has gone, retrying after a signal. */
	done = 0;
	while (done < size) {
		written = write(descriptor, data + done, size - done);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return EIO;
		done += (size_t)written;
	}

	/* Succeeded: every byte was written. */
	return 0;
}

/* Reaps child into *status, retrying after a signal; returns child, or -1 on a failure. */
static pid_t
login_wait(
	pid_t child,
	int *status)
{
	pid_t reaped;

	/* Waits for the child to end. */
	do {
		reaped = waitpid(child, status, 0);
	} while (reaped < 0 && errno == EINTR);

	/* Reports the child reaped. */
	return reaped;
}
