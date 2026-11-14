/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libFuzzer target: the monitor answering a compromised child.  The input
 * picks the stage to start from and is then a run of frames (byte type,
 * uint16 length, body) handed to sshd_monitor_handle.
 *
 * The account checks of sshd-login.c are replaced here, so nothing reaches
 * the host's accounts or files: only the user "fuzzuser" exists; its one
 * authorized key is RFC 8032 7.1 TEST 2's; its password is a secret made of
 * random bytes at the start, so no input can know it; a login record
 * aborts (on this host the monitor's parent check refuses every one).  The
 * invariants checked after every frame: no login is granted (a grant needs
 * a signature by the authorized key over the monitor's own session
 * identifier, or the secret password), the attempts stay within
 * MaxAuthTries, the pre-authentication signatures within their bound.  At
 * the start a real signed login is made once, to show that a grant would
 * be seen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssh-auth.h"
#include "sshd-monitor.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv);

static struct sshd_config fuzz_config;
static struct ssh_key fuzz_host_key;
static struct ssh_key fuzz_user_key;
static uint8_t fuzz_secret_digest[SSH_SHA256_SIZE];

static const uint8_t fuzz_user_seed[32] = {
	0x4c, 0xcd, 0x08, 0x9b, 0x28, 0xff, 0x96, 0xda, 0x9d, 0xb6, 0xc3, 0x46, 0xec, 0x11, 0x4e, 0x0f,
	0x5b, 0x8a, 0x31, 0x9f, 0x35, 0xab, 0xa6, 0x24, 0xda, 0x8c, 0xf6, 0xed, 0x4f, 0xb8, 0xa6, 0xfb
};

/* The replaced account checks. */
int
sshd_account_allowed(const struct sshd_config *config, const char *user, int method, struct passwd *account, char *buffer, size_t size)
{
	int different;

	/* The arguments the replacement does not look at. */
	(void)config;
	(void)method;
	(void)size;

	/* Admits only the one user the harness knows. */
	different = strcmp(user, "fuzzuser");
	if (different != 0)
		return ENOENT;

	/* Describes that user's account with fixed identifiers. */
	memset(account, 0, sizeof(*account));
	strcpy(buffer, "fuzzuser");
	account->pw_name = buffer;
	account->pw_uid = 4242;
	account->pw_gid = 4242;

	/* Succeeded: the user is allowed. */
	return 0;
}

int
sshd_key_authorized(const struct sshd_config *config, const struct passwd *account, const uint8_t *public_key)
{
	int different;

	/* The arguments the replacement does not look at. */
	(void)config;
	(void)account;

	/* Compares the offered key with the one authorized key. */
	different = memcmp(public_key, fuzz_user_key.public_key, SSH_ED25519_PUBLIC);

	/* Authorizes only the key the harness holds. */
	if (different != 0)
		return EACCES;

	/* Succeeded: the key is the authorized one. */
	return 0;
}

int
sshd_password_ok(const struct sshd_config *config, const char *user, char *password)
{
	uint8_t digest[SSH_SHA256_SIZE];
	int same;
	int different;

	/* The argument the replacement does not look at. */
	(void)config;

	/* Hashes the offered password and compares it with the digest of the secret. */
	(void)ssh_sha256(password, strlen(password), digest);
	same = ssh_ct_equal(digest, fuzz_secret_digest, sizeof(digest));
	ssh_wipe(password, strlen(password));

	/* Accepts only the secret, for the one user. */
	different = strcmp(user, "fuzzuser");
	if (same == 0 || different != 0)
		return EACCES;

	/* Succeeded: the password is the secret. */
	return 0;
}

int
sshd_login_record(const char *user, const char *tty, pid_t pid, int login)
{
	/* The arguments the replacement does not look at. */
	(void)user;
	(void)tty;
	(void)pid;
	(void)login;

	/* Any login record means the parent check was bypassed, which the fuzzer must see. */
	fprintf(stderr, "a login record was written without the parent check\n");
	abort();
}

int
sshd_tty_path_ok(const char *tty)
{
	int different;

	/* Compares the path with the pseudo-terminal directory. */
	different = strncmp(tty, "/dev/pts/", 9);

	/* Accepts only a name under /dev/pts. */
	if (different != 0 || tty[9] == '\0')
		return EINVAL;

	/* Succeeded: the path names a pseudo-terminal. */
	return 0;
}

int
sshd_tty_check(const char *tty, uid_t uid)
{
	int error;

	/* The argument the replacement does not look at. */
	(void)uid;

	/* Checks the path the way the real check does first. */
	error = sshd_tty_path_ok(tty);
	if (error != 0)
		return EACCES;

	/* Succeeded: the harness owns every pseudo-terminal. */
	return 0;
}

void
syslog(int priority, const char *format, ...)
{
	(void)priority;
	(void)format;
}

static void
fuzz_check_invariants(const struct sshd_monitor *monitor, int start)
{
	/* A grant, or a session stage the start did not set up, is a bypass of authentication. */
	if (monitor->stage == SSHD_STAGE_GRANTED || (start != 2 && monitor->stage == SSHD_STAGE_SESSION)) {
		fprintf(stderr, "a login was granted\n");
		abort();
	}


	/* The attempts must stay within the configured limit. */
	if (monitor->tries > fuzz_config.max_auth_tries) {
		fprintf(stderr, "more attempts than MaxAuthTries\n");
		abort();
	}


	/* The signatures made before authentication must stay within their bound. */
	if (monitor->preauth_signs > SSHD_PREAUTH_SIGN_MAX) {
		fprintf(stderr, "more signatures than the pre-authentication bound\n");
		abort();
	}
}

static int
fuzz_frame(struct sshd_monitor *monitor, unsigned type, const uint8_t *body, size_t size)
{
	struct ssh_buf reply;
	struct timespec now;
	enum sshd_monitor_next next;
	int status;

	/* Hands the frame to the monitor with a fixed clock and a reply buffer. */
	ssh_buf_init(&reply, SSH_MON_FRAME_MAX + 16U, 0);
	now.tv_sec = 0;
	now.tv_nsec = 0;
	status = sshd_monitor_handle(monitor, (enum ssh_mon_type)type, body, size, &now, &reply, &next);
	ssh_buf_free(&reply);

	/* Reports whether the monitor wants the connection to go on. */
	return status;
}

static void
fuzz_start(struct sshd_monitor *monitor)
{
	/* Starts a monitor with no delays, so that the run is not slowed. */
	sshd_monitor_init(monitor, &fuzz_config, &fuzz_host_key, 0);
	monitor->fail_delay_ms = 0;
	monitor->sign_interval_ms = 0;
}

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
	static const uint8_t host_seed[32] = {
		0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
		0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60
	};
	struct sshd_monitor monitor;
	struct ssh_buf signed_data;
	struct ssh_buf body;
	struct ssh_buf blob;
	struct ssh_buf signature;
	uint8_t h[SSH_SHA256_SIZE];
	uint8_t secret[32];
	int status;

	/* The arguments the harness does not look at. */
	(void)argc;
	(void)argv;

	/* Sets up the configuration, the host key, the user key and the secret password digest. */
	sshd_config_defaults(&fuzz_config);
	fuzz_config.max_auth_tries = 6;
	memcpy(fuzz_host_key.seed, host_seed, 32);
	(void)ssh_ed25519_public(fuzz_host_key.seed, fuzz_host_key.public_key);
	fuzz_host_key.private = 1;
	memcpy(fuzz_user_key.seed, fuzz_user_seed, 32);
	(void)ssh_ed25519_public(fuzz_user_key.seed, fuzz_user_key.public_key);
	fuzz_user_key.private = 1;
	(void)ssh_random(secret, sizeof(secret));
	(void)ssh_sha256(secret, sizeof(secret), fuzz_secret_digest);
	ssh_wipe(secret, sizeof(secret));

	/* A real signed login is granted: the check after every frame sees grants. */
	fuzz_start(&monitor);
	memset(h, 0x11, sizeof(h));
	ssh_buf_init(&body, 4096, 0);
	ssh_buf_put_string(&body, h, sizeof(h));
	status = fuzz_frame(&monitor, SSH_MON_SIGN, body.data, body.size);
	if (status != 0)
		abort();

	/* Builds the data a client signs to log in with the key. */
	ssh_buf_init(&blob, 128, 0);
	ssh_key_put_public_blob(fuzz_user_key.public_key, &blob);
	ssh_buf_init(&signed_data, 4096, 0);
	ssh_buf_put_string(&signed_data, h, sizeof(h));
	ssh_buf_put_u8(&signed_data, SSH_MSG_USERAUTH_REQUEST);
	ssh_buf_put_cstring(&signed_data, "fuzzuser");
	ssh_buf_put_cstring(&signed_data, SSH_SERVICE_CONNECTION);
	ssh_buf_put_cstring(&signed_data, "publickey");
	ssh_buf_put_bool(&signed_data, 1);
	ssh_buf_put_cstring(&signed_data, SSH_KEY_NAME);
	ssh_buf_put_string(&signed_data, blob.data, blob.size);
	ssh_buf_init(&signature, 256, 0);
	(void)ssh_key_sign(&fuzz_user_key, signed_data.data, signed_data.size, &signature);
	ssh_buf_reset(&body);
	ssh_buf_put_cstring(&body, "fuzzuser");
	ssh_buf_put_string(&body, blob.data, blob.size);
	ssh_buf_put_string(&body, signed_data.data, signed_data.size);
	ssh_buf_put_string(&body, signature.data, signature.size);
	/* Asks the monitor to log in with that signature. */
	status = fuzz_frame(&monitor, SSH_MON_PUBKEY_LOGIN, body.data, body.size);

	/* The login must be granted, or the harness could not see a grant. */
	if (status != 0 || monitor.stage != SSHD_STAGE_GRANTED) {
		fprintf(stderr, "the harness's own login was not granted\n");
		abort();
	}

	/* Releases everything the login used. */
	sshd_monitor_clear(&monitor);
	ssh_buf_free(&body);
	ssh_buf_free(&blob);
	ssh_buf_free(&signed_data);
	ssh_buf_free(&signature);

	/* Succeeded: the harness is ready. */
	return 0;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct sshd_monitor monitor;
	struct ssh_buf body;
	uint8_t h[SSH_SHA256_SIZE];
	unsigned type;
	size_t length;
	int start;
	int status;

	/* An empty input selects nothing to exercise. */
	if (size == 0)
		return 0;

	/* The first byte picks the stage to start from. */
	start = data[0] % 3;
	data++;
	size--;

	/* Starts a fresh monitor. */
	fuzz_start(&monitor);

	/* Signs the exchange first when the start is past the key exchange. */
	if (start >= 1) {
		/* The exchange is signed first, as a real child does. */
		memset(h, 0x22, sizeof(h));
		ssh_buf_init(&body, 64, 0);
		ssh_buf_put_string(&body, h, sizeof(h));
		(void)fuzz_frame(&monitor, SSH_MON_SIGN, body.data, body.size);
		ssh_buf_free(&body);
	}

	/* Moves the monitor to the stage of a session process when the start asks for it. */
	if (start == 2) {
		/* A session process: the stage as after a hand-over, for a user the monitor knows. */
		monitor.stage = SSHD_STAGE_SESSION;
		strcpy(monitor.user, "fuzzuser");
		monitor.have_user = 1;
		monitor.user_uid = 4242;
		monitor.session_pid = 4242;
	}

	/* Feeds the monitor one frame at a time and checks the invariants after each. */
	while (size >= 3) {
		type = data[0];
		length = ((size_t)data[1] << 8) | data[2];
		data += 3;
		size -= 3;
		if (length > size)
			length = size;
		status = fuzz_frame(&monitor, type, data, length);
		data += length;
		size -= length;
		fuzz_check_invariants(&monitor, start);
		if (status != 0)
			break;
	}

	/* Releases the monitor. */
	sshd_monitor_clear(&monitor);

	/* Succeeded: the input was handled without a crash. */
	return 0;
}
