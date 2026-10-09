/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /sbin/passkey (ws172-p002; docs/architecture/security.md, "Login
 * authentication"): checks one credential, or changes /etc/passkey, for
 * sessiond.
 *
 * It runs as root and only as root (its real user ID), with mode 0500 and
 * not set-user-ID.  It reads one request on its standard input (passkey.h,
 * request.c), does that one thing and writes the answer on its standard
 * output: "status touch" lines, then "ok uid=N ..." or "fail REASON".  Its
 * exit status is 0 for ok, 1 for fail, 2 for an internal error.  It reads
 * nothing from its command line or its environment.
 *
 * Passwords and PINs are checked here with the C library's crypt() alone;
 * the security key style is passkey-fido2's (/usr/libexec/passkey-fido2,
 * ws172-p003), which gets the same request, as do a key's own operations
 * (ws199-p001: key-info, key-set-pin, key-change-pin, key-reset,
 * key-owner).  While
 * passkey-fido2 runs, passkey ignores SIGTERM, SIGHUP and SIGPIPE and
 * waits for it: sessiond's TERM reaches passkey-fido2 and its helper in
 * the same process group, which end the key's work (a cancel the key
 * answers) before passkey-fido2 answers and exits.
 */

#include "passkey.h"

#include "userland/base/common/account.h"
#include "userland/base/login/verify.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The security key style's program, and the first user ID of a person's account. */
#define PASSKEY_FIDO2_PATH	"/usr/libexec/passkey-fido2"
#define PASSKEY_UID_FIRST	1000U

/* The largest /etc/passkey, read and written whole. */
#define PASSKEY_FILE_MAX	65536U

/* What an options line's change sets: the key's PIN and touch (set-options), or the sign-in methods (set-methods, WS200). */
#define PASSKEY_CHANGE_KEY	1
#define PASSKEY_CHANGE_METHODS	2

/* The exit statuses. */
#define PASSKEY_EXIT_OK		0
#define PASSKEY_EXIT_FAIL	1
#define PASSKEY_EXIT_INTERNAL	2

static void passkey_setup(void);
static int passkey_read(char *buffer, size_t capacity, size_t *length);
static int passkey_fail(const char *reason);
static int passkey_ok(uid_t uid, const char *extra);
static int passkey_account(const char *name, struct passwd *account, char *buffer, size_t size);
static int passkey_usable(const char *name, uid_t uid);
static int passkey_file(char *text, size_t capacity, size_t *length, int need_private);
static int passkey_auth_pin(const char *name, uid_t uid, char *pin);
static int passkey_styles(const char *name, uid_t uid, int enrolled);
static int passkey_change_pin(const char *name, uid_t uid, const char *pin);
static int passkey_set_options(const char *name, uid_t uid, const char *key_pin, const char *key_touch);
static int passkey_set_methods(const char *name, uid_t uid, const char *text);
static int passkey_options_write(const char *name, uid_t uid, int change, int key_pin, int key_touch, unsigned methods);
static int passkey_method_off(const char *name, uid_t uid, unsigned method);
static int passkey_fido2(const char *request, size_t length);
static void passkey_keys_listed(const char *text, size_t length, const char *name, uid_t uid, char *extra, size_t size);

/* Answers one request. */
int
main(
	void)
{
	struct passkey_request request;
	struct passwd account;
	char buffer[PASSKEY_REQUEST_MAX + 1U];
	char copy[PASSKEY_REQUEST_MAX + 1U];
	char strings[LOGIN_VERIFY_BUFFER];
	char *secret;
	size_t length;
	unsigned method;
	int found;
	int same;
	int off;
	int error;
	int status;

	/* Root alone, a clean environment, no core file, standard descriptors that are there. */
	if (getuid() != 0)
		return PASSKEY_EXIT_INTERNAL;
	passkey_setup();

	/* The request. */
	error = passkey_read(buffer, sizeof(buffer), &length);
	if (error != 0)
		return passkey_fail("bad-request");
	memcpy(copy, buffer, length);
	error = passkey_request_parse(buffer, length, &request);
	if (error != 0) {
		passkey_wipe(buffer, sizeof(buffer));
		passkey_wipe(copy, sizeof(copy));
		return passkey_fail("bad-request");
	}

	/* The security key style goes to passkey-fido2 with the whole request. */
	same = request.operation == PASSKEY_OP_ENROLL_FIDO2 || request.operation == PASSKEY_OP_REMOVE_FIDO2;
	if (request.operation >= PASSKEY_OP_KEY_INFO && request.operation <= PASSKEY_OP_KEY_RESET)
		same = 1;
	if (request.operation == PASSKEY_OP_AUTH_FIDO2 || request.operation == PASSKEY_OP_KEY_OWNER)
		same = 1;
	if (request.operation == PASSKEY_OP_AUTH && strcmp(request.fields[2], "fido2") == 0)
		same = 1;
	if (same) {
		passkey_wipe(buffer, sizeof(buffer));
		status = passkey_fido2(copy, length);
		passkey_wipe(copy, sizeof(copy));
		return status;
	}
	passkey_wipe(copy, sizeof(copy));

	/* The account. */
	found = passkey_account(request.fields[1], &account, strings, sizeof(strings));
	if (!found) {
		passkey_wipe(buffer, sizeof(buffer));
		return passkey_fail("no-such-user");
	}

	/* Each operation. */
	switch (request.operation) {
	case PASSKEY_OP_STYLES:
		status = passkey_styles(request.fields[1], account.pw_uid, 0);
		break;
	case PASSKEY_OP_ENROLLED:
		status = passkey_styles(request.fields[1], account.pw_uid, 1);
		break;
	case PASSKEY_OP_AUTH:
		/* A method the account turned off is refused before its secret is looked at (WS200). */
		secret = request.fields[3];
		method = PASSKEY_METHOD_PASSWORD;
		same = strcmp(request.fields[2], "pin");
		if (same == 0)
			method = PASSKEY_METHOD_PIN;
		off = passkey_method_off(request.fields[1], account.pw_uid, method);
		if (off) {
			passkey_wipe(secret, strlen(secret));
			status = passkey_fail("style-off");
			break;
		}

		/* The password, or the PIN. */
		if (strcmp(request.fields[2], "password") == 0) {
			error = login_verify(request.fields[1], secret, &account, strings, sizeof(strings));
			status = error == 0 ? passkey_ok(account.pw_uid, NULL) : passkey_fail("bad-secret");
		} else if (strcmp(request.fields[2], "pin") == 0) {
			status = passkey_auth_pin(request.fields[1], account.pw_uid, secret);
		} else {
			status = passkey_fail("bad-request");
		}
		break;
	case PASSKEY_OP_ENROLL_PIN:
	case PASSKEY_OP_REMOVE_PIN:
		/* The user's current password first; the PIN's rules for a new one. */
		error = login_verify(request.fields[1], request.fields[2], &account, strings, sizeof(strings));
		if (error != 0) {
			status = passkey_fail("bad-secret");
			break;
		}
		if (request.operation == PASSKEY_OP_ENROLL_PIN) {
			if (!passkey_is_pin(request.fields[3])) {
				status = passkey_fail("bad-request");
				break;
			}
			status = passkey_change_pin(request.fields[1], account.pw_uid, request.fields[3]);
		} else {
			status = passkey_change_pin(request.fields[1], account.pw_uid, NULL);
		}
		break;
	case PASSKEY_OP_SET_OPTIONS:
		/* The user's password, then the key's PIN and touch for signing in (ws199-p001). */
		error = login_verify(request.fields[1], request.fields[2], &account, strings, sizeof(strings));
		if (error != 0) {
			status = passkey_fail("bad-secret");
			break;
		}

		/* The options written. */
		status = passkey_set_options(request.fields[1], account.pw_uid, request.fields[3], request.fields[4]);
		break;
	case PASSKEY_OP_SET_METHODS:
		/* The user's password, then the methods the login and locked screens take (WS200). */
		error = login_verify(request.fields[1], request.fields[2], &account, strings, sizeof(strings));
		if (error != 0) {
			status = passkey_fail("bad-secret");
			break;
		}

		/* The methods written. */
		status = passkey_set_methods(request.fields[1], account.pw_uid, request.fields[3]);
		break;
	default:
		status = passkey_fail("bad-request");
		break;
	}

	/* Nothing secret stays. */
	passkey_wipe(buffer, sizeof(buffer));
	passkey_wipe(strings, sizeof(strings));
	return status;
}

/* Clears the environment, keeps no core file, takes the default signals and gives fd 2 to /dev/null. */
static void
passkey_setup(
	void)
{
	struct rlimit none;
	sigset_t all;
	int descriptor;
	int signal_number;

	/* No variable of the caller's. */
	(void)clearenv();
	(void)setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);

	/* No core file of a process that held secrets. */
	none.rlim_cur = 0;
	none.rlim_max = 0;
	(void)setrlimit(RLIMIT_CORE, &none);

	/* The signals as a new process has them. */
	for (signal_number = 1; signal_number < 32; signal_number++)
		(void)signal(signal_number, SIG_DFL);
	sigemptyset(&all);
	(void)sigprocmask(SIG_SETMASK, &all, NULL);

	/* fd 2 is /dev/null, so a new file never takes it (and nothing is written into one). */
	descriptor = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (descriptor >= 0 && descriptor != 2) {
		(void)dup2(descriptor, 2);
		(void)close(descriptor);
	}
}

/* Reads the whole request (at most PASSKEY_REQUEST_MAX bytes) from standard input. */
static int
passkey_read(
	char *buffer,
	size_t capacity,
	size_t *length)
{
	ssize_t count;
	size_t used;

	/* Until the end of input, one byte beyond the bound being an error. */
	used = 0U;
	for (;;) {
		count = read(0, buffer + used, capacity - used);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return errno;
		if (count == 0)
			break;
		used += (size_t)count;
		if (used == capacity)
			return EMSGSIZE;
	}

	/* The request. */
	*length = used;
	return 0;
}

/* Answers a failure. */
static int
passkey_fail(
	const char *reason)
{
	printf("fail %s\n", reason);
	(void)fflush(stdout);
	return PASSKEY_EXIT_FAIL;
}

/* Answers a success, with the account's user ID. */
static int
passkey_ok(
	uid_t uid,
	const char *extra)
{
	if (extra != NULL && extra[0] != '\0')
		printf("ok uid=%u %s\n", (unsigned)uid, extra);
	else
		printf("ok uid=%u\n", (unsigned)uid);
	(void)fflush(stdout);
	return PASSKEY_EXIT_OK;
}

/* Looks an account up in passwd; 1 when it is there. */
static int
passkey_account(
	const char *name,
	struct passwd *account,
	char *buffer,
	size_t size)
{
	struct passwd *found;
	int error;

	found = NULL;
	error = getpwnam_r(name, account, buffer, size, &found);
	if (error != 0 || found == NULL)
		return 0;
	return 1;
}

/*
 * Tells whether an account may use a PIN or a key: a person's (user ID 1000
 * and up), its password neither locked nor expired.
 */
static int
passkey_usable(
	const char *name,
	uid_t uid)
{
	struct spwd shadow;
	struct spwd *found;
	char buffer[LOGIN_VERIFY_BUFFER];
	long today;
	int usable;
	int error;

	/* A person's account. */
	if (uid < PASSKEY_UID_FIRST)
		return 0;

	/* Its shadow line: not locked, not expired. */
	found = NULL;
	error = getspnam_r(name, &shadow, buffer, sizeof(buffer), &found);
	if (error != 0 || found == NULL)
		return 0;
	usable = 1;
	if (shadow.sp_pwdp[0] == '!' || shadow.sp_pwdp[0] == '*' || shadow.sp_pwdp[0] == '\0')
		usable = 0;
	today = (long)(time(NULL) / 86400);
	if (shadow.sp_expire > 0 && today >= shadow.sp_expire)
		usable = 0;
	passkey_wipe(buffer, sizeof(buffer));
	return usable;
}

/*
 * Reads /etc/passkey whole: a missing file is empty.  With need_private, a
 * file not root's or readable by anyone else is refused (EPERM).
 */
static int
passkey_file(
	char *text,
	size_t capacity,
	size_t *length,
	int need_private)
{
	struct stat status;
	int error;

	/* Its owner and mode. */
	*length = 0U;
	error = stat(PASSKEY_FILE, &status);
	if (error != 0 && errno == ENOENT)
		return 0;
	if (error != 0)
		return errno;
	if (need_private && (status.st_uid != 0 || (status.st_mode & 077) != 0))
		return EPERM;

	/* Its text. */
	error = account_file_read(PASSKEY_FILE, text, capacity, length);
	if (error != 0)
		return error;
	return 0;
}

/* Checks a PIN against the account's line. */
static int
passkey_auth_pin(
	const char *name,
	uid_t uid,
	char *pin)
{
	static char text[PASSKEY_FILE_MAX];
	char line[PASSKEY_REQUEST_MAX];
	const char *hash;
	char *made;
	size_t length;
	int error;
	int usable;

	/* Six digits, a usable account, a private file with the account's PIN. */
	if (!passkey_is_pin(pin)) {
		passkey_wipe(pin, strlen(pin));
		return passkey_fail("bad-secret");
	}
	usable = passkey_usable(name, uid);
	if (!usable) {
		passkey_wipe(pin, strlen(pin));
		return passkey_fail("locked-account");
	}
	error = passkey_file(text, sizeof(text), &length, 1);
	if (error == 0)
		error = passkey_record_find(text, length, name, uid, "pin", 0U, line, sizeof(line));
	if (error != 0) {
		passkey_wipe(pin, strlen(pin));
		return passkey_fail(error == EPERM ? "internal" : "not-enrolled");
	}

	/* The hash after "name:uid:pin:". */
	hash = strstr(line, ":pin:");
	if (hash == NULL) {
		passkey_wipe(pin, strlen(pin));
		return passkey_fail("not-enrolled");
	}
	hash += 5;
	made = crypt(pin, hash);
	passkey_wipe(pin, strlen(pin));
	if (made == NULL || strcmp(made, hash) != 0)
		return passkey_fail("bad-secret");

	/* Succeeded. */
	return passkey_ok(uid, NULL);
}

/* Answers the account's styles (styles), or its PIN and keys without secrets (enrolled). */
static int
passkey_styles(
	const char *name,
	uid_t uid,
	int enrolled)
{
	static char text[PASSKEY_FILE_MAX];
	struct passkey_options options;
	char extra[PASSKEY_REQUEST_MAX];
	char styles[PASSKEY_METHODS_MAX];
	size_t length;
	unsigned methods;
	unsigned effective;
	int pins;
	int keys;
	int usable;
	int error;

	/* What the file holds for the account (none when it cannot be read as it must be). */
	pins = 0;
	keys = 0;
	error = passkey_file(text, sizeof(text), &length, 1);
	if (error == 0) {
		pins = passkey_record_count(text, length, name, uid, "pin");
		keys = passkey_record_count(text, length, name, uid, "fido2");
	}
	usable = passkey_usable(name, uid);

	/* The account's options, and its sign-in methods (WS200). */
	passkey_options_default(&options);
	if (error == 0)
		(void)passkey_options_read(text, length, name, uid, &options);
	methods = passkey_options_methods(&options);

	/* styles: those of the methods set up and turned on, the PIN and the keys only when usable. */
	if (!enrolled) {
		effective = passkey_methods_effective(methods, usable && pins > 0, usable && keys > 0);
		passkey_methods_text(effective, styles, sizeof(styles));
		snprintf(extra, sizeof(extra), "styles=%s", styles);
		return passkey_ok(uid, extra);
	}

	/*
	 * enrolled: the counts, the key's PIN and touch for signing in
	 * (ws199-p001), the methods turned on as bits (WS200), then each key's
	 * reference and label (ws172-p003).
	 */
	snprintf(extra, sizeof(extra), "pin=%d fido2=%d key-pin=%d key-touch=%d methods=%u", pins > 0, keys, options.key_pin, options.key_touch,
	    methods);
	if (error == 0)
		passkey_keys_listed(text, length, name, uid, extra, sizeof(extra));
	return passkey_ok(uid, extra);
}

/*
 * Adds " key=REF/LABEL" for each of the account's keys to a listing: the
 * key's reference (passkey_record_ref) and its label in hexadecimal, so
 * that the line has no space of the label's and stays short.
 */
static void
passkey_keys_listed(
	const char *text,
	size_t length,
	const char *name,
	uid_t uid,
	char *extra,
	size_t size)
{
	static const char digits[] = "0123456789abcdef";
	char line[PASSKEY_REQUEST_MAX];
	char id[PASSKEY_REQUEST_MAX];
	char label[PASSKEY_FIELD_MAX];
	char ref[PASSKEY_REF_SIZE];
	size_t wanted;
	size_t used;
	size_t place;
	unsigned index;
	int error;

	/* Each key line that has its ID and label. */
	for (index = 0U; index < PASSKEY_FIDO2_MAX; index++) {
		error = passkey_record_find(text, length, name, uid, "fido2", index, line, sizeof(line));
		if (error != 0)
			break;
		error = passkey_record_field(line, 3U, id, sizeof(id));
		if (error == 0)
			error = passkey_record_field(line, 7U, label, sizeof(label));
		if (error != 0)
			continue;

		/* " key=", the reference, "/", the label's bytes as digits, while there is room. */
		passkey_record_ref(id, ref, sizeof(ref));
		used = strlen(extra);
		wanted = 6U + strlen(ref);
		wanted += 2U * strlen(label);
		if (used + wanted + 1U > size)
			break;
		used += (size_t)snprintf(extra + used, size - used, " key=%s/", ref);
		for (place = 0U; label[place] != '\0'; place++) {
			extra[used++] = digits[(unsigned char)label[place] >> 4];
			extra[used++] = digits[(unsigned char)label[place] & 0x0fU];
		}

		/* The listing ends after it. */
		extra[used] = '\0';
	}
}

/* Sets (pin not NULL) or removes the account's PIN, under the account files' lock. */
static int
passkey_change_pin(
	const char *name,
	uid_t uid,
	const char *pin)
{
	static char text[PASSKEY_FILE_MAX];
	static char output[PASSKEY_FILE_MAX + PASSKEY_REQUEST_MAX];
	char hash[ACCOUNT_HASH_MAX];
	char line[PASSKEY_REQUEST_MAX];
	sigset_t held;
	sigset_t previous;
	size_t length;
	size_t written;
	int usable;
	int version;
	int error;

	/* A new PIN only for a usable account, hashed like a password. */
	line[0] = '\0';
	if (pin != NULL) {
		usable = passkey_usable(name, uid);
		if (!usable)
			return passkey_fail("locked-account");
		error = account_password_hash(pin, hash, sizeof(hash));
		if (error != 0)
			return passkey_fail("internal");
		snprintf(line, sizeof(line), "%s:%u:pin:%s", name, (unsigned)uid, hash);
	}

	/* The signals that would stop the change are held while the file changes. */
	sigfillset(&held);
	(void)sigprocmask(SIG_BLOCK, &held, &previous);
	error = account_files_lock();
	if (error != 0) {
		(void)sigprocmask(SIG_SETMASK, &previous, NULL);
		return passkey_fail("busy");
	}

	/* The file read again under the lock, a version this passkey may write, the new text. */
	error = passkey_file(text, sizeof(text), &length, 0);
	version = passkey_record_version(text, length);
	if (error == 0 && version > PASSKEY_VERSION)
		error = EROFS;
	if (error == 0)
		error = passkey_record_replace(text, length, name, "pin", pin != NULL ? line : NULL, output, sizeof(output), &written);
	if (error == 0)
		error = account_file_write(PASSKEY_FILE, 0600, output, written);
	account_files_unlock();
	(void)sigprocmask(SIG_SETMASK, &previous, NULL);
	passkey_wipe(hash, sizeof(hash));
	if (error != 0)
		return passkey_fail("internal");

	/* Succeeded. */
	return passkey_ok(uid, NULL);
}

/*
 * Sets whether the account's key asks its PIN and its touch to sign in
 * (ws199-p001 section 2): "1" or "0" each, the touch left out only with
 * the PIN; the account must have a key.  The methods of the line are kept
 * (WS200's).
 */
static int
passkey_set_options(
	const char *name,
	uid_t uid,
	const char *key_pin,
	const char *key_touch)
{
	int pin;
	int touch;
	int valid;
	int status;

	/* "0" or "1" each; no touch only without the PIN. */
	pin = strcmp(key_pin, "1") == 0;
	touch = strcmp(key_touch, "1") == 0;
	valid = (pin || strcmp(key_pin, "0") == 0) && (touch || strcmp(key_touch, "0") == 0);
	if (!valid || (pin && !touch))
		return passkey_fail("bad-request");

	/* Written, the methods kept. */
	status = passkey_options_write(name, uid, PASSKEY_CHANGE_KEY, pin, touch, 0U);
	return status;
}

/*
 * Sets the methods the login and locked screens take (WS200): known words,
 * each once, with the password or a key among them (the PIN alone is no
 * first sign-in); without the password the account must have a key.  The
 * key's PIN and touch of the line are kept.
 */
static int
passkey_set_methods(
	const char *name,
	uid_t uid,
	const char *text)
{
	unsigned methods;
	int status;
	int error;

	/* The words, and a first sign-in among them. */
	error = passkey_methods_parse(text, &methods);
	if (error != 0)
		return passkey_fail("bad-request");
	if ((methods & (PASSKEY_METHOD_PASSWORD | PASSKEY_METHOD_FIDO2)) == 0U)
		return passkey_fail("bad-request");

	/* Written, the key's options kept. */
	status = passkey_options_write(name, uid, PASSKEY_CHANGE_METHODS, 0, 0, methods);
	return status;
}

/*
 * Writes the account's options line with one change (the key's PIN and
 * touch, or the methods), the other fields as they were, under the
 * account files' lock; a line of the defaults is not kept.  A change of
 * the key's options needs a key, as do methods without the password.
 */
static int
passkey_options_write(
	const char *name,
	uid_t uid,
	int change,
	int key_pin,
	int key_touch,
	unsigned methods)
{
	static char text[PASSKEY_FILE_MAX];
	static char output[PASSKEY_FILE_MAX + PASSKEY_REQUEST_MAX];
	struct passkey_options options;
	char line[PASSKEY_REQUEST_MAX];
	sigset_t held;
	sigset_t previous;
	size_t length;
	size_t written;
	const char *added;
	int keyless;
	int keys;
	int defaults;
	int version;
	int error;

	/* The signals that would stop the change are held while the file changes. */
	sigfillset(&held);
	(void)sigprocmask(SIG_BLOCK, &held, &previous);
	error = account_files_lock();
	if (error != 0) {
		(void)sigprocmask(SIG_SETMASK, &previous, NULL);
		return passkey_fail("busy");
	}

	/* The file read again under the lock, and whether the account has a key. */
	error = passkey_file(text, sizeof(text), &length, 0);
	version = passkey_record_version(text, length);
	if (error == 0 && version > PASSKEY_VERSION)
		error = EROFS;
	keys = 0;
	if (error == 0)
		keys = passkey_record_count(text, length, name, uid, "fido2");

	/* A change that needs a key the account does not have. */
	keyless = 0;
	if (change == PASSKEY_CHANGE_KEY)
		keyless = 1;
	if (change == PASSKEY_CHANGE_METHODS && (methods & PASSKEY_METHOD_PASSWORD) == 0U)
		keyless = 1;
	if (error == 0 && keyless && keys == 0) {
		account_files_unlock();
		(void)sigprocmask(SIG_SETMASK, &previous, NULL);
		return passkey_fail("not-enrolled");
	}

	/* The new line (none for the defaults), in place of the old one. */
	passkey_options_default(&options);
	if (error == 0) {
		(void)passkey_options_read(text, length, name, uid, &options);
		if (change == PASSKEY_CHANGE_KEY) {
			options.key_pin = key_pin;
			options.key_touch = key_touch;
		} else {
			passkey_methods_text(methods, options.methods, sizeof(options.methods));
		}

		/* The line. */
		error = passkey_options_line(name, uid, &options, line, sizeof(line));
	}

	/* The defaults need no line. */
	defaults = passkey_options_is_default(&options);
	added = line;
	if (defaults)
		added = NULL;
	if (error == 0)
		error = passkey_record_replace(text, length, name, "options", added, output, sizeof(output), &written);
	if (error == 0)
		error = account_file_write(PASSKEY_FILE, 0600, output, written);
	account_files_unlock();
	(void)sigprocmask(SIG_SETMASK, &previous, NULL);
	if (error != 0)
		return passkey_fail("internal");

	/* Succeeded. */
	return passkey_ok(uid, NULL);
}

/*
 * Tells whether the account turned a method off for the login and locked
 * screens (WS200): not among its methods, and not taken in its stead
 * (the password when nothing else is a first sign-in).  An account whose
 * file does not read has every method.
 */
static int
passkey_method_off(
	const char *name,
	uid_t uid,
	unsigned method)
{
	static char text[PASSKEY_FILE_MAX];
	struct passkey_options options;
	size_t length;
	unsigned methods;
	unsigned effective;
	int pins;
	int keys;
	int error;

	/* The file, its options and what the account has. */
	error = passkey_file(text, sizeof(text), &length, 1);
	if (error != 0)
		return 0;
	passkey_options_default(&options);
	(void)passkey_options_read(text, length, name, uid, &options);
	methods = passkey_options_methods(&options);
	pins = passkey_record_count(text, length, name, uid, "pin");
	keys = passkey_record_count(text, length, name, uid, "fido2");
	effective = passkey_methods_effective(methods, pins > 0, keys > 0);

	/* Among the methods, or taken in their stead. */
	if ((methods & method) != 0U)
		return 0;
	if ((effective & method) != 0U)
		return 0;

	/* Turned off. */
	return 1;
}

/* Hands the whole request to passkey-fido2 on its standard input; its answer and status are this one's. */
static int
passkey_fido2(
	const char *request,
	size_t length)
{
	char *argv[2];
	int pipes[2];
	int status;
	pid_t child;

	/* The program (ws172-p003). */
	if (access(PASSKEY_FIDO2_PATH, X_OK) != 0)
		return passkey_fail("no-key");

	/* Its input. */
	if (pipe(pipes) != 0)
		return passkey_fail("internal");
	(void)fflush(stdout);
	child = fork();
	if (child < 0)
		return passkey_fail("internal");
	if (child == 0) {
		(void)dup2(pipes[0], 0);
		(void)close(pipes[0]);
		(void)close(pipes[1]);
		argv[0] = "passkey-fido2";
		argv[1] = NULL;
		(void)execv(PASSKEY_FIDO2_PATH, argv);
		_exit(PASSKEY_EXIT_INTERNAL);
	}
	(void)close(pipes[0]);
	(void)write(pipes[1], request, length);
	(void)close(pipes[1]);

	/* sessiond's end of the work is passkey-fido2's to finish: passkey waits for it whatever comes (ws199-p001). */
	(void)signal(SIGTERM, SIG_IGN);
	(void)signal(SIGHUP, SIG_IGN);
	(void)signal(SIGPIPE, SIG_IGN);

	/* Its status. */
	while (waitpid(child, &status, 0) < 0 && errno == EINTR)
		continue;
	if (!WIFEXITED(status))
		return PASSKEY_EXIT_INTERNAL;
	return WEXITSTATUS(status);
}
