/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * account-admin: the changes to the people's accounts (ws089-p026,
 * ws196-p002; docs/architecture/security.md, "Account administration").
 *
 *   /usr/libexec/account-admin < request
 *
 * Set-user-ID root, run by the compositor's backend for Settings and by
 * root's useradd, usermod and userdel (it is outside PATH).  The request
 * is text on standard input, one field a line (edit.h).  A caller who is
 * not root gives its password, the operation (add, remove,
 * reset-password, group-add, group-remove, system-language) and its
 * arguments; system-language (ws158-p004) sets the login screen's
 * language, /etc/keiland/language, and has no target account.  A caller
 * whose real user ID is 0 gives no password: one of root's operations
 * (user-add, user-modify, user-remove) and its keys.  Nothing comes from
 * the command line, and from the environment only root's SUDO_USER, for
 * the log.  The answer is one line on standard output, "ok" or "error
 * REASON", and the exit status is 0 or 1.
 *
 * The checks, in order: the caller (root, or a user in wheel -- otherwise
 * the password is not checked, so that the tool cannot test a
 * non-administrator's password -- whose password is right; a wrong one
 * costs two seconds); the target (no root or system account; for Settings,
 * not oneself for a removal or a wheel removal; not a user with a running
 * process for a removal, unless root forces it); the values (a new name,
 * display name, password, shell, user ID and groups by the rules).  Then
 * the three files' new texts are made in memory, and a change that would
 * leave no administrator who can log in, where there was one, is refused.
 * Only then are the files changed, under the lock of the account files:
 * the record of the last user ID given, /etc/passkey's lines of the name
 * (its PIN and security keys, ws172-p002: at an addition, so that none
 * left over is taken, a person's account then given the first PIN, 0000;
 * at a removal, so that a new account of the same name never finds them;
 * at a password's reset), then each account file
 * that changed, replaced atomically (group, passwd, shadow for an
 * addition, the reverse otherwise), every one written already put back
 * when a later one fails.  The signals that would stop the tool are held
 * meanwhile.  What may take long -- a new home with the skeleton, the
 * removal of an old home, a removed user's jobs and automatic login --
 * comes after the lock is given back.  Every request's result goes to the
 * system log (auth), never a password or a hash.
 */

#include "edit.h"

#include "userland/base/common/account.h"
#include "userland/base/login/verify.h"
#include "userland/base/passkey/passkey.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>

#include <uapi/system.h>

/* Where the people's homes are, the skeleton copied into a new one, and the shell they get. */
#ifndef ADMIN_HOME_BASE
#define ADMIN_HOME_BASE		"/home"
#endif
#ifndef ADMIN_SKELETON
#define ADMIN_SKELETON		"/etc/skel"
#endif
#define ADMIN_SHELL		"/bin/sh"

/* The group whose members may open the GPU's nodes, which Settings' new person joins. */
#define ADMIN_GPU_GROUP		"video"

/*
 * The system's language, the login screen's (ws158-p004; the compositor's
 * language.c reads it), and its directory.
 */
#ifndef ADMIN_LANGUAGE_FILE
#define ADMIN_LANGUAGE_FILE	"/etc/keiland/language"
#endif
#ifndef ADMIN_LANGUAGE_DIRECTORY
#define ADMIN_LANGUAGE_DIRECTORY	"/etc/keiland"
#endif

/*
 * The record of the highest user ID ever given to a person (one decimal
 * line), so that a removed user's ID is never given again.
 */
#ifndef ADMIN_LAST_ID_FILE
#define ADMIN_LAST_ID_FILE	"/etc/account-admin.last-id"
#endif

/* cron's spool (one file a user, named by its user ID) and at's (each job a file the user owns). */
#ifndef ADMIN_CRON_SPOOL
#define ADMIN_CRON_SPOOL	"/var/spool/cron"
#endif
#ifndef ADMIN_AT_SPOOL
#define ADMIN_AT_SPOOL		"/var/spool/at"
#endif

/* The file naming the account sessiond logs in by itself at boot (its first line). */
#ifndef ADMIN_AUTOLOGIN_FILE
#define ADMIN_AUTOLOGIN_FILE	"/etc/keiland/autologin"
#endif

/* The largest account file read, and the room of an edited one. */
#define ADMIN_FILE_MAX		(256U * 1024U)
#define ADMIN_OUTPUT_MAX	(ADMIN_FILE_MAX + 1024U)

/* The pause after a wrong password (seconds), the deepest home removed, and the most processes looked at. */
#define ADMIN_WRONG_PAUSE	2U
#define ADMIN_TREE_DEPTH	64
#define ADMIN_PROCESSES_MAX	4096U

/* The longest path built, and a line of an account file. */
#define ADMIN_PATH_MAX		512U
#define ADMIN_LINE_MAX		(ACCOUNT_HASH_MAX + 512U)

/*
 * The PIN a person's new account is given, which the person changes in
 * Settings: a machine without a keyboard (a tablet) can type it on the
 * login screen's keypad.
 */
#define ADMIN_FIRST_PIN		"0000"

/* The largest small file read whole: the record of the last user ID, and the automatic login's. */
#define ADMIN_SMALL_MAX		128U

/*
 * The three account files, in the order of the texts of struct
 * admin_files.
 */
enum admin_file {
	ADMIN_FILE_PASSWD,
	ADMIN_FILE_GROUP,
	ADMIN_FILE_SHADOW,
	ADMIN_FILE_COUNT
};

/*
 * One account file of a request: its path and mode, the text read under
 * the lock (kept as it was, so that a failure midway can write it back),
 * and its new text, which the operation edits.
 */
struct admin_text {
	const char *path;
	mode_t mode;
	char *original;
	size_t original_length;
	char *text;
	size_t length;
};

/*
 * The account files of one request, read under the lock: each file's
 * texts, the scratch buffer every edit goes through (it then becomes the
 * new text, and the old new text the scratch), and today (days since
 * 1970), the day the count of administrators judges expiry by.
 */
struct admin_files {
	struct admin_text texts[ADMIN_FILE_COUNT];
	char *scratch;
	long day;
};

/*
 * A user found in passwd: its user ID, its primary group, and its home.
 */
struct admin_user {
	long uid;
	long gid;
	char home[ADMIN_PATH_MAX];
};

/*
 * What a request leaves to do after the account files are written and the
 * lock is given back, since it may take long: a new home made for the user
 * and its group, an old home removed, and a removed user's jobs (by its
 * user ID; -1 when it is not known) and automatic login forgotten.
 */
struct admin_after {
	int make_home;
	int remove_home;
	int forget;
	char name[ADMIN_NAME_MAX + 1U];
	long uid;
	long gid;
};

int main(int argc, char **argv);
static void admin_sudo_user(uid_t real, char *name, size_t size);
static int admin_check_caller(struct admin_request *request, uid_t real, char *caller, size_t size);
static int admin_read_request(char *text, size_t capacity, size_t *length);
static int admin_apply(const struct admin_request *request, const char *caller);
static int admin_files_open(struct admin_files *files);
static void admin_files_close(struct admin_files *files);
static void admin_take(struct admin_files *files, enum admin_file which, size_t written);
static int admin_find_user(const struct admin_files *files, const char *name, struct admin_user *user);
static int admin_add(struct admin_files *files, const struct admin_request *request, struct admin_after *after);
static int admin_add_lines(struct admin_files *files, const struct admin_request *request, long id, long gid);
static int admin_remove(struct admin_files *files, const struct admin_request *request, const char *caller, struct admin_after *after);
static int admin_remove_rest(struct admin_files *files, const char *name, struct admin_after *after);
static int admin_reset(struct admin_files *files, const struct admin_request *request);
static int admin_group(struct admin_files *files, const struct admin_request *request, const char *caller);
static int admin_modify(struct admin_files *files, const struct admin_request *request);
static int admin_modify_lock(struct admin_files *files, const char *name, enum admin_lock lock);
static int admin_primary_group(const struct admin_files *files, const char *name, const char *group, long *gid);
static int admin_groups_join(struct admin_files *files, const char *name, const char *groups);
static int admin_keeps_administrator(const struct admin_files *files);
static int admin_commit(struct admin_files *files, int adding);
static int admin_write(const char *path, mode_t mode, const char *text, size_t length);
static long admin_id_floor(void);
static void admin_highest_in_directory(const char *path, int by_name, long *highest);
static int admin_last_id_raise(long id);
static int admin_passkey_forget(const char *name, int *changed);
static int admin_passkey_first(const char *name, long uid);
static int admin_passkey_lines(const char *name, const char *added, int *changed);
static int admin_passkey_edit(const char *name, const char *added, char *text, char *output, int *changed);
static int admin_busy(const char *name, long uid);
static int admin_shell_there(const char *shell);
static int admin_after_work(const struct admin_after *after);
static int admin_make_home(const char *name, long uid, long gid);
static void admin_copy_skeleton(int home, long uid, long gid);
static void admin_remove_home(const char *name, long uid);
static int admin_remove_tree(int parent, const char *name, int depth, dev_t device);
static void admin_spool_forget(long uid);
static void admin_autologin_forget(const char *name);
static int admin_dot(const char *name);
static void admin_answer(int reason);
static void admin_standard_descriptors(void);
static void admin_wipe(char *buffer, size_t size);
static int admin_system_language(const struct admin_request *request);

/*
 * Reads the request, checks the caller, and carries the request out.
 */
int
main(
	int argc,
	char **argv)
{
	char text[ADMIN_REQUEST_MAX + 2U];
	char caller[ADMIN_NAME_MAX + 1U];
	char sudo_user[ADMIN_NAME_MAX + 1U];
	struct admin_request request;
	sigset_t held;
	sigset_t previous;
	const char *operation_word;
	const char *reason_word;
	size_t length;
	uid_t real;
	uid_t effective;
	int by_root;
	int reason;
	int error;

	/* Nothing from the command line; descriptors 0 to 2 open. */
	(void)argc;
	(void)argv;
	admin_standard_descriptors();

	/* The caller, and for root the user who asked through sudo, read before the environment goes. */
	real = getuid();
	admin_sudo_user(real, sudo_user, sizeof(sudo_user));

	/* Nothing else from the environment; the files' modes; the log. */
	(void)clearenv();
	(void)umask(022);
	openlog("account-admin", LOG_PID, LOG_AUTH);

	/* Installed set-user-ID root, or it can do nothing. */
	effective = geteuid();
	if (effective != 0) {
		admin_answer(ADMIN_FAILED);
		return 1;
	}

	/* The request, bounded: a longer one is refused before any check. */
	error = admin_read_request(text, sizeof(text), &length);
	if (error != 0) {
		syslog(LOG_NOTICE, "refused: a request too long or unreadable");
		admin_answer(ADMIN_BAD_REQUEST);
		return 1;
	}

	/* Its lines: root's form for root, Settings' for anyone else. */
	by_root = 0;
	if (real == 0)
		by_root = 1;
	reason = admin_parse(text, length, by_root, &request);
	if (reason != ADMIN_OK) {
		admin_wipe(text, sizeof(text));
		syslog(LOG_NOTICE, "refused: a request not understood");
		admin_answer(reason);
		return 1;
	}

	/* The caller: root, or an administrator whose password is right. */
	(void)snprintf(caller, sizeof(caller), "root");
	if (real != 0) {
		reason = admin_check_caller(&request, real, caller, sizeof(caller));
		if (reason != ADMIN_OK) {
			admin_wipe(text, sizeof(text));
			admin_answer(reason);
			return 1;
		}
	}

	/* The work, with the signals that would stop it midway held. */
	sigemptyset(&held);
	sigaddset(&held, SIGINT);
	sigaddset(&held, SIGTERM);
	sigaddset(&held, SIGHUP);
	sigaddset(&held, SIGQUIT);
	sigaddset(&held, SIGTSTP);
	sigaddset(&held, SIGPIPE);
	(void)sigprocmask(SIG_BLOCK, &held, &previous);
	reason = admin_apply(&request, caller);
	(void)sigprocmask(SIG_SETMASK, &previous, NULL);

	/* The words the log line names the operation and its result by. */
	operation_word = admin_operation_word(request.operation);
	reason_word = admin_reason_word(reason);

	/* The result logged, with the user who asked root through sudo. */
	if (sudo_user[0] != '\0') {
		syslog(LOG_NOTICE, "%s by root (sudo %s) for %s: %s", operation_word, sudo_user, request.name, reason_word);
	} else {
		syslog(LOG_NOTICE, "%s by %s for %s: %s", operation_word, caller, request.name, reason_word);
	}

	/* The request (with any new password) goes; the result is answered. */
	admin_wipe(text, sizeof(text));
	admin_answer(reason);

	/* Reports a refusal or a failure. */
	if (reason != ADMIN_OK)
		return 1;

	/* Succeeded: the change is made. */
	return 0;
}

/*
 * Copies the name of the user who ran root's front end through sudo
 * (SUDO_USER), for the log only: anyone may set the variable, so nothing
 * is decided by it.  Empty when the caller is not root or the variable is
 * not set, "?" when it is not a name.
 */
static void
admin_sudo_user(
	uid_t real,
	char *name,
	size_t size)
{
	const char *value;
	size_t length;
	size_t i;

	/* None, unless root asks. */
	name[0] = '\0';
	if (real != 0)
		return;

	/* The variable. */
	value = getenv("SUDO_USER");
	if (value == NULL)
		return;

	/* A name that fits. */
	length = strlen(value);
	if (length == 0U || length >= size) {
		(void)snprintf(name, size, "?");
		return;
	}

	/* Printable characters without ':'. */
	for (i = 0; i < length; i++) {
		/* One character. */
		if (value[i] <= ' ' ||
		    value[i] == ':' ||
		    value[i] == 0x7f) {
			(void)snprintf(name, size, "?");
			return;
		}
	}

	/* Succeeded: the name, kept. */
	memcpy(name, value, length + 1U);
}

/*
 * Checks a caller who is not root: an account whose name fits, in wheel
 * (otherwise its password is not even checked), whose password is the one
 * in the request (which is erased).  Copies its name into caller.  Returns
 * ADMIN_OK or the refusal, logged.
 */
static int
admin_check_caller(
	struct admin_request *request,
	uid_t real,
	char *caller,
	size_t size)
{
	char check[ACCOUNT_PASSWORD_MAX + 2U];
	char buffer[LOGIN_VERIFY_BUFFER];
	struct passwd account;
	struct passwd *found;
	size_t name_length;
	size_t password_length;
	gid_t primary;
	int wheel;
	int error;

	/* The caller's account, its name not longer than a name may be. */
	found = getpwuid(real);
	name_length = ADMIN_NAME_MAX + 1U;
	if (found != NULL)
		name_length = strlen(found->pw_name);
	if (found == NULL || name_length > ADMIN_NAME_MAX)
		return ADMIN_FAILED;

	/* Its name and primary group, kept. */
	(void)snprintf(caller, size, "%s", found->pw_name);
	primary = found->pw_gid;

	/* An administrator, or the password is not even checked. */
	wheel = account_in_wheel(caller, primary);
	if (!wheel) {
		syslog(LOG_NOTICE, "refused: %s is not an administrator", caller);
		return ADMIN_NOT_ADMINISTRATOR;
	}

	/* The caller's password (login_verify erases its copy); a wrong one costs two seconds. */
	(void)snprintf(check, sizeof(check), "%s", request->password);
	password_length = strlen(request->password);
	admin_wipe(request->password, password_length);
	error = login_verify(caller, check, &account, buffer, sizeof(buffer));
	admin_wipe(check, sizeof(check));
	admin_wipe(buffer, sizeof(buffer));
	if (error != 0) {
		syslog(LOG_NOTICE, "refused: a wrong password for %s", caller);
		(void)sleep(ADMIN_WRONG_PAUSE);
		return ADMIN_BAD_PASSWORD;
	}

	/* Succeeded: an administrator who knows its password. */
	return ADMIN_OK;
}

/*
 * Reads standard input to its end into a buffer, a NUL after it.  Returns
 * 0, or EFBIG when it holds more than ADMIN_REQUEST_MAX bytes, or an errno
 * value.
 */
static int
admin_read_request(
	char *text,
	size_t capacity,
	size_t *length)
{
	ssize_t got;
	size_t used;

	/* All of it, one byte more than taken at most. */
	used = 0;
	for (;;) {
		got = read(STDIN_FILENO, text + used, capacity - 1U - used);

		/* Interrupted: again. */
		if (got < 0 && errno == EINTR)
			continue;

		/* An error, or the end. */
		if (got < 0)
			return errno;
		if (got == 0)
			break;

		/* More than a request may be. */
		used += (size_t)got;
		if (used > ADMIN_REQUEST_MAX)
			return EFBIG;
	}

	/* Succeeded: the text, ended. */
	text[used] = '\0';
	*length = used;
	return 0;
}

/*
 * Carries out a checked caller's request: the account files changed under
 * their lock, then what may take long after it.  Returns ADMIN_OK or a
 * reason.
 */
static int
admin_apply(
	const struct admin_request *request,
	const char *caller)
{
	struct admin_files files;
	struct admin_after after;
	const char *message;
	int reason;
	int error;

	/* The system's language touches no account file, so it takes no lock. */
	if (request->operation == ADMIN_SYSTEM_LANGUAGE) {
		reason = admin_system_language(request);
		return reason;
	}

	/* Nothing to do after the lock yet. */
	memset(&after, 0, sizeof(after));
	after.uid = -1;
	after.gid = -1;

	/* The lock. */
	error = account_files_lock();
	if (error != 0) {
		message = strerror(error);
		syslog(LOG_ERR, "the account files could not be locked: %s", message);
		return ADMIN_FAILED;
	}

	/* The three files. */
	error = admin_files_open(&files);
	if (error != 0) {
		admin_files_close(&files);
		account_files_unlock();
		return ADMIN_FAILED;
	}

	/* The operation. */
	switch (request->operation) {
	case ADMIN_ADD:
	case ADMIN_USER_ADD:
		reason = admin_add(&files, request, &after);
		break;
	case ADMIN_REMOVE:
	case ADMIN_USER_REMOVE:
		reason = admin_remove(&files, request, caller, &after);
		break;
	case ADMIN_RESET_PASSWORD:
		reason = admin_reset(&files, request);
		break;
	case ADMIN_GROUP_ADD:
	case ADMIN_GROUP_REMOVE:
		reason = admin_group(&files, request, caller);
		break;
	case ADMIN_USER_MODIFY:
		reason = admin_modify(&files, request);
		break;
	default:
		reason = ADMIN_BAD_REQUEST;
		break;
	}

	/* The copies go (the shadow's hold every hash), then the lock. */
	admin_files_close(&files);
	account_files_unlock();

	/* A refusal or a failure leaves nothing to do after. */
	if (reason != ADMIN_OK)
		return reason;

	/* The home and the forgetting, outside the lock. */
	reason = admin_after_work(&after);
	if (reason != ADMIN_OK)
		return reason;

	/* Succeeded: the request is carried out. */
	return ADMIN_OK;
}

/*
 * Reads the three account files and makes the buffers of their new texts,
 * each a copy of the file.  Returns 0 or an errno value (the caller closes
 * in either case).
 */
static int
admin_files_open(
	struct admin_files *files)
{
	struct admin_text *text;
	const char *message;
	int which;
	int error;

	/* The files' paths and modes, and today. */
	memset(files, 0, sizeof(files[0]));
	files->texts[ADMIN_FILE_PASSWD].path = ACCOUNT_PASSWD_FILE;
	files->texts[ADMIN_FILE_PASSWD].mode = 0644;
	files->texts[ADMIN_FILE_GROUP].path = ACCOUNT_GROUP_FILE;
	files->texts[ADMIN_FILE_GROUP].mode = 0644;
	files->texts[ADMIN_FILE_SHADOW].path = ACCOUNT_SHADOW_PATH;
	files->texts[ADMIN_FILE_SHADOW].mode = 0400;
	files->day = (long)(time(NULL) / 86400);

	/* The scratch buffer the edits go through. */
	files->scratch = malloc(ADMIN_OUTPUT_MAX);
	if (files->scratch == NULL)
		return ENOMEM;

	/* Each file: read, and copied as its new text. */
	for (which = 0; which < ADMIN_FILE_COUNT; which++) {
		/* The buffer of the file as it was read. */
		text = &files->texts[which];
		text->original = malloc(ADMIN_FILE_MAX);
		if (text->original == NULL)
			return ENOMEM;

		/* The buffer of its new text. */
		text->text = malloc(ADMIN_OUTPUT_MAX);
		if (text->text == NULL)
			return ENOMEM;

		/* The file. */
		error = account_file_read(text->path, text->original, ADMIN_FILE_MAX, &text->original_length);
		if (error != 0) {
			message = strerror(error);
			syslog(LOG_ERR, "%s could not be read: %s", text->path, message);
			return error;
		}

		/* Unchanged so far. */
		memcpy(text->text, text->original, text->original_length);
		text->length = text->original_length;
	}

	/* Succeeded: the files are read. */
	return 0;
}

/*
 * Frees the account files' texts, wiped (the shadow's and the edits hold
 * hashes).
 */
static void
admin_files_close(
	struct admin_files *files)
{
	struct admin_text *text;
	int which;

	/* Each file's two texts. */
	for (which = 0; which < ADMIN_FILE_COUNT; which++) {
		/* The text as it was read. */
		text = &files->texts[which];
		if (text->original != NULL) {
			admin_wipe(text->original, ADMIN_FILE_MAX);
			free(text->original);
			text->original = NULL;
		}

		/* The new text. */
		if (text->text != NULL) {
			admin_wipe(text->text, ADMIN_OUTPUT_MAX);
			free(text->text);
			text->text = NULL;
		}
	}

	/* The scratch buffer. */
	if (files->scratch != NULL) {
		admin_wipe(files->scratch, ADMIN_OUTPUT_MAX);
		free(files->scratch);
		files->scratch = NULL;
	}
}

/*
 * Makes the scratch buffer, which an edit has just written, a file's new
 * text; its former new text becomes the scratch buffer.
 */
static void
admin_take(
	struct admin_files *files,
	enum admin_file which,
	size_t written)
{
	struct admin_text *text;
	char *former;

	/* The two buffers change places. */
	text = &files->texts[which];
	former = text->text;
	text->text = files->scratch;
	text->length = written;
	files->scratch = former;
}

/*
 * Finds a user in the new passwd: its IDs and home.  Returns 1 with it, 0
 * when there is no such user.
 */
static int
admin_find_user(
	const struct admin_files *files,
	const char *name,
	struct admin_user *user)
{
	const struct admin_text *passwd;
	const char *home;
	size_t home_length;
	int error;

	/* The IDs. */
	passwd = &files->texts[ADMIN_FILE_PASSWD];
	user->uid = admin_number_field(passwd->text, passwd->length, name, 2U);
	user->gid = admin_number_field(passwd->text, passwd->length, name, 3U);
	if (user->uid < 0 || user->gid < 0)
		return 0;

	/* The home, empty when it does not fit. */
	user->home[0] = '\0';
	error = admin_field(passwd->text, passwd->length, name, 5U, &home, &home_length);
	if (error == 0 && home_length < sizeof(user->home)) {
		memcpy(user->home, home, home_length);
		user->home[home_length] = '\0';
	}

	/* Found. */
	return 1;
}

/*
 * Adds a user (Settings' add, root's user-add): the values checked, the
 * next user ID never given (or root's), the private group of the same
 * number, the passwd and shadow lines and the groups asked for; the home
 * is made after the lock.
 */
static int
admin_add(
	struct admin_files *files,
	const struct admin_request *request,
	struct admin_after *after)
{
	const struct admin_text *passwd;
	const struct admin_text *group;
	char home[ADMIN_PATH_MAX];
	struct stat status;
	long floor;
	long id;
	long gid;
	int valid;
	int rule;
	int used;
	int reason;
	int error;

	/* The name by the rules. */
	valid = admin_name_valid(request->name);
	if (!valid)
		return ADMIN_BAD_NAME;

	/* Settings' display name and password by the rules. */
	if (!request->by_root) {
		/* The display name. */
		valid = admin_display_valid(request->display);
		if (!valid)
			return ADMIN_BAD_NAME;

		/* The password. */
		rule = account_password_check(request->fresh, NULL, 0);
		if (rule != ACCOUNT_PASSWORD_OK)
			return ADMIN_WEAK_PASSWORD;
	}

	/* Root's shell, a program that is there. */
	if (request->shell != NULL) {
		valid = admin_shell_there(request->shell);
		if (!valid)
			return ADMIN_BAD_VALUE;
	}

	/* Not a user's or a group's name already. */
	passwd = &files->texts[ADMIN_FILE_PASSWD];
	group = &files->texts[ADMIN_FILE_GROUP];
	id = admin_number_field(passwd->text, passwd->length, request->name, 2U);
	if (id >= 0)
		return ADMIN_NAME_TAKEN;
	id = admin_number_field(group->text, group->length, request->name, 2U);
	if (id >= 0)
		return ADMIN_NAME_TAKEN;

	/* A home already there (one kept by a removal) is not given to the new user. */
	(void)snprintf(home, sizeof(home), "%s/%s", ADMIN_HOME_BASE, request->name);
	if (request->make_home) {
		error = lstat(home, &status);
		if (error == 0 || errno != ENOENT)
			return ADMIN_HOME_EXISTS;
	}

	/* Root's user ID, when it is free as a user ID and as a group ID. */
	if (request->uid >= 0) {
		/* A user's. */
		used = admin_number_used(passwd->text, passwd->length, 2U, request->uid);
		if (used)
			return ADMIN_ID_TAKEN;

		/* A group's. */
		used = admin_number_used(group->text, group->length, 2U, request->uid);
		if (used)
			return ADMIN_ID_TAKEN;
	}

	/* The user ID: root's, or the next one never given. */
	id = request->uid;
	if (id < 0) {
		floor = admin_id_floor();
		id = admin_next_id(passwd->text, passwd->length, group->text, group->length, floor);
		if (id < 0) {
			syslog(LOG_ERR, "no user ID is left for %s", request->name);
			return ADMIN_FAILED;
		}
	}

	/* The primary group: the private one, or root's choice. */
	gid = id;
	if (request->primary != NULL) {
		reason = admin_primary_group(files, request->name, request->primary, &gid);
		if (reason != ADMIN_OK)
			return reason;

		/* The private group is made below, with the user's own number. */
		if (gid < 0)
			gid = id;
	}

	/* The lines of the three files. */
	reason = admin_add_lines(files, request, id, gid);
	if (reason != ADMIN_OK)
		return reason;

	/* An addition changes no other user's lines, but every change is held to the same rule. */
	reason = admin_keeps_administrator(files);
	if (reason != ADMIN_OK)
		return reason;

	/* The ID is recorded before anything is written, so that it is never given twice. */
	error = admin_last_id_raise(id);
	if (error != 0)
		return ADMIN_FAILED;

	/*
	 * No PIN or security key left for the name is taken (ws172-p002), and
	 * a person's account gets the first PIN, 0000, so that it can log in
	 * on a machine without a keyboard (a tablet).
	 */
	error = admin_passkey_first(request->name, id);
	if (error != 0)
		return ADMIN_FAILED;

	/* The files, group first. */
	reason = admin_commit(files, 1);
	if (reason != ADMIN_OK)
		return reason;

	/* The home, with the skeleton, after the lock. */
	(void)snprintf(after->name, sizeof(after->name), "%s", request->name);
	after->make_home = request->make_home;
	after->uid = id;
	after->gid = gid;

	/* Succeeded: the user is added. */
	return ADMIN_OK;
}

/*
 * Edits a new user's lines into the new texts: its private group, the
 * groups asked for (Settings' wheel and video, root's list), its passwd
 * line, and its shadow line (Settings' password's hash, or root's lock:
 * the password is set with passwd).  Returns ADMIN_OK or a reason.
 */
static int
admin_add_lines(
	struct admin_files *files,
	const struct admin_request *request,
	long id,
	long gid)
{
	const struct admin_text *text;
	char line[ADMIN_LINE_MAX];
	char hash[ACCOUNT_HASH_MAX];
	const char *display;
	const char *shell;
	size_t written;
	int length;
	int error;

	/* The private group, of the user's name and number. */
	text = &files->texts[ADMIN_FILE_GROUP];
	(void)snprintf(line, sizeof(line), "%s:x:%ld:", request->name, id);
	error = admin_line_append(text->text, text->length, line, files->scratch, ADMIN_OUTPUT_MAX, &written);
	if (error != 0)
		return ADMIN_FAILED;
	admin_take(files, ADMIN_FILE_GROUP, written);

	/* Settings' administrator, in wheel. */
	if (request->administrator) {
		error = admin_member_set(text->text, text->length, "wheel", request->name, 1, files->scratch, ADMIN_OUTPUT_MAX, &written);
		if (error != 0)
			return ADMIN_FAILED;
		admin_take(files, ADMIN_FILE_GROUP, written);
	}

	/* Root's groups. */
	if (request->groups != NULL) {
		error = admin_groups_join(files, request->name, request->groups);
		if (error != 0)
			return ADMIN_FAILED;
	}

	/*
	 * Settings' person, in the GPU's group so that a console login can
	 * start the desktop; root's useradd puts only the groups it lists.  A
	 * system without the group adds the user without it.
	 */
	if (!request->by_root) {
		error = admin_member_set(text->text, text->length, ADMIN_GPU_GROUP, request->name, 1, files->scratch, ADMIN_OUTPUT_MAX, &written);
		if (error != 0 && error != ENOENT)
			return ADMIN_FAILED;
		if (error == 0)
			admin_take(files, ADMIN_FILE_GROUP, written);
	}

	/* The display name and the shell: Settings', or root's with their defaults. */
	display = request->display;
	if (display == NULL)
		display = "";
	shell = request->shell;
	if (shell == NULL)
		shell = ADMIN_SHELL;

	/* The passwd line, whole. */
	length = snprintf(line, sizeof(line), "%s:x:%ld:%ld:%s:%s/%s:%s", request->name, id, gid, display, ADMIN_HOME_BASE, request->name, shell);
	if (length < 0 || (size_t)length >= sizeof(line))
		return ADMIN_BAD_VALUE;

	/* Added to passwd. */
	text = &files->texts[ADMIN_FILE_PASSWD];
	error = admin_line_append(text->text, text->length, line, files->scratch, ADMIN_OUTPUT_MAX, &written);
	if (error != 0)
		return ADMIN_FAILED;
	admin_take(files, ADMIN_FILE_PASSWD, written);

	/* The shadow's password: Settings' hash, or root's lock. */
	(void)snprintf(hash, sizeof(hash), "!");
	if (!request->by_root) {
		error = account_password_hash(request->fresh, hash, sizeof(hash));
		if (error != 0)
			return ADMIN_FAILED;
	}

	/* The shadow line: the password, today, and the usual ageing. */
	(void)snprintf(line, sizeof(line), "%s:%s:%ld:0:99999:7:::", request->name, hash, files->day);
	admin_wipe(hash, sizeof(hash));
	text = &files->texts[ADMIN_FILE_SHADOW];
	error = admin_line_append(text->text, text->length, line, files->scratch, ADMIN_OUTPUT_MAX, &written);
	admin_wipe(line, sizeof(line));
	if (error != 0)
		return ADMIN_FAILED;
	admin_take(files, ADMIN_FILE_SHADOW, written);

	/* Succeeded: the new texts hold the user. */
	return ADMIN_OK;
}

/*
 * Removes a user (Settings' remove, root's user-remove): the target
 * checked, its lines taken out of shadow and passwd, its own group and its
 * name out of every group; its home, jobs and automatic login are dealt
 * with after the lock.  A name without a passwd line has what is left of
 * it cleaned.
 */
static int
admin_remove(
	struct admin_files *files,
	const struct admin_request *request,
	const char *caller,
	struct admin_after *after)
{
	const struct admin_text *text;
	struct admin_user user;
	char home[ADMIN_PATH_MAX];
	size_t written;
	long group_id;
	int removable;
	int changed;
	int found;
	int same;
	int busy;
	int reason;
	int error;

	/* No passwd line: what is left of the name. */
	found = admin_find_user(files, request->name, &user);
	if (!found) {
		reason = admin_remove_rest(files, request->name, after);
		return reason;
	}

	/* A person's account. */
	if (user.uid < ADMIN_ID_FIRST || user.uid > ADMIN_ID_LAST)
		return ADMIN_ROOT;

	/* Settings' administrator does not remove itself (root's front end has no self). */
	if (!request->by_root) {
		same = strcmp(request->name, caller);
		if (same == 0)
			return ADMIN_SELF;
	}

	/* shadow (an account without a line there is removed all the same). */
	text = &files->texts[ADMIN_FILE_SHADOW];
	error = admin_line_remove(text->text, text->length, request->name, files->scratch, ADMIN_OUTPUT_MAX, &written);
	if (error != 0 && error != ENOENT)
		return ADMIN_FAILED;
	if (error == 0)
		admin_take(files, ADMIN_FILE_SHADOW, written);

	/* passwd. */
	text = &files->texts[ADMIN_FILE_PASSWD];
	error = admin_line_remove(text->text, text->length, request->name, files->scratch, ADMIN_OUTPUT_MAX, &written);
	if (error != 0)
		return ADMIN_FAILED;
	admin_take(files, ADMIN_FILE_PASSWD, written);

	/* Its own group, unless somebody else's primary group or a group with other members. */
	text = &files->texts[ADMIN_FILE_GROUP];
	removable = admin_group_removable(text->text, text->length, files->texts[ADMIN_FILE_PASSWD].text, files->texts[ADMIN_FILE_PASSWD].length, request->name, user.uid, user.gid);
	if (removable) {
		error = admin_line_remove(text->text, text->length, request->name, files->scratch, ADMIN_OUTPUT_MAX, &written);
		if (error != 0)
			return ADMIN_FAILED;
		admin_take(files, ADMIN_FILE_GROUP, written);
	}

	/* A group of the name that stays is said in the log. */
	group_id = admin_number_field(text->text, text->length, request->name, 2U);
	if (!removable && group_id >= 0)
		syslog(LOG_NOTICE, "the group %s is kept: another user's primary group, or not the user's own", request->name);

	/* Its name out of every group's list. */
	error = admin_member_forget(text->text, text->length, request->name, files->scratch, ADMIN_OUTPUT_MAX, &written);
	if (error != 0)
		return ADMIN_FAILED;
	admin_take(files, ADMIN_FILE_GROUP, written);

	/* Not the last administrator who can log in. */
	reason = admin_keeps_administrator(files);
	if (reason != ADMIN_OK)
		return reason;

	/* Nobody logged in or running as it, unless root forces the removal. */
	if (!request->force) {
		busy = admin_busy(request->name, user.uid);
		if (busy)
			return ADMIN_BUSY;
	}

	/* Its PIN and security keys first (ws172-p002). */
	error = admin_passkey_forget(request->name, &changed);
	if (error != 0)
		return ADMIN_FAILED;

	/* The files, shadow first. */
	reason = admin_commit(files, 0);
	if (reason != ADMIN_OK)
		return reason;

	/* After the lock: its jobs and automatic login, and its home when asked. */
	(void)snprintf(after->name, sizeof(after->name), "%s", request->name);
	after->forget = 1;
	after->uid = user.uid;
	after->gid = user.gid;

	/* The home only when it is the one under the homes' directory by its name. */
	(void)snprintf(home, sizeof(home), "%s/%s", ADMIN_HOME_BASE, request->name);
	same = strcmp(home, user.home);
	if (request->remove_home && same == 0)
		after->remove_home = 1;
	if (request->remove_home && same != 0)
		syslog(LOG_NOTICE, "the home of %s is kept: it is not %s", request->name, home);

	/* Succeeded: the user is removed. */
	return ADMIN_OK;
}

/*
 * Cleans what a failed change left of a name without a passwd line: its
 * shadow line, its own group, its name in the groups' lists, and its lines
 * in /etc/passkey.  Returns ADMIN_OK, or ADMIN_NO_SUCH_USER when nothing
 * of it is there.
 */
static int
admin_remove_rest(
	struct admin_files *files,
	const char *name,
	struct admin_after *after)
{
	const struct admin_text *text;
	size_t written;
	int removable;
	int changed;
	int differs;
	int valid;
	int rest;
	int reason;
	int error;

	/* Only a name a person may have. */
	valid = admin_name_valid(name);
	if (!valid)
		return ADMIN_NO_SUCH_USER;

	/* Its shadow line. */
	rest = 0;
	text = &files->texts[ADMIN_FILE_SHADOW];
	error = admin_line_remove(text->text, text->length, name, files->scratch, ADMIN_OUTPUT_MAX, &written);
	if (error != 0 && error != ENOENT)
		return ADMIN_FAILED;
	if (error == 0) {
		admin_take(files, ADMIN_FILE_SHADOW, written);
		rest = 1;
	}

	/* Its own group. */
	text = &files->texts[ADMIN_FILE_GROUP];
	removable = admin_group_removable(text->text, text->length, files->texts[ADMIN_FILE_PASSWD].text, files->texts[ADMIN_FILE_PASSWD].length, name, -1, -1);
	if (removable) {
		error = admin_line_remove(text->text, text->length, name, files->scratch, ADMIN_OUTPUT_MAX, &written);
		if (error != 0)
			return ADMIN_FAILED;
		admin_take(files, ADMIN_FILE_GROUP, written);
		rest = 1;
	}

	/* Its name in the groups' lists. */
	error = admin_member_forget(text->text, text->length, name, files->scratch, ADMIN_OUTPUT_MAX, &written);
	if (error != 0)
		return ADMIN_FAILED;
	differs = 1;
	if (written == text->length)
		differs = memcmp(text->text, files->scratch, written);
	if (differs != 0) {
		admin_take(files, ADMIN_FILE_GROUP, written);
		rest = 1;
	}

	/* Its PIN and security keys. */
	error = admin_passkey_forget(name, &changed);
	if (error != 0)
		return ADMIN_FAILED;
	if (changed)
		rest = 1;

	/* Nothing of the name. */
	if (!rest)
		return ADMIN_NO_SUCH_USER;

	/* The files, shadow first. */
	reason = admin_commit(files, 0);
	if (reason != ADMIN_OK)
		return reason;

	/* Its automatic login after the lock (its user ID, and so its jobs, are not known). */
	(void)snprintf(after->name, sizeof(after->name), "%s", name);
	after->forget = 1;
	syslog(LOG_NOTICE, "what was left of %s without a passwd line is cleaned", name);

	/* Succeeded: nothing of the name is left. */
	return ADMIN_OK;
}

/*
 * Sets another person's password (Settings): the target checked, the
 * password by the rules, its hash into shadow (a line added when it has
 * none), and the user's PIN and security keys forgotten.
 */
static int
admin_reset(
	struct admin_files *files,
	const struct admin_request *request)
{
	const struct admin_text *text;
	struct admin_user user;
	char hash[ACCOUNT_HASH_MAX];
	char line[ADMIN_LINE_MAX];
	size_t written;
	int changed;
	int found;
	int rule;
	int reason;
	int error;

	/* A person's account. */
	found = admin_find_user(files, request->name, &user);
	if (!found)
		return ADMIN_NO_SUCH_USER;
	if (user.uid < ADMIN_ID_FIRST || user.uid > ADMIN_ID_LAST)
		return ADMIN_ROOT;

	/* The password by the rules. */
	rule = account_password_check(request->fresh, NULL, 0);
	if (rule != ACCOUNT_PASSWORD_OK)
		return ADMIN_WEAK_PASSWORD;

	/* Its hash. */
	error = account_password_hash(request->fresh, hash, sizeof(hash));
	if (error != 0)
		return ADMIN_FAILED;

	/* The hash in the user's line, or a new line. */
	text = &files->texts[ADMIN_FILE_SHADOW];
	error = account_shadow_replace(text->text, text->length, request->name, hash, files->day, files->scratch, ADMIN_OUTPUT_MAX, &written);
	if (error == ENOENT) {
		(void)snprintf(line, sizeof(line), "%s:%s:%ld:0:99999:7:::", request->name, hash, files->day);
		error = admin_line_append(text->text, text->length, line, files->scratch, ADMIN_OUTPUT_MAX, &written);
		admin_wipe(line, sizeof(line));
	}

	/* The new shadow. */
	admin_wipe(hash, sizeof(hash));
	if (error != 0)
		return ADMIN_FAILED;
	admin_take(files, ADMIN_FILE_SHADOW, written);

	/* The user's PIN and security keys go with the old password (ws172-p002). */
	error = admin_passkey_forget(request->name, &changed);
	if (error != 0)
		return ADMIN_FAILED;

	/* The file. */
	reason = admin_commit(files, 0);
	if (reason != ADMIN_OK)
		return reason;

	/* Succeeded: the password is set. */
	return ADMIN_OK;
}

/*
 * Adds a person to wheel or network, or takes it out (Settings): the
 * target checked (not oneself out of wheel), the group's list of members
 * changed; no primary group is ever changed here.
 */
static int
admin_group(
	struct admin_files *files,
	const struct admin_request *request,
	const char *caller)
{
	const struct admin_text *text;
	struct admin_user user;
	size_t written;
	int member;
	int found;
	int wheel;
	int same;
	int reason;
	int error;

	/* A person's account. */
	found = admin_find_user(files, request->name, &user);
	if (!found)
		return ADMIN_NO_SUCH_USER;
	if (user.uid < ADMIN_ID_FIRST || user.uid > ADMIN_ID_LAST)
		return ADMIN_ROOT;

	/* Out of wheel: not oneself. */
	member = 0;
	if (request->operation == ADMIN_GROUP_ADD)
		member = 1;
	wheel = strcmp(request->group, "wheel");
	same = strcmp(request->name, caller);
	if (!member &&
	    wheel == 0 &&
	    same == 0)
		return ADMIN_SELF;

	/* The group's list of members. */
	text = &files->texts[ADMIN_FILE_GROUP];
	error = admin_member_set(text->text, text->length, request->group, request->name, member, files->scratch, ADMIN_OUTPUT_MAX, &written);
	if (error != 0)
		return ADMIN_FAILED;
	admin_take(files, ADMIN_FILE_GROUP, written);

	/* Not the last administrator who can log in. */
	reason = admin_keeps_administrator(files);
	if (reason != ADMIN_OK)
		return reason;

	/* The file. */
	reason = admin_commit(files, 0);
	if (reason != ADMIN_OK)
		return reason;

	/* Succeeded: the membership is changed. */
	return ADMIN_OK;
}

/*
 * Changes a user for root's user-modify: its display name, shell, primary
 * group, groups, and the lock of its password, each only when asked.
 */
static int
admin_modify(
	struct admin_files *files,
	const struct admin_request *request)
{
	const struct admin_text *text;
	struct admin_user user;
	char number[24];
	size_t written;
	long gid;
	int found;
	int there;
	int reason;
	int error;

	/* A person's account. */
	found = admin_find_user(files, request->name, &user);
	if (!found)
		return ADMIN_NO_SUCH_USER;
	if (user.uid < ADMIN_ID_FIRST || user.uid > ADMIN_ID_LAST)
		return ADMIN_ROOT;

	/* The display name, passwd's fifth field. */
	text = &files->texts[ADMIN_FILE_PASSWD];
	if (request->display != NULL) {
		error = admin_field_set(text->text, text->length, request->name, 4U, request->display, files->scratch, ADMIN_OUTPUT_MAX, &written);
		if (error != 0)
			return ADMIN_FAILED;
		admin_take(files, ADMIN_FILE_PASSWD, written);
	}

	/* The shell, a program that is there, passwd's seventh field. */
	if (request->shell != NULL) {
		/* There. */
		there = admin_shell_there(request->shell);
		if (!there)
			return ADMIN_BAD_VALUE;

		/* Set. */
		error = admin_field_set(text->text, text->length, request->name, 6U, request->shell, files->scratch, ADMIN_OUTPUT_MAX, &written);
		if (error != 0)
			return ADMIN_FAILED;
		admin_take(files, ADMIN_FILE_PASSWD, written);
	}

	/* The primary group, passwd's fourth field. */
	if (request->primary != NULL) {
		/* Its ID. */
		reason = admin_primary_group(files, request->name, request->primary, &gid);
		if (reason != ADMIN_OK)
			return reason;
		if (gid < 0)
			return ADMIN_BAD_VALUE;

		/* Set. */
		(void)snprintf(number, sizeof(number), "%ld", gid);
		error = admin_field_set(text->text, text->length, request->name, 3U, number, files->scratch, ADMIN_OUTPUT_MAX, &written);
		if (error != 0)
			return ADMIN_FAILED;
		admin_take(files, ADMIN_FILE_PASSWD, written);
	}

	/* The groups that replace the user's: out of every list first. */
	text = &files->texts[ADMIN_FILE_GROUP];
	if (request->groups != NULL && !request->groups_append) {
		error = admin_member_forget(text->text, text->length, request->name, files->scratch, ADMIN_OUTPUT_MAX, &written);
		if (error != 0)
			return ADMIN_FAILED;
		admin_take(files, ADMIN_FILE_GROUP, written);
	}

	/* Then into the groups listed. */
	if (request->groups != NULL) {
		error = admin_groups_join(files, request->name, request->groups);
		if (error != 0)
			return ADMIN_FAILED;
	}

	/* The lock of the password. */
	if (request->lock != ADMIN_LOCK_KEEP) {
		reason = admin_modify_lock(files, request->name, request->lock);
		if (reason != ADMIN_OK)
			return reason;
	}

	/* Not the last administrator who can log in. */
	reason = admin_keeps_administrator(files);
	if (reason != ADMIN_OK)
		return reason;

	/* The files, shadow first. */
	reason = admin_commit(files, 0);
	if (reason != ADMIN_OK)
		return reason;

	/* Succeeded: the user is changed. */
	return ADMIN_OK;
}

/*
 * Locks a user's password in the new shadow, or takes the lock off when a
 * password that can be typed is behind it.  Returns ADMIN_OK,
 * ADMIN_NO_PASSWORD, or ADMIN_FAILED.
 */
static int
admin_modify_lock(
	struct admin_files *files,
	const char *name,
	enum admin_lock lock)
{
	const struct admin_text *text;
	size_t written;
	int error;

	/* The shadow with the hash locked or not. */
	text = &files->texts[ADMIN_FILE_SHADOW];
	error = admin_lock_hash(text->text, text->length, name, lock, files->scratch, ADMIN_OUTPUT_MAX, &written);

	/* No shadow line: the user cannot log in already, so a lock changes nothing and an unlock has no password. */
	if (error == ENOENT) {
		if (lock == ADMIN_LOCK_SET)
			return ADMIN_OK;
		return ADMIN_NO_PASSWORD;
	}

	/* Nothing that can be typed behind the lock (an empty hash would take an empty password). */
	if (error == EACCES)
		return ADMIN_NO_PASSWORD;
	if (error != 0)
		return ADMIN_FAILED;

	/* The new shadow. */
	admin_take(files, ADMIN_FILE_SHADOW, written);

	/* Succeeded: the password is locked or not. */
	return ADMIN_OK;
}

/*
 * Finds the ID of a primary group root asks for: wheel's or network's, or
 * the user's own group's (a person's number).  The user's own group of a
 * user being added is not there yet: its ID is then -1, for the caller to
 * use the user's.  Returns ADMIN_OK, or ADMIN_BAD_VALUE for a group that is
 * not there or not a person's.
 */
static int
admin_primary_group(
	const struct admin_files *files,
	const char *name,
	const char *group,
	long *gid)
{
	const struct admin_text *text;
	int own;

	/* The group's ID. */
	text = &files->texts[ADMIN_FILE_GROUP];
	*gid = admin_number_field(text->text, text->length, group, 2U);

	/* wheel or network: any ID they have. */
	own = strcmp(group, name);
	if (own != 0) {
		if (*gid < 0)
			return ADMIN_BAD_VALUE;
		return ADMIN_OK;
	}

	/* The user's own group: not there yet, or a person's number. */
	if (*gid < 0)
		return ADMIN_OK;
	if (*gid < ADMIN_ID_FIRST || *gid > ADMIN_ID_LAST)
		return ADMIN_BAD_VALUE;

	/* Succeeded: the user's own group. */
	return ADMIN_OK;
}

/*
 * Makes a user a member of each group of a list (split by ',', already
 * checked to be wheel or network) in the new group text.  Returns 0 or an
 * errno value.
 */
static int
admin_groups_join(
	struct admin_files *files,
	const char *name,
	const char *groups)
{
	const struct admin_text *text;
	const char *comma;
	char group[16];
	size_t length;
	size_t at;
	size_t item;
	size_t written;
	int error;

	/* Each group of the list. */
	text = &files->texts[ADMIN_FILE_GROUP];
	length = strlen(groups);
	at = 0;
	while (at < length) {
		/* One group, to the next ',' or the list's end. */
		comma = memchr(groups + at, ',', length - at);
		item = length - at;
		if (comma != NULL)
			item = (size_t)(comma - (groups + at));
		if (item >= sizeof(group))
			return EINVAL;
		memcpy(group, groups + at, item);
		group[item] = '\0';

		/* The user in its list. */
		error = admin_member_set(text->text, text->length, group, name, 1, files->scratch, ADMIN_OUTPUT_MAX, &written);
		if (error != 0)
			return error;
		admin_take(files, ADMIN_FILE_GROUP, written);

		/* The next. */
		at += item + 1U;
	}

	/* Succeeded: the user is in every group listed. */
	return 0;
}

/*
 * Refuses a change that leaves no administrator who can log in where the
 * files had one: a removal, a lock, a shell that does not log in, a
 * primary group or a list of groups without wheel, Settings' wheel
 * removal.  A machine that had none already is not refused (the change
 * cannot make it worse).
 */
static int
admin_keeps_administrator(
	const struct admin_files *files)
{
	const struct admin_text *passwd;
	const struct admin_text *group;
	const struct admin_text *shadow;
	size_t before;
	size_t after;

	/* The administrators who can log in, as the files are. */
	passwd = &files->texts[ADMIN_FILE_PASSWD];
	group = &files->texts[ADMIN_FILE_GROUP];
	shadow = &files->texts[ADMIN_FILE_SHADOW];
	before = admin_administrators(passwd->original, passwd->original_length, group->original, group->original_length, shadow->original, shadow->original_length, files->day);

	/* And as they would be. */
	after = admin_administrators(passwd->text, passwd->length, group->text, group->length, shadow->text, shadow->length, files->day);

	/* The last one would go. */
	if (before > 0U && after == 0U)
		return ADMIN_LAST_ADMINISTRATOR;

	/* Succeeded: an administrator is kept, or there was none. */
	return ADMIN_OK;
}

/*
 * Writes each account file whose text changed, in the order of an
 * addition (group, passwd, shadow) or of any other change (shadow, passwd,
 * group), so that a crash between two never leaves an account that logs
 * in half made.  When one cannot be written, the ones written already get
 * their old texts back.  Returns ADMIN_OK or ADMIN_FAILED.
 */
static int
admin_commit(
	struct admin_files *files,
	int adding)
{
	enum admin_file order[ADMIN_FILE_COUNT];
	int written[ADMIN_FILE_COUNT];
	struct admin_text *text;
	int differs;
	int error;
	int i;
	int back;

	/* The order. */
	order[0] = ADMIN_FILE_SHADOW;
	order[1] = ADMIN_FILE_PASSWD;
	order[2] = ADMIN_FILE_GROUP;
	if (adding) {
		order[0] = ADMIN_FILE_GROUP;
		order[2] = ADMIN_FILE_SHADOW;
	}

	/* Each file in turn. */
	for (i = 0; i < ADMIN_FILE_COUNT; i++) {
		/* An unchanged file is not written. */
		written[i] = 0;
		text = &files->texts[order[i]];
		differs = 1;
		if (text->length == text->original_length)
			differs = memcmp(text->text, text->original, text->length);
		if (differs == 0)
			continue;

		/* The new text. */
		error = admin_write(text->path, text->mode, text->text, text->length);
		if (error == 0) {
			written[i] = 1;
			continue;
		}

		/* A failure: the files written already, back to their old texts, the last first. */
		for (back = i - 1; back >= 0; back--) {
			/* One written. */
			if (!written[back])
				continue;
			text = &files->texts[order[back]];
			error = admin_write(text->path, text->mode, text->original, text->original_length);
			if (error == 0)
				syslog(LOG_NOTICE, "%s is put back as it was", text->path);
		}

		/* The change is not made. */
		return ADMIN_FAILED;
	}

	/* Succeeded: every changed file is written. */
	return ADMIN_OK;
}

/*
 * Replaces an account file, saying a failure in the log.  Returns 0 or an
 * errno value.
 */
static int
admin_write(
	const char *path,
	mode_t mode,
	const char *text,
	size_t length)
{
	const char *message;
	int error;

	/* The atomic replacement. */
	error = account_file_write(path, mode, text, length);
	if (error != 0) {
		message = strerror(error);
		syslog(LOG_ERR, "%s could not be replaced: %s", path, message);
		return error;
	}

	/* Succeeded: the file is the new one. */
	return 0;
}

/*
 * Reports the highest person's user ID that something outside the account
 * files still shows was given: the record of the last ID given, the
 * owners of the homes, cron's spool by its names and at's by its owners.
 * A new user's ID is above it.
 */
static long
admin_id_floor(void)
{
	char text[ADMIN_SMALL_MAX];
	size_t length;
	char *end;
	long highest;
	long recorded;
	int error;

	/* The record, when there is one that is a number. */
	highest = 0;
	error = account_file_read(ADMIN_LAST_ID_FILE, text, sizeof(text) - 1U, &length);
	if (error == 0) {
		text[length] = '\0';
		recorded = strtol(text, &end, 10);
		if (end != text && recorded > highest)
			highest = recorded;
	}

	/* The homes' owners. */
	admin_highest_in_directory(ADMIN_HOME_BASE, 0, &highest);

	/* cron's tables, named by their users' IDs. */
	admin_highest_in_directory(ADMIN_CRON_SPOOL, 1, &highest);

	/* at's jobs, owned by their users. */
	admin_highest_in_directory(ADMIN_AT_SPOOL, 0, &highest);

	/* The highest found. */
	return highest;
}

/*
 * Raises a highest ID to the largest person's user ID among a directory's
 * entries: their names when by_name is nonzero, their owners otherwise (no
 * link is followed).  A directory that is not there has none.
 */
static void
admin_highest_in_directory(
	const char *path,
	int by_name,
	long *highest)
{
	struct dirent *entry;
	struct stat status;
	DIR *directory;
	char *end;
	long number;
	int directory_descriptor;
	int error;
	int dot;

	/* The directory, when there is one. */
	directory = opendir(path);
	if (directory == NULL)
		return;
	directory_descriptor = dirfd(directory);

	/* Each entry. */
	for (;;) {
		/* The next entry, or the end. */
		entry = readdir(directory);
		if (entry == NULL)
			break;

		/* Not "." or "..". */
		dot = admin_dot(entry->d_name);
		if (dot)
			continue;

		/* The number: the name's, or the owner's. */
		if (by_name) {
			number = strtol(entry->d_name, &end, 10);
			if (end == entry->d_name || *end != '\0')
				continue;
		} else {
			error = fstatat(directory_descriptor, entry->d_name, &status, AT_SYMLINK_NOFOLLOW);
			if (error != 0)
				continue;
			number = (long)status.st_uid;
		}

		/* A person's ID above the highest so far. */
		if (number < ADMIN_ID_FIRST || number > ADMIN_ID_LAST)
			continue;
		if (number > *highest)
			*highest = number;
	}

	/* The directory is no longer needed. */
	(void)closedir(directory);
}

/*
 * Raises the record of the highest user ID given to a person to an ID,
 * under the lock of the account files the caller holds.  Returns 0 or an
 * errno value, said in the log.
 */
static int
admin_last_id_raise(
	long id)
{
	char text[ADMIN_SMALL_MAX];
	size_t length;
	char *end;
	long recorded;
	int written;
	int error;

	/* The record; none, or one that is not a number, holds nothing. */
	recorded = 0;
	error = account_file_read(ADMIN_LAST_ID_FILE, text, sizeof(text) - 1U, &length);
	if (error == 0) {
		text[length] = '\0';
		recorded = strtol(text, &end, 10);
		if (end == text)
			recorded = 0;
	}

	/* A higher ID is recorded already. */
	if (recorded >= id)
		return 0;

	/* The new record. */
	written = snprintf(text, sizeof(text), "%ld\n", id);
	if (written < 0 || (size_t)written >= sizeof(text))
		return ERANGE;
	error = admin_write(ADMIN_LAST_ID_FILE, 0644, text, (size_t)written);
	if (error != 0)
		return error;

	/* Succeeded: the ID is never given again. */
	return 0;
}

/*
 * Takes every line of a name out of /etc/passkey (its PIN and security
 * keys, whatever their user ID; ws172-p002), under the lock of the account
 * files the caller holds; changed says whether there was one.  Returns 0
 * (a missing file has none), or an errno value, said in the log.
 */
static int
admin_passkey_forget(
	const char *name,
	int *changed)
{
	const char *message;
	int error;

	/* The name's lines go, and none comes. */
	error = admin_passkey_lines(name, NULL, changed);

	/* A failure is said. */
	if (error != 0) {
		message = strerror(error);
		syslog(LOG_ERR, "the passkey lines of %s could not be removed: %s", name, message);
		return error;
	}

	/* Succeeded: the name has no line left. */
	return 0;
}

/*
 * Gives a new account its lines in /etc/passkey: none of the name's left
 * over, and for a person's account (a user ID from ADMIN_ID_FIRST to
 * ADMIN_ID_LAST) the first PIN, ADMIN_FIRST_PIN, hashed like a password.
 * Under the lock of the account files the caller holds.  Returns 0, or an
 * errno value, said in the log.
 */
static int
admin_passkey_first(
	const char *name,
	long uid)
{
	char hash[ACCOUNT_HASH_MAX];
	char line[ADMIN_LINE_MAX];
	const char *message;
	int changed;
	int error;

	/* A system account gets no PIN: only what was left over goes. */
	if (uid < ADMIN_ID_FIRST || uid > ADMIN_ID_LAST)
		return admin_passkey_forget(name, &changed);

	/* The PIN's line. */
	error = account_password_hash(ADMIN_FIRST_PIN, hash, sizeof(hash));
	if (error == 0) {
		(void)snprintf(line, sizeof(line), "%s:%ld:pin:%s", name, uid, hash);
		error = admin_passkey_lines(name, line, &changed);
	}
	admin_wipe(hash, sizeof(hash));
	admin_wipe(line, sizeof(line));

	/* A failure is said. */
	if (error != 0) {
		message = strerror(error);
		syslog(LOG_ERR, "the first PIN of %s could not be set: %s", name, message);
		return error;
	}

	/* Succeeded: the account has its first PIN. */
	return 0;
}

/*
 * Rewrites /etc/passkey with every line of a name taken out, and added (a
 * whole line, or NULL) put in; changed says whether the file changed.
 * Returns 0 or an errno value.
 */
static int
admin_passkey_lines(
	const char *name,
	const char *added,
	int *changed)
{
	char *text;
	char *output;
	int error;

	/* The file's text. */
	*changed = 0;
	output = NULL;
	error = ENOMEM;
	text = malloc(ADMIN_FILE_MAX);
	if (text != NULL) {
		/* Its edited copy, and the edit through both. */
		output = malloc(ADMIN_OUTPUT_MAX);
		if (output != NULL)
			error = admin_passkey_edit(name, added, text, output, changed);
	}

	/* Both wiped (they hold the PINs' hashes) and freed. */
	if (text != NULL) {
		admin_wipe(text, ADMIN_FILE_MAX);
		free(text);
	}

	/* The edited copy, the same way. */
	if (output != NULL) {
		admin_wipe(output, ADMIN_OUTPUT_MAX);
		free(output);
	}

	/* Succeeded or not. */
	return error;
}

/*
 * Rewrites /etc/passkey without the name's lines and with added (NULL for
 * none), through the two buffers (ADMIN_FILE_MAX and ADMIN_OUTPUT_MAX
 * bytes).  An unchanged file is not written, a missing one is made only
 * for a line added, and a file a later passkey wrote is not changed
 * (EROFS).
 */
static int
admin_passkey_edit(
	const char *name,
	const char *added,
	char *text,
	char *output,
	int *changed)
{
	size_t length;
	size_t written;
	int version;
	int differs;
	int error;

	/* The file; none has no line, and is made for a line added. */
	error = account_file_read(PASSKEY_FILE, text, ADMIN_FILE_MAX, &length);
	if (error == ENOENT && added == NULL)
		return 0;
	if (error == ENOENT) {
		text[0] = '\0';
		length = 0U;
		error = 0;
	}
	if (error != 0)
		return error;

	/* Only a version this tool knows is rewritten. */
	version = passkey_record_version(text, length);
	if (version > PASSKEY_VERSION)
		return EROFS;

	/* The text without the name's lines. */
	error = passkey_record_replace(text, length, name, NULL, added, output, ADMIN_OUTPUT_MAX, &written);
	if (error != 0)
		return error;

	/* Written only when it changed. */
	differs = 1;
	if (written == length)
		differs = memcmp(text, output, length);
	if (differs == 0)
		return 0;
	error = admin_write(PASSKEY_FILE, 0600, output, written);
	if (error != 0)
		return error;

	/* Succeeded: the name's lines are gone, and the one added is there. */
	*changed = 1;
	return 0;
}

/*
 * Reports whether a user is busy: a process runs with its user ID, or the
 * accounting records it logged in.
 */
static int
admin_busy(
	const char *name,
	long uid)
{
	struct process_info process;
	struct utmpx *record;
	int32_t cursor;
	unsigned count;
	int descriptor;
	int error;
	int same;
	int busy;

	/* The kernel's processes, one after another. */
	busy = 0;
	cursor = -1;
	descriptor = open("/dev/system", O_RDONLY | O_CLOEXEC);
	for (count = 0; descriptor >= 0 && count < ADMIN_PROCESSES_MAX; count++) {
		/* The next process after the last one (as ps asks for it). */
		memset(&process, 0, sizeof(process));
		process.pid = cursor;
		error = ioctl(descriptor, KERN_SYSTEM_GET_PROCESS, &process);
		if (error != 0)
			break;
		cursor = process.pid;

		/* One running as the user. */
		if ((long)process.uid == uid) {
			busy = 1;
			break;
		}
	}

	/* The list is no longer needed. */
	if (descriptor >= 0)
		(void)close(descriptor);

	/* A process of the user's. */
	if (busy)
		return 1;

	/* A login recorded for it. */
	setutxent();
	for (;;) {
		/* The next record, or the end. */
		record = getutxent();
		if (record == NULL)
			break;

		/* A login of the user's. */
		same = strncmp(record->ut_user, name, sizeof(record->ut_user));
		if (record->ut_type == USER_PROCESS && same == 0) {
			busy = 1;
			break;
		}
	}

	/* The records are closed. */
	endutxent();

	/* Busy or not. */
	return busy;
}

/*
 * Reports whether a shell root gives a user is a program that is there: a
 * regular file (a link to one is followed) that someone may run.
 */
static int
admin_shell_there(
	const char *shell)
{
	struct stat status;
	int error;

	/* The file. */
	error = stat(shell, &status);
	if (error != 0)
		return 0;

	/* A regular file. */
	if ((status.st_mode & S_IFMT) != S_IFREG)
		return 0;

	/* That can be run. */
	if ((status.st_mode & 0111) == 0)
		return 0;

	/* Succeeded: the shell is there. */
	return 1;
}

/*
 * Does what a request left for after the lock: makes a new home, removes
 * an old one, and forgets a removed user's jobs and automatic login.
 * Returns ADMIN_OK, or ADMIN_HOME_FAILED when the account was made but its
 * home could not be (said in the log).
 */
static int
admin_after_work(
	const struct admin_after *after)
{
	const char *message;
	int error;

	/* A removed user's home, when asked. */
	if (after->remove_home)
		admin_remove_home(after->name, after->uid);

	/* A removed user's jobs (by its user ID, when known) and automatic login. */
	if (after->forget) {
		if (after->uid >= 0)
			admin_spool_forget(after->uid);
		admin_autologin_forget(after->name);
	}

	/* A new user's home, with the skeleton. */
	if (after->make_home) {
		error = admin_make_home(after->name, after->uid, after->gid);
		if (error != 0) {
			message = strerror(error);
			syslog(LOG_ERR, "the home of %s could not be made: %s", after->name, message);
			return ADMIN_HOME_FAILED;
		}
	}

	/* Succeeded: nothing is left to do. */
	return ADMIN_OK;
}

/*
 * Makes a new user's home under the homes' directory: mode 0700, owned by
 * the user and its primary group, with the skeleton's files.  Returns 0 or
 * an errno value.
 */
static int
admin_make_home(
	const char *name,
	long uid,
	long gid)
{
	int base;
	int home;
	int error;

	/* The homes' directory, not followed if it is a link. */
	base = open(ADMIN_HOME_BASE, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (base < 0)
		return errno;

	/* The home, made anew (never an existing one). */
	error = mkdirat(base, name, 0700);
	if (error != 0) {
		error = errno;
		(void)close(base);
		return error;
	}

	/* Opened without following a link, and given to the user. */
	home = openat(base, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	(void)close(base);
	if (home < 0)
		return errno;
	error = fchown(home, (uid_t)uid, (gid_t)gid);
	if (error != 0) {
		error = errno;
		(void)close(home);
		return error;
	}

	/* The skeleton's files. */
	admin_copy_skeleton(home, uid, gid);
	(void)close(home);

	/* Succeeded: the home is made. */
	return 0;
}

/*
 * Copies the regular files directly in the skeleton into a new home,
 * owned by the user and its primary group, without the group's and the
 * others' permissions (no link is followed; the skeleton may be missing;
 * a directory in it is not copied).
 */
static void
admin_copy_skeleton(
	int home,
	long uid,
	long gid)
{
	struct dirent *entry;
	struct stat status;
	char block[4096];
	ssize_t got;
	ssize_t put;
	DIR *skeleton;
	int skeleton_descriptor;
	int regular;
	int source;
	int target;
	int error;
	int dot;

	/* The skeleton, when there is one. */
	skeleton = opendir(ADMIN_SKELETON);
	if (skeleton == NULL)
		return;
	skeleton_descriptor = dirfd(skeleton);

	/* Each entry that is a regular file. */
	for (;;) {
		/* The next entry, or the end. */
		entry = readdir(skeleton);
		if (entry == NULL)
			break;

		/* Not "." or "..". */
		dot = admin_dot(entry->d_name);
		if (dot)
			continue;

		/* A regular file (not followed). */
		error = fstatat(skeleton_descriptor, entry->d_name, &status, AT_SYMLINK_NOFOLLOW);
		regular = 0;
		if (error == 0 && (status.st_mode & S_IFMT) == S_IFREG)
			regular = 1;
		if (!regular)
			continue;

		/* The file and its copy, made anew in the home. */
		source = openat(skeleton_descriptor, entry->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		if (source < 0)
			continue;
		target = openat(home, entry->d_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, status.st_mode & 0700);
		if (target < 0) {
			(void)close(source);
			continue;
		}

		/* Its bytes. */
		for (;;) {
			/* A block, or the end. */
			got = read(source, block, sizeof(block));
			if (got <= 0)
				break;

			/* Written whole, or the copy stops. */
			put = write(target, block, (size_t)got);
			if (put != got)
				break;
		}

		/* Given to the user. */
		(void)fchown(target, (uid_t)uid, (gid_t)gid);
		(void)close(target);
		(void)close(source);
	}

	/* The skeleton is no longer needed. */
	(void)closedir(skeleton);
}

/*
 * Removes a removed user's home under the homes' directory by its name,
 * never following a link, only when it is a directory the user owns (a
 * directory that was there before the user stays), and never below a
 * mount point inside it.  What could not be removed is said in the log.
 */
static void
admin_remove_home(
	const char *name,
	long uid)
{
	struct stat status;
	const char *message;
	int base;
	int error;

	/* The homes' directory, not followed if it is a link. */
	base = open(ADMIN_HOME_BASE, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (base < 0) {
		message = strerror(errno);
		syslog(LOG_ERR, "the home of %s could not be removed: %s", name, message);
		return;
	}

	/* The home itself; none is removed already. */
	error = fstatat(base, name, &status, AT_SYMLINK_NOFOLLOW);
	if (error != 0) {
		(void)close(base);
		return;
	}

	/* A directory of the user's. */
	if ((status.st_mode & S_IFMT) != S_IFDIR || (long)status.st_uid != uid) {
		syslog(LOG_NOTICE, "the home of %s is kept: not a directory the user owns", name);
		(void)close(base);
		return;
	}

	/* The tree, on the home's own file system. */
	error = admin_remove_tree(base, name, 0, status.st_dev);
	if (error != 0) {
		message = strerror(error);
		syslog(LOG_ERR, "the home of %s could not be removed whole: %s", name, message);
	}

	/* The homes' directory is no longer needed. */
	(void)close(base);
}

/*
 * Removes an entry of a directory and, when it is a directory, everything
 * under it, never following a link and never entering another file system
 * than the device's (a mount point is left, said in the log).  Returns 0
 * or the first errno value.
 */
static int
admin_remove_tree(
	int parent,
	const char *name,
	int depth,
	dev_t device)
{
	struct dirent *entry;
	struct stat status;
	DIR *directory;
	int descriptor;
	int directory_descriptor;
	int error;
	int first;
	int dot;

	/* Too deep. */
	if (depth > ADMIN_TREE_DEPTH)
		return ELOOP;

	/* The entry, not followed. */
	error = fstatat(parent, name, &status, AT_SYMLINK_NOFOLLOW);
	if (error != 0)
		return errno;

	/* Another file system mounted here stays. */
	if (status.st_dev != device) {
		syslog(LOG_NOTICE, "%s is on another file system and is left", name);
		return EXDEV;
	}

	/* Not a directory (or a link to one): unlinked. */
	if ((status.st_mode & S_IFMT) != S_IFDIR) {
		error = unlinkat(parent, name, 0);
		if (error != 0)
			return errno;
		return 0;
	}

	/* A directory: opened without following a link. */
	descriptor = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return errno;
	directory = fdopendir(descriptor);
	if (directory == NULL) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Everything in it, the first error kept. */
	directory_descriptor = dirfd(directory);
	first = 0;
	for (;;) {
		/* The next entry, or the end. */
		entry = readdir(directory);
		if (entry == NULL)
			break;

		/* Not "." or "..". */
		dot = admin_dot(entry->d_name);
		if (dot)
			continue;

		/* Removed, and under it. */
		error = admin_remove_tree(directory_descriptor, entry->d_name, depth + 1, device);
		if (error != 0 && first == 0)
			first = error;
	}

	/* The directory itself, once empty. */
	(void)closedir(directory);
	if (first != 0)
		return first;
	error = unlinkat(parent, name, AT_REMOVEDIR);
	if (error != 0)
		return errno;

	/* Succeeded: removed. */
	return 0;
}

/*
 * Forgets a removed user's jobs: its cron table, named by its user ID, and
 * the at jobs it owns (regular files; no link is followed).
 */
static void
admin_spool_forget(
	long uid)
{
	struct dirent *entry;
	struct stat status;
	char path[ADMIN_PATH_MAX];
	const char *message;
	DIR *directory;
	int descriptor;
	int directory_descriptor;
	int error;
	int dot;

	/* The cron table. */
	(void)snprintf(path, sizeof(path), "%s/%ld", ADMIN_CRON_SPOOL, uid);
	error = unlink(path);
	if (error != 0 && errno != ENOENT) {
		message = strerror(errno);
		syslog(LOG_ERR, "%s could not be removed: %s", path, message);
	}

	/* at's spool, not followed if it is a link; none has no job. */
	descriptor = open(ADMIN_AT_SPOOL, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return;
	directory = fdopendir(descriptor);
	if (directory == NULL) {
		(void)close(descriptor);
		return;
	}

	/* Each job the user owns. */
	directory_descriptor = dirfd(directory);
	for (;;) {
		/* The next entry, or the end. */
		entry = readdir(directory);
		if (entry == NULL)
			break;

		/* Not "." or "..". */
		dot = admin_dot(entry->d_name);
		if (dot)
			continue;

		/* A regular file of the user's. */
		error = fstatat(directory_descriptor, entry->d_name, &status, AT_SYMLINK_NOFOLLOW);
		if (error != 0)
			continue;
		if ((status.st_mode & S_IFMT) != S_IFREG || (long)status.st_uid != uid)
			continue;

		/* Removed. */
		error = unlinkat(directory_descriptor, entry->d_name, 0);
		if (error != 0) {
			message = strerror(errno);
			syslog(LOG_ERR, "the at job %s could not be removed: %s", entry->d_name, message);
		}
	}

	/* The spool is no longer needed. */
	(void)closedir(directory);
}

/*
 * Forgets a removed user's automatic login: the file that names the
 * account sessiond logs in at boot goes when its first line names the
 * user, so that a later account of the same name is not logged in by
 * itself.
 */
static void
admin_autologin_forget(
	const char *name)
{
	char text[ADMIN_SMALL_MAX];
	size_t length;
	const char *message;
	size_t first;
	int error;
	int same;

	/* The file; none names nobody. */
	error = account_file_read(ADMIN_AUTOLOGIN_FILE, text, sizeof(text) - 1U, &length);
	if (error != 0)
		return;

	/* Its first line, without its end or blanks, as sessiond reads it. */
	text[length] = '\0';
	first = strcspn(text, "\r\n \t");
	text[first] = '\0';
	same = strcmp(text, name);
	if (same != 0)
		return;

	/* It names the user: it goes. */
	error = unlink(ADMIN_AUTOLOGIN_FILE);
	if (error != 0) {
		message = strerror(errno);
		syslog(LOG_ERR, "%s could not be removed: %s", ADMIN_AUTOLOGIN_FILE, message);
		return;
	}

	/* Succeeded: no automatic login names the user. */
	syslog(LOG_NOTICE, "the automatic login of %s is removed", name);
}

/*
 * Reports whether a directory entry is "." or "..".
 */
static int
admin_dot(
	const char *name)
{
	/* ".". */
	if (name[0] == '.' && name[1] == '\0')
		return 1;

	/* "..". */
	if (name[0] == '.' &&
	    name[1] == '.' &&
	    name[2] == '\0')
		return 1;

	/* Another name. */
	return 0;
}

/*
 * Writes the answer line on standard output.
 */
static void
admin_answer(
	int reason)
{
	const char *word;

	/* The reason's word. */
	word = admin_reason_word(reason);

	/* "ok", or "error" and the reason's word. */
	if (reason == ADMIN_OK)
		(void)printf("ok\n");
	else
		(void)printf("error %s\n", word);
	(void)fflush(stdout);
}

/*
 * Opens /dev/null on any of descriptors 0 to 2 that is closed, so that
 * nothing written later lands in a file opened there.
 */
static void
admin_standard_descriptors(void)
{
	int descriptor;
	int flags;
	int index;

	/* Each of the three. */
	for (index = 0; index < 3; index++) {
		/* Open already. */
		flags = fcntl(index, F_GETFD);
		if (flags != -1)
			continue;

		/* /dev/null in its place, or nothing can be done safely. */
		descriptor = open("/dev/null", O_RDWR);
		if (descriptor < 0)
			_exit(1);
	}
}

/*
 * Overwrites a buffer that held a secret.
 */
static void
admin_wipe(
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

/*
 * Sets the system's language, the login screen's (ws158-p004): the one
 * line of ADMIN_LANGUAGE_FILE, replaced atomically, readable by everyone.
 * The language was checked when the request was read.  Returns ADMIN_OK
 * or ADMIN_FAILED (said in the log).
 */
static int
admin_system_language(
	const struct admin_request *request)
{
	char line[16];
	const char *message;
	int written;
	int made;
	int error;

	/* The line. */
	written = snprintf(line, sizeof(line), "%s\n", request->name);
	if (written < 0 || (size_t)written >= sizeof(line))
		return ADMIN_FAILED;

	/* The directory, when the system has none yet (an existing one is kept as it is). */
	made = mkdir(ADMIN_LANGUAGE_DIRECTORY, 0755);
	if (made != 0 && errno != EEXIST) {
		message = strerror(errno);
		syslog(LOG_ERR, "%s could not be made: %s", ADMIN_LANGUAGE_DIRECTORY, message);
		return ADMIN_FAILED;
	}

	/* The file, replaced. */
	error = admin_write(ADMIN_LANGUAGE_FILE, 0644, line, (size_t)written);
	if (error != 0)
		return ADMIN_FAILED;

	/* Succeeded: the login screen shows the language from its next start. */
	return ADMIN_OK;
}
