/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The accounts' shared core (ws160-p001; the 2026-10-05 user request "su,
 * sudoを実装してください。" and "passwd を実装する").
 *
 * A new password is at least ACCOUNT_PASSWORD_MIN characters when a user
 * chooses it (root setting one for an account may choose any that is not
 * empty), at most ACCOUNT_PASSWORD_MAX, of printable characters, and not
 * the one it replaces.  Its hash is SHA-512 crypt ("$6$") with
 * ACCOUNT_HASH_ROUNDS rounds and sixteen random characters of salt from
 * getentropy.
 *
 * /etc/shadow is replaced, never written in place: under the lock (an
 * exclusive flock on the lock file, which the kernel gives back when its
 * holder exits or dies; ws196-p002), the whole file is read, the user's
 * line gets the new hash and today's day in its third field (every other
 * byte is kept), the result is written to a new file in /etc with the
 * shadow's mode (0400) and synced, and renamed over /etc/shadow; then /etc
 * itself is synced.  The signals
 * that would stop the program midway are held meanwhile.  A reader sees
 * the old file or the new one, never a part.
 *
 * The wheel group (by name) holds a user when it is the user's primary
 * group or names the user.
 *
 * The environment of a command run as another user keeps only the
 * terminal's and the locale's variables of the caller (TERM, COLORTERM,
 * LANG, LC_*, TZ) and sets HOME, SHELL, USER, LOGNAME and the secure PATH,
 * with SUDO_USER, SUDO_UID, SUDO_GID and SUDO_COMMAND for sudo.  Nothing
 * else of the caller's comes along (LD_*, IFS, ENV and the rest).
 */

#include "userland/base/common/account.h"

#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The salt's characters and length. */
#define ACCOUNT_SALT_CHARACTERS	"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
#define ACCOUNT_SALT_LENGTH	16U

/* How many times the lock is tried, and the wait between (microseconds): five seconds in all. */
#define ACCOUNT_LOCK_TRIES	50U
#define ACCOUNT_LOCK_WAIT_US	100000U

/* The largest shadow file handled. */
#define ACCOUNT_SHADOW_MAX	(256U * 1024U)

/* The new shadow's name pattern, in /etc so that the rename stays in one file system. */

/* The caller's variables that come along. */
static const char *const account_kept[] = { "TERM=", "COLORTERM=", "LANG=", "LC_", "TZ=" };

/*
 * The open lock file while this process holds the lock of the account
 * files, -1 otherwise.
 *
 * The lock is an exclusive flock on it, so closing the descriptor -- by
 * account_files_unlock, or by the kernel when the process exits or dies --
 * gives the lock back; a lock is never taken over from a holder that is
 * still there.  The file itself is never removed: a waiter that opened it
 * would otherwise lock a file nobody else sees.
 */
static int account_lock_descriptor = -1;

static int account_append(char *storage, size_t storage_size, size_t *used, char **variables, size_t capacity, size_t *count, const char *name, const char *value);
static int account_kept_variable(const char *variable);

/*
 * Checks a new password against the rules: by_root is nonzero when root
 * sets it for an account, previous (or NULL) the password it replaces.
 * Returns ACCOUNT_PASSWORD_OK or the reason it is refused.
 */
int
account_password_check(
	const char *password,
	const char *previous,
	int by_root)
{
	size_t length;
	size_t index;
	int same;

	/* Its length: not empty, long enough for a user's choice, not too long. */
	length = strlen(password);
	if (length == 0U)
		return ACCOUNT_PASSWORD_SHORT;
	if (!by_root && length < ACCOUNT_PASSWORD_MIN)
		return ACCOUNT_PASSWORD_SHORT;
	if (length > ACCOUNT_PASSWORD_MAX)
		return ACCOUNT_PASSWORD_LONG;

	/* Printable characters only (UTF-8's bytes above 0x7f too), no control character. */
	for (index = 0; index < length; index++) {
		if ((unsigned char)password[index] < 0x20U || (unsigned char)password[index] == 0x7fU)
			return ACCOUNT_PASSWORD_CHARACTER;
	}

	/* Not the one it replaces. */
	if (previous != NULL) {
		same = strcmp(password, previous);
		if (same == 0)
			return ACCOUNT_PASSWORD_SAME;
	}

	/* Succeeded: it may be used. */
	return ACCOUNT_PASSWORD_OK;
}

/* Says why a new password was refused, as a message. */
const char *
account_password_reason(
	int reason)
{
	/* Each reason's message. */
	switch (reason) {
	case ACCOUNT_PASSWORD_SHORT:
		return "the password is too short (at least 8 characters)";
	case ACCOUNT_PASSWORD_LONG:
		return "the password is too long";
	case ACCOUNT_PASSWORD_CHARACTER:
		return "the password has a control character";
	case ACCOUNT_PASSWORD_SAME:
		return "the new password is the old one";
	default:
		return "the password is accepted";
	}
}

/*
 * Makes a password's SHA-512 crypt hash with a random salt.  Returns 0, or
 * an errno value (from getentropy or crypt, or ERANGE when it does not fit).
 */
int
account_password_hash(
	const char *password,
	char *hash,
	size_t size)
{
	unsigned char random[ACCOUNT_SALT_LENGTH];
	char setting[64];
	const char *result;
	size_t length;
	size_t index;
	int error;

	/* The salt's random bytes. */
	error = getentropy(random, sizeof(random));
	if (error != 0)
		return errno;

	/* The setting: the rounds and the salt's characters. */
	length = (size_t)snprintf(setting, sizeof(setting), "$6$rounds=%u$", ACCOUNT_HASH_ROUNDS);
	for (index = 0; index < ACCOUNT_SALT_LENGTH; index++) {
		setting[length] = ACCOUNT_SALT_CHARACTERS[random[index] % 64U];
		length++;
	}

	/* The salt ends with a dollar. */
	setting[length] = '$';
	setting[length + 1U] = '\0';
	memset(random, 0, sizeof(random));

	/* The hash. */
	result = crypt(password, setting);
	if (result == NULL)
		return errno;
	length = strlen(result);
	if (length + 1U > size || length + 1U > ACCOUNT_HASH_MAX)
		return ERANGE;

	/* Succeeded: the hash is copied. */
	memcpy(hash, result, length + 1U);
	return 0;
}

/*
 * Replaces a user's hash in the text of a shadow file: the line whose
 * first field is name gets hash as its second field and day (days since
 * 1970) as its third; every other byte is copied.  Returns 0 with the new
 * text in output (written bytes), ENOENT when no line names the user,
 * EINVAL for a line of the user without its fields or a hash with a colon
 * or a line end, ERANGE when the output does not fit.
 */
int
account_shadow_replace(
	const char *text,
	size_t length,
	const char *name,
	const char *hash,
	long day,
	char *output,
	size_t capacity,
	size_t *written)
{
	char changed[32];
	const char *line;
	const char *end;
	const char *field;
	const char *rest;
	size_t name_length;
	size_t hash_length;
	size_t clean_length;
	size_t changed_length;
	size_t used;
	size_t piece;
	int found;
	int mine;
	int differs;

	/* The hash may not break the line's fields. */
	hash_length = strlen(hash);
	clean_length = strcspn(hash, ":\n");
	if (clean_length != hash_length)
		return EINVAL;
	name_length = strlen(name);
	changed_length = (size_t)snprintf(changed, sizeof(changed), "%ld", day);

	/* Each line. */
	found = 0;
	used = 0;
	line = text;
	while (line < text + length) {
		/* The line, with its end if it has one. */
		end = memchr(line, '\n', (size_t)(text + length - line));
		if (end == NULL)
			end = text + length;
		else
			end++;
		piece = (size_t)(end - line);

		/* A line of another user, or not of the form, is copied. */
		mine = 0;
		if (!found && piece > name_length && line[name_length] == ':') {
			differs = memcmp(line, name, name_length);
			if (differs == 0)
				mine = 1;
		}

		/* Copied as it is. */
		if (!mine) {
			if (used + piece > capacity)
				return ERANGE;
			memcpy(output + used, line, piece);
			used += piece;
			line = end;
			continue;
		}

		/* The user's line: its third field starts after the second colon. */
		field = memchr(line + name_length + 1U, ':', (size_t)(end - line - (ptrdiff_t)name_length - 1));
		if (field == NULL)
			return EINVAL;
		rest = memchr(field + 1, ':', (size_t)(end - field - 1));
		if (rest == NULL)
			return EINVAL;

		/* The name, the new hash, the day, and the rest of the line from the third colon. */
		if (used + name_length + 1U + hash_length + 1U + changed_length + (size_t)(end - rest) > capacity)
			return ERANGE;
		memcpy(output + used, line, name_length + 1U);
		used += name_length + 1U;
		memcpy(output + used, hash, hash_length);
		used += hash_length;
		output[used] = ':';
		used++;
		memcpy(output + used, changed, changed_length);
		used += changed_length;
		memcpy(output + used, rest, (size_t)(end - rest));
		used += (size_t)(end - rest);
		found = 1;
		line = end;
	}

	/* No line named the user. */
	if (!found)
		return ENOENT;

	/* Succeeded: the new text. */
	*written = used;
	return 0;
}

/*
 * Sets a user's hash in /etc/shadow, safely (see the file's comment).
 * Returns 0, or an errno value: EBUSY when the lock stays taken, ENOENT
 * when the shadow has no line of the user.
 */
int
account_shadow_set(
	const char *name,
	const char *hash)
{
	sigset_t held;
	sigset_t previous;
	char *text;
	char *output;
	size_t length;
	size_t written;
	long day;
	int error;

	/* The signals that would stop the work midway are held. */
	sigemptyset(&held);
	sigaddset(&held, SIGINT);
	sigaddset(&held, SIGTERM);
	sigaddset(&held, SIGHUP);
	sigaddset(&held, SIGQUIT);
	sigaddset(&held, SIGTSTP);
	sigprocmask(SIG_BLOCK, &held, &previous);

	/* The buffers. */
	text = malloc(ACCOUNT_SHADOW_MAX);
	output = malloc(ACCOUNT_SHADOW_MAX + ACCOUNT_HASH_MAX + 64U);
	error = ENOMEM;
	if (text != NULL && output != NULL)
		error = account_files_lock();

	/* Under the lock: the file, its user's line replaced, written anew. */
	if (error == 0) {
		day = (long)(time(NULL) / 86400);
		error = account_file_read(ACCOUNT_SHADOW_PATH, text, ACCOUNT_SHADOW_MAX, &length);
		if (error == 0)
			error = account_shadow_replace(text, length, name, hash, day, output, ACCOUNT_SHADOW_MAX + ACCOUNT_HASH_MAX + 64U, &written);
		if (error == 0)
			error = account_file_write(ACCOUNT_SHADOW_PATH, 0400, output, written);
		account_files_unlock();
	}

	/* The copies of the file are wiped (they hold every hash), and the signals come again. */
	if (text != NULL) {
		memset(text, 0, ACCOUNT_SHADOW_MAX);
		free(text);
	}

	/* So is the new text. */
	if (output != NULL) {
		memset(output, 0, ACCOUNT_SHADOW_MAX + ACCOUNT_HASH_MAX + 64U);
		free(output);
	}

	/* The signals come again. */
	sigprocmask(SIG_SETMASK, &previous, NULL);

	/* The result. */
	return error;
}

/* Tells whether a user is in the wheel group: as its primary group, or named in it. */
int
account_in_wheel(
	const char *name,
	gid_t primary)
{
	struct group *wheel;
	unsigned index;
	int same;

	/* The wheel group. */
	wheel = getgrnam("wheel");
	if (wheel == NULL)
		return 0;

	/* The user's primary group. */
	if (wheel->gr_gid == primary)
		return 1;

	/* Or a member by name. */
	for (index = 0; wheel->gr_mem != NULL && wheel->gr_mem[index] != NULL; index++) {
		same = strcmp(wheel->gr_mem[index], name);
		if (same == 0)
			return 1;
	}

	/* Not in it. */
	return 0;
}

/*
 * Builds the environment of a command run as another user into variables
 * (at most capacity, NULL-terminated), its strings in storage.  Returns the
 * number of variables, or 0 when they do not fit.
 */
size_t
account_environment(
	const struct account_environment_input *input,
	char *storage,
	size_t storage_size,
	char **variables,
	size_t capacity)
{
	char number[24];
	size_t used;
	size_t count;
	size_t length;
	size_t index;
	int kept;
	int error;

	/* Room for the terminating NULL. */
	if (capacity == 0U)
		return 0;
	capacity--;

	/* The caller's variables of the terminal and the locale. */
	used = 0;
	count = 0;
	for (index = 0; input->caller != NULL && input->caller[index] != NULL; index++) {
		/* Only the kept ones. */
		kept = account_kept_variable(input->caller[index]);
		if (!kept)
			continue;

		/* The whole "NAME=value". */
		length = strlen(input->caller[index]) + 1U;
		if (used + length > storage_size || count >= capacity)
			return 0;
		memcpy(storage + used, input->caller[index], length);
		variables[count] = storage + used;
		used += length;
		count++;
	}

	/* The target's. */
	error = account_append(storage, storage_size, &used, variables, capacity, &count, "HOME", input->home);
	if (error == 0)
		error = account_append(storage, storage_size, &used, variables, capacity, &count, "SHELL", input->shell);
	if (error == 0)
		error = account_append(storage, storage_size, &used, variables, capacity, &count, "USER", input->user);
	if (error == 0)
		error = account_append(storage, storage_size, &used, variables, capacity, &count, "LOGNAME", input->user);
	if (error == 0)
		error = account_append(storage, storage_size, &used, variables, capacity, &count, "PATH", ACCOUNT_SECURE_PATH);
	if (error != 0)
		return 0;

	/* sudo's own. */
	if (input->sudo_user != NULL) {
		error = account_append(storage, storage_size, &used, variables, capacity, &count, "SUDO_USER", input->sudo_user);
		(void)snprintf(number, sizeof(number), "%u", (unsigned)input->sudo_uid);
		if (error == 0)
			error = account_append(storage, storage_size, &used, variables, capacity, &count, "SUDO_UID", number);
		(void)snprintf(number, sizeof(number), "%u", (unsigned)input->sudo_gid);
		if (error == 0)
			error = account_append(storage, storage_size, &used, variables, capacity, &count, "SUDO_GID", number);
		if (error == 0 && input->sudo_command != NULL)
			error = account_append(storage, storage_size, &used, variables, capacity, &count, "SUDO_COMMAND", input->sudo_command);
		if (error != 0)
			return 0;
	}

	/* Succeeded: the list, ended. */
	variables[count] = NULL;
	return count;
}

/*
 * Takes the lock of the account files (passwd, group and shadow, and
 * passkey's file, change only under it), waiting a little for another
 * holder.  Returns 0, EBUSY when it stays taken, or an errno value.
 */
int
account_files_lock(void)
{
	unsigned tries;
	int descriptor;
	int taken;
	int error;

	/* Held already by this process. */
	if (account_lock_descriptor >= 0)
		return EDEADLK;

	/* The lock file, made when it is not there, never a link. */
	descriptor = open(ACCOUNT_SHADOW_LOCK, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (descriptor < 0)
		return errno;

	/* A few tries. */
	taken = 0;
	for (tries = 0; tries < ACCOUNT_LOCK_TRIES; tries++) {
		/* Taken: the descriptor holds it until it is closed. */
		error = flock(descriptor, LOCK_EX | LOCK_NB);
		if (error == 0) {
			taken = 1;
			break;
		}

		/* An error other than another holder. */
		if (errno != EWOULDBLOCK && errno != EINTR) {
			error = errno;
			(void)close(descriptor);
			return error;
		}

		/* Another holder: wait a little. */
		(void)usleep(ACCOUNT_LOCK_WAIT_US);
	}

	/* It stayed taken. */
	if (!taken) {
		(void)close(descriptor);
		return EBUSY;
	}

	/* Succeeded: this process holds the lock until it closes the descriptor. */
	account_lock_descriptor = descriptor;
	return 0;
}

/* Gives the lock of the account files back. */
void
account_files_unlock(void)
{
	/* Not held: nothing to give back. */
	if (account_lock_descriptor < 0)
		return;

	/* The descriptor closed, which gives the flock back; the file stays. */
	(void)close(account_lock_descriptor);
	account_lock_descriptor = -1;
}

/*
 * Reads a whole account file into a buffer.  Returns 0, EFBIG when it
 * does not fit, or an errno value.
 */
int
account_file_read(
	const char *path,
	char *buffer,
	size_t capacity,
	size_t *length)
{
	ssize_t got;
	size_t used;
	int descriptor;
	int error;

	/* The file. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* All of it, to its end; a file too large for the buffer is refused. */
	used = 0;
	error = 0;
	for (;;) {
		got = read(descriptor, buffer + used, capacity - used);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			error = errno;
			break;
		}

		/* The end of the file. */
		if (got == 0)
			break;
		used += (size_t)got;
		if (used == capacity) {
			error = EFBIG;
			break;
		}
	}

	/* The file is no longer needed. */
	(void)close(descriptor);

	/* Succeeded or not. */
	*length = used;
	return error;
}

/*
 * Replaces an account file with a text: written to a new file in its
 * directory with a mode, synced, renamed over it, and the directory
 * synced; a reader sees the old file or the new one.  Returns 0 or an
 * errno value (the old file is kept).
 */
int
account_file_write(
	const char *target,
	mode_t mode,
	const char *text,
	size_t length)
{
	char path[ACCOUNT_PATH_MAX];
	char directory_path[ACCOUNT_PATH_MAX];
	const char *slash;
	ssize_t put;
	size_t done;
	int descriptor;
	int directory;
	int written;
	int error;

	/* The new file's name, in the target's directory. */
	slash = strrchr(target, '/');
	if (slash == NULL || (size_t)(slash - target) + sizeof("/.account.XXXXXX") > sizeof(path))
		return ENAMETOOLONG;
	(void)snprintf(directory_path, sizeof(directory_path), "%.*s", (int)(slash - target), target);
	written = snprintf(path, sizeof(path), "%s/.account.XXXXXX", directory_path);
	if (written < 0 || (size_t)written >= sizeof(path))
		return ENAMETOOLONG;

	/* The new file, with its mode. */
	descriptor = mkstemp(path);
	if (descriptor < 0)
		return errno;
	error = fchmod(descriptor, mode);

	/* Its text. */
	done = 0;
	while (error == 0 && done < length) {
		put = write(descriptor, text + done, length - done);
		if (put < 0 && errno == EINTR)
			continue;
		if (put < 0) {
			error = errno;
			break;
		}

		/* The bytes written. */
		done += (size_t)put;
	}

	/* On the disk before the rename. */
	if (error == 0) {
		error = fsync(descriptor);
		if (error != 0)
			error = errno;
	}

	/* The file is no longer needed. */
	(void)close(descriptor);

	/* Over the old file; a failure leaves the old one and removes the new. */
	if (error == 0) {
		error = rename(path, target);
		if (error != 0)
			error = errno;
	}

	/* A failure. */
	if (error != 0) {
		(void)unlink(path);
		return error;
	}

	/* The directory's entry on the disk too. */
	directory = open(directory_path, O_RDONLY | O_CLOEXEC);
	if (directory >= 0) {
		(void)fsync(directory);
		(void)close(directory);
	}

	/* Succeeded: the file is the new one. */
	return 0;
}

/* Appends "NAME=value" to the environment being built; returns 0, or ERANGE when it does not fit. */
static int
account_append(
	char *storage,
	size_t storage_size,
	size_t *used,
	char **variables,
	size_t capacity,
	size_t *count,
	const char *name,
	const char *value)
{
	size_t length;

	/* No value: nothing. */
	if (value == NULL)
		return 0;

	/* Room for it. */
	length = strlen(name) + 1U + strlen(value) + 1U;
	if (*used + length > storage_size || *count >= capacity)
		return ERANGE;

	/* Succeeded: it is added. */
	(void)snprintf(storage + *used, length, "%s=%s", name, value);
	variables[*count] = storage + *used;
	*used += length;
	(*count)++;
	return 0;
}

/* Tells whether a caller's variable comes along. */
static int
account_kept_variable(
	const char *variable)
{
	size_t index;
	size_t length;
	int same;

	/* One of the kept prefixes. */
	for (index = 0; index < sizeof(account_kept) / sizeof(account_kept[0]); index++) {
		length = strlen(account_kept[index]);
		same = strncmp(variable, account_kept[index], length);
		if (same == 0)
			return 1;
	}

	/* Not kept. */
	return 0;
}
