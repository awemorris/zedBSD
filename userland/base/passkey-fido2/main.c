/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /usr/libexec/passkey-fido2 (fido2.h; ws172-p003): the security key
 * style of /sbin/passkey, which starts it with the same request on its
 * standard input; its answer goes to passkey's standard output.
 *
 *   auth NAME fido2 KEY-PIN                   the login with a key (its PIN and touch)
 *   auth-fido2 NAME login|unlock KEY-PIN      the login or unlock with a key as the
 *                                             account's options ask (ws199-p001)
 *   enroll-fido2 NAME PASSWORD LABEL KEY-PIN  a new key for the account
 *   remove-fido2 NAME PASSWORD ID-OR-REF      one of the account's keys goes
 *   key-info NAME                             what the key there is (ws199-p001)
 *   key-set-pin NAME NEW-PIN                  the key's first PIN
 *   key-change-pin NAME KEY-PIN NEW-PIN       the key's PIN changed
 *   key-reset NAME PASSWORD                   the key reset after it is plugged
 *                                             in again, its lines removed
 *   key-owner NAME|-                          whose the key there is: the account
 *                                             named, or every account (-)
 *
 * It runs as root alone (its real user ID), reads nothing from its command
 * line or environment, makes the challenge, opens and claims the keys,
 * starts the device helper (helper.c) and checks the key's answer itself:
 * the public key is the account's own line's, never the key's word.  The
 * answer is passkey's: "status touch" lines (and for a reset "status
 * verified" once the password is right and "status replug" while the key
 * is to be plugged in again), then "ok uid=N" (with "id=ID" after a
 * registration, the key's facts after key-info, "removed=N" after a reset)
 * or "fail REASON"; the exit status 0, 1 or 2.
 */

#include "fido2.h"

#include "userland/base/common/account.h"
#include "userland/base/login/verify.h"
#include "userland/base/libpasskey/verify.h"
#include "userland/base/passkey/passkey.h"

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
#include <time.h>
#include <unistd.h>

/* The helper's account, and the first user ID of a person's account. */
#define MAIN_HELPER_ACCOUNT	"_passkey"
#define MAIN_UID_FIRST		1000U

/* The largest /etc/passkey, read and written whole, and the longest line. */
#define MAIN_FILE_MAX		65536U
#define MAIN_LINE_MAX		4096U

/* The exit statuses. */
#define MAIN_EXIT_OK		0
#define MAIN_EXIT_FAIL		1
#define MAIN_EXIT_INTERNAL	2

/* The account's keys read from /etc/passkey (the lines as they are, and taken apart). */
struct main_keys {
	char lines[PASSKEY_FIDO2_MAX][MAIN_LINE_MAX];
	struct fido2_record records[PASSKEY_FIDO2_MAX];
	size_t count;
};

/* Every account's key lines (a reset removes the lines of every account the key held, ws199-p001). */
struct main_all_keys {
	char lines[FIDO2_IDS_MAX][MAIN_LINE_MAX];
	char names[FIDO2_IDS_MAX][64];
	struct fido2_record records[FIDO2_IDS_MAX];
	size_t count;
};

/*
 * The accounts an owner's question looks for (ws199-p001 section 4.2):
 * each group's name and user ID, and for each credential asked its line
 * in main_all and its group.
 */
struct main_owners {
	char names[FIDO2_IDS_MAX][64];
	uid_t uids[FIDO2_IDS_MAX];
	size_t group_count;
	size_t records[FIDO2_IDS_MAX];
	size_t count;
};

/* How long a reset waits for the key to be plugged in again, and how often it looks. */
#define MAIN_REPLUG_MS		30000U
#define MAIN_REPLUG_STEP_MS	100U

/* The file's text, the new text, the account's keys, every account's, and the keys the helper saw (large; one request a run). */
static char main_text[MAIN_FILE_MAX];
static char main_output[MAIN_FILE_MAX + MAIN_LINE_MAX];
static struct main_keys main_account_keys;
static struct main_all_keys main_all;
static struct main_owners main_owners;
static struct fido2_devices main_devices;

static void main_setup(void);
static int main_read(char *buffer, size_t capacity, size_t *length);
static int main_fail(const char *reason);
static int main_ok(uid_t uid, const char *extra);
static int main_account(const char *name, struct passwd *account, char *buffer, size_t size);
static int main_usable(const char *name, uid_t uid);
static int main_file(size_t *length, int need_private);
static int main_keys(const char *name, uid_t uid, struct main_keys *keys);
static int main_helper_account(uid_t *uid, gid_t *gid);
static int main_auth(const char *name, uid_t uid, char *pin, int unlock, int optional);
static int main_enroll(const char *name, uid_t uid, const char *label, char *pin);
static int main_remove(const char *name, uid_t uid, const char *id);
static int main_change(const char *name, const char *field, const char *added);
static int main_run(const struct fido2_job *job, struct fido2_message *message);
static int main_key_info(uid_t uid);
static int main_key_pin(uid_t uid, int kind, char *pin, char *fresh);
static int main_key_reset(const char *name, uid_t uid);
static int main_all_keys(struct main_all_keys *all);
static int main_key_owner(const char *name);
static int main_owners_gather(const char *name, const struct main_all_keys *all, struct main_owners *owners);
static int main_owner_group(struct main_owners *owners, const char *name, const char *line);
static int main_key_method_on(const char *name, uid_t uid);
static int main_replug(void);
static int main_job_failed(int error, const struct fido2_message *message, int kind);
static uint64_t main_now_ms(void);
static int main_options_reset(const char *name, uid_t uid);
static const char *main_verify_reason(int error);

/* Answers one request. */
int
main(void)
{
	struct passkey_request request;
	struct passwd account;
	char buffer[PASSKEY_REQUEST_MAX + 1U];
	char strings[LOGIN_VERIFY_BUFFER];
	size_t length;
	int status;
	int error;
	uid_t real;
	int found;
	int is_auth;
	int is_key;
	int unlock;
	int same;

	/* Root alone, a clean environment, no core file. */
	real = getuid();
	if (real != 0)
		return MAIN_EXIT_INTERNAL;
	main_setup();
	fido2_catch_end();

	/* The request. */
	length = 0U;
	error = main_read(buffer, sizeof(buffer), &length);
	if (error == 0)
		error = passkey_request_parse(buffer, length, &request);
	if (error != 0) {
		passkey_wipe(buffer, sizeof(buffer));
		return main_fail("bad-request");
	}

	/* Whose the key there is: no secret and, for the login screen (-), no account (ws199-p001). */
	if (request.operation == PASSKEY_OP_KEY_OWNER) {
		status = main_key_owner(request.fields[1]);
		passkey_wipe(buffer, sizeof(buffer));
		return status;
	}

	/* A security key's: the login with one, a registration, a removal, or a key's own operation. */
	is_auth = 0;
	if (request.operation == PASSKEY_OP_AUTH)
		is_auth = strcmp(request.fields[2], "fido2") == 0;
	unlock = 0;
	if (request.operation == PASSKEY_OP_AUTH_FIDO2) {
		is_auth = 1;
		unlock = strcmp(request.fields[2], "unlock") == 0;
		same = strcmp(request.fields[2], "login") == 0;
		if (!unlock && !same) {
			passkey_wipe(buffer, sizeof(buffer));
			return main_fail("bad-request");
		}
	}

	/* A key's own operation (ws199-p001): the info, its PIN or its reset. */
	is_key = 0;
	if (request.operation >= PASSKEY_OP_KEY_INFO && request.operation <= PASSKEY_OP_KEY_RESET)
		is_key = 1;

	/* Only the operations this program carries out. */
	if (!is_auth && !is_key && request.operation != PASSKEY_OP_ENROLL_FIDO2 && request.operation != PASSKEY_OP_REMOVE_FIDO2) {
		passkey_wipe(buffer, sizeof(buffer));
		return main_fail("bad-request");
	}

	/* A key's own operation: the account is there; only a reset needs its password (ws199-p001). */
	if (is_key && request.operation != PASSKEY_OP_KEY_RESET) {
		found = main_account(request.fields[1], &account, strings, sizeof(strings));
		if (!found) {
			status = main_fail("no-such-user");
		} else if (request.operation == PASSKEY_OP_KEY_INFO) {
			status = main_key_info(account.pw_uid);
		} else if (request.operation == PASSKEY_OP_KEY_SET_PIN) {
			status = main_key_pin(account.pw_uid, FIDO2_JOB_SET_PIN, NULL, request.fields[2]);
		} else {
			status = main_key_pin(account.pw_uid, FIDO2_JOB_CHANGE_PIN, request.fields[2], request.fields[3]);
		}

		/* Nothing secret stays. */
		passkey_wipe(buffer, sizeof(buffer));
		return status;
	}

	/* The login's account. */
	if (is_auth) {
		found = main_account(request.fields[1], &account, strings, sizeof(strings));
		if (!found) {
			passkey_wipe(buffer, sizeof(buffer));
			return main_fail("no-such-user");
		}

		/* The login, and nothing secret stays. */
		status = main_auth(request.fields[1], account.pw_uid, request.fields[3], unlock, request.operation == PASSKEY_OP_AUTH_FIDO2);
		passkey_wipe(buffer, sizeof(buffer));
		return status;
	}

	/* A change needs the account's password. */
	error = login_verify(request.fields[1], request.fields[2], &account, strings, sizeof(strings));
	if (error != 0) {
		passkey_wipe(buffer, sizeof(buffer));
		passkey_wipe(strings, sizeof(strings));
		return main_fail("bad-secret");
	}

	/* The registration, the removal, or a reset (the password was right: sessiond clears the count). */
	if (request.operation == PASSKEY_OP_ENROLL_FIDO2) {
		status = main_enroll(request.fields[1], account.pw_uid, request.fields[3], request.fields[4]);
	} else if (request.operation == PASSKEY_OP_REMOVE_FIDO2) {
		status = main_remove(request.fields[1], account.pw_uid, request.fields[3]);
	} else {
		printf("status verified\n");
		(void)fflush(stdout);
		status = main_key_reset(request.fields[1], account.pw_uid);
	}

	/* Nothing secret stays. */
	passkey_wipe(buffer, sizeof(buffer));
	passkey_wipe(strings, sizeof(strings));
	return status;
}

/* Clears the environment, keeps no core file, takes the default signals and gives fd 2 to /dev/null. */
static void
main_setup(void)
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

	/* fd 2 is /dev/null, so a new file never takes it. */
	descriptor = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (descriptor >= 0 && descriptor != 2) {
		(void)dup2(descriptor, 2);
		(void)close(descriptor);
	}
}

/* Reads the whole request (at most PASSKEY_REQUEST_MAX bytes) from standard input. */
static int
main_read(
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

	/* Succeeded: the request. */
	*length = used;
	return 0;
}

/* Answers a failure; returns the exit status. */
static int
main_fail(
	const char *reason)
{
	/* The line, at once. */
	printf("fail %s\n", reason);
	(void)fflush(stdout);
	return MAIN_EXIT_FAIL;
}

/* Answers a success with the account's user ID (and extra after it); returns the exit status. */
static int
main_ok(
	uid_t uid,
	const char *extra)
{
	/* The line, at once. */
	if (extra != NULL)
		printf("ok uid=%u %s\n", (unsigned)uid, extra);
	else
		printf("ok uid=%u\n", (unsigned)uid);
	(void)fflush(stdout);
	return MAIN_EXIT_OK;
}

/* Looks an account up in passwd; 1 when it is there. */
static int
main_account(
	const char *name,
	struct passwd *account,
	char *buffer,
	size_t size)
{
	struct passwd *found;
	int error;

	/* The account's entry. */
	found = NULL;
	error = getpwnam_r(name, account, buffer, size, &found);
	if (error != 0 || found == NULL)
		return 0;

	/* It is there. */
	return 1;
}

/*
 * Tells whether an account may use a key: a person's (user ID 1000 and
 * up), its password neither locked nor expired.
 */
static int
main_usable(
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
	if (uid < MAIN_UID_FIRST)
		return 0;

	/* Its shadow line. */
	found = NULL;
	error = getspnam_r(name, &shadow, buffer, sizeof(buffer), &found);
	if (error != 0 || found == NULL)
		return 0;

	/* Not locked, not expired. */
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
 * Reads /etc/passkey whole into main_text: a missing file is empty.  With
 * need_private, a file not root's or readable by anyone else is refused
 * (EPERM).
 */
static int
main_file(
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
	error = account_file_read(PASSKEY_FILE, main_text, sizeof(main_text), length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Gathers the account's keys from /etc/passkey (a malformed line is left
 * out).  Returns 0, or EPERM for a file that is not private.
 */
static int
main_keys(
	const char *name,
	uid_t uid,
	struct main_keys *keys)
{
	size_t length;
	unsigned index;
	int error;

	/* The file. */
	memset(keys, 0, sizeof(*keys));
	error = main_file(&length, 1);
	if (error != 0)
		return error;

	/* Each of the account's key lines that reads. */
	for (index = 0U; keys->count < PASSKEY_FIDO2_MAX; index++) {
		error = passkey_record_find(main_text, length, name, uid, "fido2", index, keys->lines[keys->count], MAIN_LINE_MAX);
		if (error == ENOENT)
			break;
		if (error != 0)
			continue;
		error = fido2_record_parse(keys->lines[keys->count], &keys->records[keys->count]);
		if (error == 0)
			keys->count++;
	}

	/* Succeeded: the keys. */
	return 0;
}

/* Looks up the helper's account (_passkey); returns 0 or ENOENT. */
static int
main_helper_account(
	uid_t *uid,
	gid_t *gid)
{
	struct passwd account;
	char buffer[1024];
	int found;

	/* Its entry. */
	found = main_account(MAIN_HELPER_ACCOUNT, &account, buffer, sizeof(buffer));
	if (!found || account.pw_uid == 0)
		return ENOENT;

	/* Succeeded: its IDs. */
	*uid = account.pw_uid;
	*gid = account.pw_gid;
	return 0;
}

/*
 * Logs in (or unlocks) with a key: the account's keys, a new challenge,
 * the helper's answer checked against the account's own public key, and a
 * larger count kept.  With optional (auth-fido2, ws199-p001) the account's
 * options decide: an empty PIN only when the key's PIN is not asked
 * (then the user is not verified), and no touch only to unlock when the
 * touch is not asked; the flags checked follow.
 */
static int
main_auth(
	const char *name,
	uid_t uid,
	char *pin,
	int unlock,
	int optional)
{
	struct passkey_options options;
	size_t length;
	unsigned required;
	static struct fido2_job job;
	static struct fido2_message message;
	struct pk_credential allowed[PASSKEY_FIDO2_MAX];
	struct pk_expectation expectation;
	struct pk_assertion assertion;
	struct main_keys *keys;
	char line[MAIN_LINE_MAX];
	uint8_t challenge[FIDO2_CHALLENGE_SIZE];
	uint32_t count;
	size_t matched;
	size_t index;
	int pin_given;
	int usable;
	int error;

	/* A usable account with keys. */
	usable = main_usable(name, uid);
	if (!usable) {
		passkey_wipe(pin, strlen(pin));
		return main_fail("locked-account");
	}

	/* Its keys. */
	keys = &main_account_keys;
	error = main_keys(name, uid, keys);
	if (error != 0 || keys->count == 0U) {
		passkey_wipe(pin, strlen(pin));
		if (error != 0)
			return main_fail("internal");
		return main_fail("not-enrolled");
	}

	/* A security key the account turned off for the screens is refused (WS200). */
	usable = main_key_method_on(name, uid);
	if (!usable) {
		passkey_wipe(pin, strlen(pin));
		return main_fail("style-off");
	}

	/* Reads the account's options; the old auth request keeps the defaults (PIN and touch). */
	passkey_options_default(&options);
	if (optional) {
		error = main_file(&length, 1);
		if (error == 0)
			(void)passkey_options_read(main_text, length, name, uid, &options);
	}

	/* Whether the caller gave the key's PIN. */
	pin_given = 0;
	if (pin[0] != '\0')
		pin_given = 1;

	/* What the key is asked and what its answer must carry; an empty PIN the account asks for is refused. */
	error = fido2_auth_flags(options.key_pin, options.key_touch, unlock, pin_given, &required, &job.presence);
	if (error != 0)
		return main_fail("bad-request");

	/* The challenge and the client data hash. */
	error = pk_crypto_random(challenge, sizeof(challenge));
	if (error == 0)
		error = fido2_client_data_hash(name, challenge, job.client_data_hash);
	if (error != 0) {
		passkey_wipe(pin, strlen(pin));
		return main_fail("internal");
	}

	/* The helper's job: the credentials allowed and the key's PIN. */
	job.kind = FIDO2_JOB_ASSERT;
	(void)snprintf(job.pin, sizeof(job.pin), "%s", pin);
	passkey_wipe(pin, strlen(pin));
	for (index = 0U; index < keys->count; index++) {
		job.ids[index] = keys->records[index].id;
		job.id_sizes[index] = keys->records[index].id_size;
	}

	/* As many as the account has. */
	job.id_count = keys->count;

	/* The helper's answer. */
	error = main_run(&job, &message);
	passkey_wipe(job.pin, sizeof(job.pin));
	if (error != 0) {
		if (error == ETIMEDOUT)
			return main_fail("timeout");
		return main_fail("device");
	}

	/* The helper's own failure is the answer. */
	if (message.kind == FIDO2_MESSAGE_FAIL)
		return main_fail(message.reason);
	if (message.kind != FIDO2_MESSAGE_ASSERTION)
		return main_fail("device");

	/* What is expected: the login's relying party, this client data hash, the user present and verified as asked, the account's keys. */
	memset(&expectation, 0, sizeof(expectation));
	for (index = 0U; index < keys->count; index++) {
		allowed[index].id = keys->records[index].id;
		allowed[index].id_size = keys->records[index].id_size;
		allowed[index].cose_key = keys->records[index].cose_key;
		allowed[index].cose_key_size = keys->records[index].cose_key_size;
		allowed[index].sign_count = keys->records[index].sign_count;
	}

	/* The relying party, the hash, the flags, the keys. */
	expectation.rp_id = FIDO2_RP;
	memcpy(expectation.client_data_hash, job.client_data_hash, sizeof(expectation.client_data_hash));
	expectation.required_flags = required;
	expectation.credentials = allowed;
	expectation.credential_count = keys->count;

	/* The answer, checked here. */
	assertion.credential_id = message.id;
	assertion.credential_id_size = message.id_size;
	assertion.auth_data = message.auth_data;
	assertion.auth_data_size = message.auth_data_size;
	assertion.signature = message.signature;
	assertion.signature_size = message.signature_size;
	error = pk_verify_assertion(&expectation, &assertion, &matched, &count);
	if (error != 0)
		return main_fail(main_verify_reason(error));

	/* A larger count is kept (a failure to keep it does not undo the login). */
	if (count > keys->records[matched].sign_count) {
		error = fido2_record_recount(keys->lines[matched], count, line, sizeof(line));
		if (error == 0)
			(void)main_change(name, keys->records[matched].id_text, line);
	}

	/* Succeeded: the account's key. */
	return main_ok(uid, NULL);
}

/*
 * Registers a new key: a usable account with room, a valid label, the one
 * key there making a credential, its authenticator data read here.
 */
static int
main_enroll(
	const char *name,
	uid_t uid,
	const char *label,
	char *pin)
{
	static struct fido2_job job;
	static struct fido2_message message;
	static struct pk_made_credential made;
	struct main_keys *keys;
	struct tm day;
	time_t now;
	char id[FIDO2_BASE64_MAX];
	char cose_key[FIDO2_BASE64_MAX];
	char date[16];
	char line[MAIN_LINE_MAX];
	char extra[FIDO2_BASE64_MAX + 8U];
	size_t index;
	int usable;
	int valid;
	int error;

	/* A usable account, a label, room for one more key. */
	usable = main_usable(name, uid);
	valid = fido2_label_valid(label);
	keys = &main_account_keys;
	error = main_keys(name, uid, keys);
	if (!usable || !valid || error != 0 || keys->count >= PASSKEY_FIDO2_MAX) {
		passkey_wipe(pin, strlen(pin));
		if (!usable)
			return main_fail("locked-account");
		if (error != 0)
			return main_fail("internal");
		return main_fail("bad-request");
	}

	/* The helper's job: a new client data hash, the user's ID and name, the keys not to make again, the key's PIN. */
	job.kind = FIDO2_JOB_MAKE;
	error = pk_crypto_random(job.client_data_hash, sizeof(job.client_data_hash));
	if (error == 0)
		error = fido2_user_id(name, job.user_id);
	if (error != 0) {
		passkey_wipe(pin, strlen(pin));
		return main_fail("internal");
	}

	/* The name, the key's PIN (the request's copy wiped). */
	job.user_name = name;
	(void)snprintf(job.pin, sizeof(job.pin), "%s", pin);
	passkey_wipe(pin, strlen(pin));
	for (index = 0U; index < keys->count; index++) {
		job.ids[index] = keys->records[index].id;
		job.id_sizes[index] = keys->records[index].id_size;
	}

	/* As many as the account has. */
	job.id_count = keys->count;

	/* The helper's answer. */
	error = main_run(&job, &message);
	passkey_wipe(job.pin, sizeof(job.pin));
	if (error != 0) {
		if (error == ETIMEDOUT)
			return main_fail("timeout");
		return main_fail("device");
	}

	/* The helper's own failure is the answer. */
	if (message.kind == FIDO2_MESSAGE_FAIL)
		return main_fail(message.reason);
	if (message.kind != FIDO2_MESSAGE_MADE)
		return main_fail("device");

	/* Read here: the login's relying party, the user present and verified, an ES256 key on the curve. */
	error = pk_ctap2_read_made(FIDO2_RP, message.auth_data, message.auth_data_size, &made);
	if (error != 0 || (made.flags & PK_FLAG_UV) == 0U)
		return main_fail("device");

	/* The account's new line. */
	now = time(NULL);
	(void)gmtime_r(&now, &day);
	(void)strftime(date, sizeof(date), "%Y-%m-%d", &day);
	error = fido2_base64_encode(made.id, made.id_size, id, sizeof(id));
	if (error == 0)
		error = fido2_base64_encode(made.cose_key, made.cose_key_size, cose_key, sizeof(cose_key));
	if (error == 0)
		error = fido2_record_line(name, uid, id, cose_key, made.sign_count, label, date, line, sizeof(line));
	if (error != 0)
		return main_fail("internal");

	/* Kept, under the lock. */
	error = main_change(name, NULL, line);
	if (error != 0)
		return main_fail("internal");

	/* Succeeded: the new key's ID. */
	(void)snprintf(extra, sizeof(extra), "id=%s", id);
	return main_ok(uid, extra);
}

/* Removes one of the account's keys by its ID or its reference (passkey_record_ref, Settings' list). */
static int
main_remove(
	const char *name,
	uid_t uid,
	const char *id)
{
	struct main_keys *keys;
	char ref[PASSKEY_REF_SIZE];
	size_t index;
	size_t chosen;
	int found;
	int same;
	int error;

	/* The account's key of that ID or reference. */
	keys = &main_account_keys;
	error = main_keys(name, uid, keys);
	if (error != 0)
		return main_fail("internal");
	found = 0;
	chosen = 0U;
	for (index = 0U; index < keys->count && !found; index++) {
		passkey_record_ref(keys->records[index].id_text, ref, sizeof(ref));
		same = strcmp(keys->records[index].id_text, id) == 0 || strcmp(ref, id) == 0;
		if (same) {
			found = 1;
			chosen = index;
		}
	}

	/* Not one of the account's. */
	if (!found)
		return main_fail("not-enrolled");

	/* Its line goes, under the lock; with the last key the key's options go back to the defaults (ws199-p001). */
	error = main_change(name, keys->records[chosen].id_text, NULL);
	if (error != 0)
		return main_fail("internal");
	if (keys->count == 1U)
		(void)main_options_reset(name, uid);

	/* Succeeded. */
	return main_ok(uid, NULL);
}

/*
 * Changes /etc/passkey under the account files' lock, the signals held:
 * the name's key line of the ID field goes (none when field is NULL) and
 * added comes at the end (none when NULL).  Returns 0 or an errno value.
 */
static int
main_change(
	const char *name,
	const char *field,
	const char *added)
{
	sigset_t held;
	sigset_t previous;
	size_t length;
	size_t written;
	int version;
	int error;

	/* The signals that would stop the change are held; the lock. */
	sigfillset(&held);
	(void)sigprocmask(SIG_BLOCK, &held, &previous);
	error = account_files_lock();
	if (error != 0) {
		(void)sigprocmask(SIG_SETMASK, &previous, NULL);
		return error;
	}

	/* The file read again under the lock, a version this program may write, the new text. */
	error = main_file(&length, 0);
	version = passkey_record_version(main_text, length);
	if (error == 0 && version > PASSKEY_VERSION)
		error = EROFS;
	if (error == 0)
		error = passkey_record_edit(main_text, length, name, "fido2", field, added, main_output, sizeof(main_output), &written);
	if (error == 0)
		error = account_file_write(PASSKEY_FILE, 0600, main_output, written);

	/* The lock and the signals given back. */
	account_files_unlock();
	(void)sigprocmask(SIG_SETMASK, &previous, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the file is changed. */
	return 0;
}

/*
 * Opens and claims the keys, runs the helper as _passkey and closes the
 * keys.  Returns 0 with its answer, or an errno value.
 */
static int
main_run(
	const struct fido2_job *job,
	struct fido2_message *message)
{
	uid_t uid;
	gid_t gid;
	int error;

	/* The helper's account, and the keys (their names kept after they are closed). */
	error = main_helper_account(&uid, &gid);
	if (error != 0)
		return error;
	error = fido2_devices_open(&main_devices);
	if (error != 0)
		return error;

	/* The helper's answer, the keys given back. */
	error = fido2_run_helper(&main_devices, job, uid, gid, message);
	fido2_devices_close(&main_devices);
	if (error != 0)
		return error;

	/* Succeeded: the answer. */
	return 0;
}

/*
 * Tells what the key there is (Settings' wizards, ws199-p001 section
 * 4.1): "ok uid=N count=N" and, for one key, "name=HEX pin=0|1
 * retries=N min=N" (the name hexadecimal, as the device gives it).
 */
static int
main_key_info(
	uid_t uid)
{
	static struct fido2_job job;
	static struct fido2_message message;
	char name[2U * PK_OS_NAME_MAX + 1U];
	char extra[3U * PK_OS_NAME_MAX];
	const char *device;
	int error;

	/* The helper's answer. */
	memset(&job, 0, sizeof(job));
	job.kind = FIDO2_JOB_INFO;
	error = main_run(&job, &message);
	if (error != 0 || message.kind != FIDO2_MESSAGE_INFO)
		return main_job_failed(error, &message, FIDO2_MESSAGE_INFO);

	/* Not one key: how many. */
	if (message.info_count != 1U) {
		(void)snprintf(extra, sizeof(extra), "count=%u", message.info_count);
		return main_ok(uid, extra);
	}

	/* The one key's name, from its node (a USB key's or a reader's). */
	device = "";
	if (message.info_index < PK_OS_DEVICES_MAX && message.info_card)
		device = main_devices.card_names[message.info_index];
	if (message.info_index < PK_OS_DEVICES_MAX && !message.info_card)
		device = main_devices.names[message.info_index];
	(void)fido2_hex_encode((const uint8_t *)device, strlen(device), name, sizeof(name));

	/* Told. */
	(void)snprintf(extra, sizeof(extra), "count=1 name=%s pin=%u retries=%u min=%u", name, message.info_pin, message.info_retries,
	    message.info_min);
	return main_ok(uid, extra);
}

/* Sets the key's first PIN (current NULL) or changes it; the key checks the PINs and counts the wrong ones. */
static int
main_key_pin(
	uid_t uid,
	int kind,
	char *current,
	char *fresh)
{
	static struct fido2_job job;
	static struct fido2_message message;
	int error;

	/* The job: the PINs (the request's copies wiped). */
	memset(&job, 0, sizeof(job));
	job.kind = kind;
	if (current != NULL) {
		(void)snprintf(job.pin, sizeof(job.pin), "%s", current);
		passkey_wipe(current, strlen(current));
	}

	/* The new PIN. */
	(void)snprintf(job.new_pin, sizeof(job.new_pin), "%s", fresh);
	passkey_wipe(fresh, strlen(fresh));

	/* The helper's answer. */
	error = main_run(&job, &message);
	passkey_wipe(job.pin, sizeof(job.pin));
	passkey_wipe(job.new_pin, sizeof(job.new_pin));
	if (error != 0 || message.kind != FIDO2_MESSAGE_DONE)
		return main_job_failed(error, &message, FIDO2_MESSAGE_DONE);

	/* Succeeded. */
	return main_ok(uid, NULL);
}

/*
 * Resets the key (ws199-p001 section 4.5): the key is to be plugged in
 * again (or held to the reader again), then at once the helper looks for
 * the credentials of every account it holds and resets it with the touch;
 * the lines of those it held are removed.  "ok uid=N removed=N".
 */
static int
main_key_reset(
	const char *name,
	uid_t uid)
{
	static struct fido2_job job;
	static struct fido2_message message;
	struct main_all_keys *all;
	char extra[32];
	unsigned removed;
	size_t index;
	int usable;
	int error;

	/* A usable account. */
	usable = main_usable(name, uid);
	if (!usable)
		return main_fail("locked-account");

	/* Every account's credentials, for the helper to look for. */
	all = &main_all;
	error = main_all_keys(all);
	if (error != 0)
		return main_fail("internal");
	memset(&job, 0, sizeof(job));
	job.kind = FIDO2_JOB_RESET;
	error = pk_crypto_random(job.client_data_hash, sizeof(job.client_data_hash));
	if (error != 0)
		return main_fail("internal");
	for (index = 0U; index < all->count; index++) {
		job.ids[index] = all->records[index].id;
		job.id_sizes[index] = all->records[index].id_size;
	}

	/* As many as there are. */
	job.id_count = all->count;

	/* The key plugged in again. */
	error = main_replug();
	if (error == ECANCELED)
		return main_fail("canceled");
	if (error == EEXIST)
		return main_fail("many-keys");
	if (error != 0)
		return main_fail("timeout");

	/* The helper's answer. */
	error = main_run(&job, &message);
	if (error != 0 || message.kind != FIDO2_MESSAGE_RESET)
		return main_job_failed(error, &message, FIDO2_MESSAGE_RESET);

	/* The lines of the credentials it held go (every account's: the key no longer holds them). */
	removed = 0U;
	for (index = 0U; index < all->count && index < 32U; index++) {
		if ((message.held & (1U << index)) == 0U)
			continue;
		error = main_change(all->names[index], all->records[index].id_text, NULL);
		if (error == 0)
			removed++;
	}

	/* Succeeded. */
	(void)snprintf(extra, sizeof(extra), "removed=%u", removed);
	return main_ok(uid, extra);
}

/* Gathers every account's key lines from /etc/passkey (a malformed line is left out).  Returns 0 or an errno value. */
static int
main_all_keys(
	struct main_all_keys *all)
{
	char field[64];
	size_t length;
	size_t start;
	size_t end;
	int same;
	int error;

	/* The file. */
	memset(all, 0, sizeof(*all));
	error = main_file(&length, 1);
	if (error != 0)
		return error;

	/* Each line of kind fido2 that reads, while there is room. */
	start = 0U;
	while (start < length && all->count < FIDO2_IDS_MAX) {
		end = start;
		while (end < length && main_text[end] != '\n')
			end++;
		if (end - start < MAIN_LINE_MAX && main_text[start] != '#') {
			memcpy(all->lines[all->count], main_text + start, end - start);
			all->lines[all->count][end - start] = '\0';
			error = passkey_record_field(all->lines[all->count], 2U, field, sizeof(field));
			same = error == 0 && strcmp(field, "fido2") == 0;
			if (same)
				error = passkey_record_field(all->lines[all->count], 0U, all->names[all->count], sizeof(all->names[0]));
			if (same && error == 0)
				error = fido2_record_parse(all->lines[all->count], &all->records[all->count]);
			if (same && error == 0)
				all->count++;
		}

		/* The next line. */
		start = end + 1U;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Tells whose the key there is (ws199-p001 section 4.2): the accounts
 * looked for are the one named, or with "-" every person's account that
 * may log in; the key is asked silently which of them it holds, and the
 * answer of the one it holds is checked against that account's own public
 * keys (an answer that does not verify names nobody).  "ok uid=N user=NAME
 * key-pin=0|1 key-touch=0|1 card=0|1", or "fail none" (no account's),
 * "fail many-owners" (two accounts' or more), "fail no-key",
 * "fail many-keys", or another failure.  Nothing is written and nothing is
 * counted.
 */
static int
main_key_owner(
	const char *name)
{
	static struct fido2_job job;
	static struct fido2_message message;
	struct passkey_options options;
	struct pk_credential allowed[FIDO2_IDS_MAX];
	struct pk_expectation expectation;
	struct pk_assertion assertion;
	struct main_owners *owners;
	struct fido2_record *record;
	char extra[160];
	uint32_t count;
	size_t matched;
	size_t allowed_count;
	size_t length;
	size_t index;
	unsigned group;
	int same;
	int error;

	/* Every account's key lines, and those of the accounts looked for, grouped by account. */
	error = main_all_keys(&main_all);
	if (error != 0)
		return main_fail("internal");
	owners = &main_owners;
	error = main_owners_gather(name, &main_all, owners);
	if (error != 0)
		return main_fail("internal");

	/* The job: a hash nobody signs anything else with, the credentials and their groups. */
	memset(&job, 0, sizeof(job));
	job.kind = FIDO2_JOB_OWNER;
	error = pk_crypto_random(job.client_data_hash, sizeof(job.client_data_hash));
	if (error != 0)
		return main_fail("internal");
	for (index = 0U; index < owners->count; index++) {
		record = &main_all.records[owners->records[index]];
		job.ids[index] = record->id;
		job.id_sizes[index] = record->id_size;
	}

	/* Their groups (the job was cleared), and as many as there are. */
	for (index = 0U; index < owners->count; index++) {
		for (group = 0U; group < owners->group_count; group++) {
			same = strcmp(main_all.names[owners->records[index]], owners->names[group]);
			if (same == 0)
				job.groups[index] = group;
		}
	}

	/* As many as there are. */
	job.id_count = owners->count;

	/* The helper's answer. */
	error = main_run(&job, &message);
	if (error != 0 || message.kind != FIDO2_MESSAGE_OWNER)
		return main_job_failed(error, &message, FIDO2_MESSAGE_OWNER);

	/* No account's key, or more than one account's. */
	if (message.held == 0U)
		return main_fail("none");
	if ((message.held & (message.held - 1U)) != 0U)
		return main_fail("many-owners");

	/* The one group held. */
	group = 0U;
	while ((message.held & (1U << group)) == 0U)
		group++;
	if (group >= owners->group_count)
		return main_fail("none");

	/*
	 * What is expected: the login's relying party, this hash, nothing of
	 * the user (a silent answer), that account's credentials; the stored
	 * counts are not compared (nothing is kept, and the login itself checks
	 * them).
	 */
	allowed_count = 0U;
	for (index = 0U; index < owners->count; index++) {
		if (job.groups[index] != group)
			continue;
		record = &main_all.records[owners->records[index]];
		allowed[allowed_count].id = record->id;
		allowed[allowed_count].id_size = record->id_size;
		allowed[allowed_count].cose_key = record->cose_key;
		allowed[allowed_count].cose_key_size = record->cose_key_size;
		allowed[allowed_count].sign_count = 0U;
		allowed_count++;
	}

	/* The relying party, the hash, no flags, the account's keys. */
	memset(&expectation, 0, sizeof(expectation));
	expectation.rp_id = FIDO2_RP;
	memcpy(expectation.client_data_hash, job.client_data_hash, sizeof(expectation.client_data_hash));
	expectation.required_flags = 0U;
	expectation.credentials = allowed;
	expectation.credential_count = allowed_count;

	/* The answer, checked here: one that does not verify names nobody. */
	assertion.credential_id = message.id;
	assertion.credential_id_size = message.id_size;
	assertion.auth_data = message.auth_data;
	assertion.auth_data_size = message.auth_data_size;
	assertion.signature = message.signature;
	assertion.signature_size = message.signature_size;
	error = pk_verify_assertion(&expectation, &assertion, &matched, &count);
	if (error != 0)
		return main_fail("none");

	/* The owner's options, read as the login reads them. */
	passkey_options_default(&options);
	error = main_file(&length, 1);
	if (error == 0)
		(void)passkey_options_read(main_text, length, owners->names[group], owners->uids[group], &options);

	/* Succeeded: the owner. */
	(void)snprintf(extra, sizeof(extra), "user=%s key-pin=%d key-touch=%d card=%u", owners->names[group], options.key_pin,
	    options.key_touch, message.owner_card);
	return main_ok(owners->uids[group], extra);
}

/*
 * Picks the key lines an owner's question looks for: those of the account
 * named (or of every account, "-") whose account is in passwd with the
 * line's user ID and may use a key; grouped by account in their order.
 * Returns 0 or an errno value.
 */
static int
main_owners_gather(
	const char *name,
	const struct main_all_keys *all,
	struct main_owners *owners)
{
	size_t index;
	int every;
	int same;
	int group;

	/* Every account, or the one named. */
	memset(owners, 0, sizeof(*owners));
	every = 0;
	same = strcmp(name, "-");
	if (same == 0)
		every = 1;

	/* Each line of an account looked for, whose account may use it. */
	for (index = 0U; index < all->count && owners->count < FIDO2_IDS_MAX; index++) {
		if (!every) {
			same = strcmp(all->names[index], name);
			if (same != 0)
				continue;
		}

		/* Its account's group; a line of an account that may not use a key is left out. */
		group = main_owner_group(owners, all->names[index], all->lines[index]);
		if (group < 0)
			continue;
		owners->records[owners->count] = index;
		owners->count++;
	}

	/* Succeeded: the lines and their groups. */
	return 0;
}

/*
 * Finds or adds the group of a line's account: in passwd with the line's
 * user ID, a person's, its password neither locked nor expired.  Returns
 * the group, or -1 for an account that may not use a key.
 */
static int
main_owner_group(
	struct main_owners *owners,
	const char *name,
	const char *line)
{
	struct passwd account;
	char strings[LOGIN_VERIFY_BUFFER];
	char field[32];
	unsigned long uid;
	size_t length;
	size_t group;
	char *end;
	int found;
	int usable;
	int same;
	int error;

	/* A group already made for the account. */
	for (group = 0U; group < owners->group_count; group++) {
		same = strcmp(owners->names[group], name);
		if (same == 0)
			return (int)group;
	}

	/* The line's user ID. */
	error = passkey_record_field(line, 1U, field, sizeof(field));
	if (error != 0)
		return -1;
	uid = strtoul(field, &end, 10);
	if (end == field || *end != '\0')
		return -1;

	/* The account in passwd, with that user ID, that may use a key. */
	found = main_account(name, &account, strings, sizeof(strings));
	if (!found || (unsigned long)account.pw_uid != uid)
		return -1;
	usable = main_usable(name, account.pw_uid);
	if (!usable || owners->group_count >= FIDO2_IDS_MAX)
		return -1;

	/* An account that turned its keys off for the screens owns none there (WS200). */
	usable = main_key_method_on(name, account.pw_uid);
	if (!usable)
		return -1;

	/* A name the group can hold. */
	length = strlen(name);
	if (length >= sizeof(owners->names[0]))
		return -1;

	/* A new group. */
	(void)snprintf(owners->names[owners->group_count], sizeof(owners->names[0]), "%s", name);
	owners->uids[owners->group_count] = account.pw_uid;
	owners->group_count++;

	/* Succeeded: the new group. */
	return (int)(owners->group_count - 1U);
}

/*
 * Waits for the key to be plugged in again: "status replug", then a USB
 * key that was there must go and one come back, or a card must come to a
 * reader (one already there taken away and held again).  Returns 0 as soon
 * as one key is there, EEXIST for two USB keys, ECANCELED when sessiond
 * ended the work, ETIMEDOUT, or another errno value.
 */
static int
main_replug(void)
{
	struct pk_os_device found[PK_OS_DEVICES_MAX];
	struct pk_os_card cards[PK_OS_DEVICES_MAX];
	struct timespec step;
	uint64_t deadline;
	uint64_t now;
	size_t card_count;
	size_t count;
	size_t index;
	int inserted;
	int gone;
	int came;
	int error;

	/* The readers' slots, attached to hear their cards. */
	card_count = 0U;
	error = pk_os_list_slots(found, PK_OS_DEVICES_MAX, &count);
	if (error != 0)
		count = 0U;
	for (index = 0U; index < count; index++) {
		error = pk_os_card_attach(&cards[card_count], found[index].path);
		if (error == 0)
			card_count++;
	}

	/* The USB keys now: one there must go first. */
	error = pk_os_list(found, PK_OS_DEVICES_MAX, &count);
	gone = error != 0 || count == 0U;

	/* Asked. */
	printf("status replug\n");
	(void)fflush(stdout);

	/* Until a key comes back, the work ends, or the time is out. */
	deadline = main_now_ms() + MAIN_REPLUG_MS;
	came = 0;
	error = ETIMEDOUT;
	for (;;) {
		/* Ended by sessiond. */
		now = main_now_ms();
		if (fido2_ended) {
			error = ECANCELED;
			break;
		}

		/* Out of time. */
		if (now >= deadline)
			break;

		/* A card that came to a reader. */
		for (index = 0U; index < card_count && !came; index++) {
			for (;;) {
				error = pk_os_card_event(&cards[index], &inserted);
				if (error != 0)
					break;
				if (inserted)
					came = 1;
			}
		}

		/* One came. */
		if (came) {
			error = 0;
			break;
		}

		/* The USB keys: none (it went), then one or more. */
		error = pk_os_list(found, PK_OS_DEVICES_MAX, &count);
		if (error != 0)
			count = 0U;
		if (count == 0U)
			gone = 1;
		if (gone && count == 1U) {
			error = 0;
			break;
		}

		/* Two or more. */
		if (gone && count > 1U) {
			error = EEXIST;
			break;
		}

		/* A moment. */
		step.tv_sec = 0;
		step.tv_nsec = (long)MAIN_REPLUG_STEP_MS * 1000000L;
		(void)nanosleep(&step, NULL);
		error = ETIMEDOUT;
	}

	/* The slots let go. */
	for (index = 0U; index < card_count; index++)
		pk_os_card_close(&cards[index]);
	return error;
}

/* Answers a job that did not give the message expected: the helper's own failure, a timeout, or the device. */
static int
main_job_failed(
	int error,
	const struct fido2_message *message,
	int kind)
{
	/* The run itself failed. */
	if (error == ETIMEDOUT)
		return main_fail("timeout");
	if (error != 0)
		return main_fail("device");

	/* The helper's own failure is the answer; any other message is the device's fault. */
	(void)kind;
	if (message->kind == FIDO2_MESSAGE_FAIL)
		return main_fail(message->reason);
	return main_fail("device");
}

/*
 * Sets the account's key's PIN and touch back to asked, under the account
 * files' lock (its last key went): the line keeps WS200's methods, or goes
 * when it is all defaults.  Returns 0 or an errno value.
 */
static int
main_options_reset(
	const char *name,
	uid_t uid)
{
	struct passkey_options options;
	char line[MAIN_LINE_MAX];
	const char *added;
	sigset_t held;
	sigset_t previous;
	size_t length;
	size_t written;
	int defaults;
	int error;

	/* The signals that would stop the change are held; the lock. */
	sigfillset(&held);
	(void)sigprocmask(SIG_BLOCK, &held, &previous);
	error = account_files_lock();
	if (error != 0) {
		(void)sigprocmask(SIG_SETMASK, &previous, NULL);
		return error;
	}

	/* The file read again under the lock; the options' key part back to the defaults. */
	passkey_options_default(&options);
	error = main_file(&length, 0);
	if (error == 0) {
		(void)passkey_options_read(main_text, length, name, uid, &options);
		options.key_pin = 1;
		options.key_touch = 1;
		error = passkey_options_line(name, uid, &options, line, sizeof(line));
	}

	/* The defaults need no line. */
	defaults = passkey_options_is_default(&options);
	added = line;
	if (defaults)
		added = NULL;
	if (error == 0)
		error = passkey_record_replace(main_text, length, name, "options", added, main_output, sizeof(main_output), &written);
	if (error == 0)
		error = account_file_write(PASSKEY_FILE, 0600, main_output, written);

	/* The lock and the signals given back. */
	account_files_unlock();
	(void)sigprocmask(SIG_SETMASK, &previous, NULL);
	return error;
}

/* Gives the monotonic time in milliseconds. */
static uint64_t
main_now_ms(void)
{
	struct timespec now;

	/* The clock that does not go back. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Gives passkey's reason for an answer that did not verify. */
static const char *
main_verify_reason(
	int error)
{
	/* The check that failed. */
	switch (error) {
	case PK_VERIFY_NOT_ALLOWED:
		return "no-key";
	case PK_VERIFY_FLAGS:
	case PK_VERIFY_SIGNATURE:
		return "bad-secret";
	case PK_VERIFY_REPLAY:
		return "cloned";
	default:
		break;
	}

	/* A malformed answer, or another party's. */
	return "device";
}

/*
 * Tells whether the account takes a security key on the login and locked
 * screens (WS200): fido2 among the methods of its options line (every
 * method without one, or when the file does not read).
 */
static int
main_key_method_on(
	const char *name,
	uid_t uid)
{
	struct passkey_options options;
	size_t length;
	unsigned methods;
	int error;

	/* The account's options as the file has them. */
	passkey_options_default(&options);
	error = main_file(&length, 1);
	if (error == 0)
		(void)passkey_options_read(main_text, length, name, uid, &options);
	methods = passkey_options_methods(&options);

	/* A key among them. */
	if ((methods & PASSKEY_METHOD_FIDO2) == 0U)
		return 0;

	/* Taken. */
	return 1;
}
