/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of sshd's monitor (sshd-monitor.c) and account checks
 * (sshd-login.c), on a development host without root.
 *
 * The requests are built with wire.h (apart from the modules) as a
 * compromised child would send them, one field wrong at a time; the
 * expected answers come from the contracts of ssh-privsep.h, sshd.h and
 * sshd-monitor.h and from RFC 4252 7 (the signed data).  The keys are
 * those of RFC 8032 7.1 TEST 1 (the host key) and TEST 2 (the user key).
 * crypt and syslog are replaced here, so the test counts the hashing a
 * password check does and keeps the log off the host.  The whole monitor
 * runs with this program itself as its children (the fake
 * pre-authentication and session processes at the end of the file).
 *
 * run-unit-sources: userland/base/ssh/sshd-monitor.c userland/base/ssh/sshd-login.c userland/base/login/verify.c
 */

/* The host's pseudo-terminal calls (ptsname_r) and environ; the modules under test do not depend on this. */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "wire.h"
#include "ssh-hostfile.h"
#include "sshd-monitor.h"

/* The salt text of the dummy hash sshd-login.c gives crypt (SHA-512 crypt, a 16-character salt). */
#define TEST_DUMMY_PREFIX	"$6$zedbsdsshddummy0$"

/* The peer text the run tests give the monitor. */
#define TEST_PEER		"test-peer"

/* The exported transport the fake pre-authentication process hands over. */
#define TEST_STATE		"exported-transport-bytes"

/* The room for paths under the work directory. */
#define TEST_PATH_MAX		1024U

/* RFC 8032 7.1 TEST 1: the host key's seed and public key. */
static const char host_seed_hex[] = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
static const char host_public_hex[] = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";

/* RFC 8032 7.1 TEST 2: the user key's seed and public key. */
static const char user_seed_hex[] = "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
static const char user_public_hex[] = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";

/* The keys, their blobs (built with wire.h), and the session identifier the tests sign first. */
static struct ssh_key host_key;
static struct ssh_key user_key;
static struct wire host_blob;
static struct wire user_blob;
static uint8_t session_h[SSH_SHA256_SIZE];
static uint8_t other_h[SSH_SHA256_SIZE];

/* The account the test runs as, the work directory, and the authorized_keys file the handler tests use. */
static char self_name[SSH_USER_MAX + 1];
static char work[TEST_PATH_MAX];
static char keys_path[TEST_PATH_MAX];
static char program_path[TEST_PATH_MAX];

/* The settings of the handler tests: the file above, no StrictModes (the directories above the work directory are not the test's). */
static struct sshd_config base_config;

/* The request, the signed data and a scratch message being built; static because they are large. */
static struct wire request;
static struct wire signed_data;
static struct wire scratch;

/* What the replacement crypt saw: how often it ran, the last salt, and the answer it gives (kept writable, since the caller wipes it). */
static unsigned long crypt_calls;
static char crypt_salt[128];
static char crypt_answer[64];

/* How many lines the replacement syslog took, and the last one. */
static unsigned long log_count;
static char log_last[512];

static void setup(const char *argv0);
static void setup_files(void);
static void load_key(struct ssh_key *key, const char *seed_hex, const char *public_hex);
static void put_blob(struct wire *wire, const struct ssh_key *key);
static void put_key_line(struct wire *wire, const struct wire *blob);
static void write_file(const char *path, const void *data, size_t size, mode_t mode);
static void join(char *out, const char *left, const char *right);
static void fresh(struct sshd_monitor *monitor, const struct sshd_config *config);
static int call(struct sshd_monitor *monitor, unsigned type, const struct wire *body, unsigned *reply_type, uint32_t *result, struct wire *reply_body, enum sshd_monitor_next *next);
static int sign_h(struct sshd_monitor *monitor, const uint8_t *h, size_t size);
static void build_signed(struct wire *wire, const uint8_t *sid, size_t sid_size, unsigned number, const char *user, const char *service, const char *method, unsigned flag, const char *algorithm, const struct wire *blob, int trailing);
static void build_login(struct wire *wire, const char *user, const struct wire *blob, const struct wire *data, const struct ssh_key *signer, int trailing);
static void build_query(struct wire *wire, const char *user, const struct wire *blob);
static void build_password(struct wire *wire, const char *user, const char *password, size_t size);
static void build_record(struct wire *wire, const char *tty, size_t tty_size, uint32_t pid, int trailing);
static int login_good(struct sshd_monitor *monitor);
static void build_right(unsigned type);
static long elapsed_ms(const struct timespec *start);
static void test_config_text(void);
static void test_account_policy(void);
static void test_account_allowed(void);
static void test_password(void);
static void test_authorized_path(void);
static void test_key_authorized(void);
static void test_tty(void);
static void test_stages(void);
static void test_sign(void);
static void test_signed_data(void);
static void test_login_malformed(void);
static void test_attempts(void);
static void test_password_requests(void);
static void test_timing(void);
static void test_session_sign(void);
static void test_records(void);
static void test_run(void);
static int run_scenario(const char *codes, const struct sshd_config *config, int bad_blob, char *report, size_t capacity, int *grant_byte, long *took);
static int child_parent_holds_connection(void);
static int child_read_frame(int descriptor, struct ssh_buf *input, enum ssh_mon_type *type, struct ssh_buf *body);
static int child_check_config(const struct ssh_buf *body);
static void child_report(const char *text);
static int child_other_descriptors(void);
static void fake_preauth(void);
static void fake_session(void);

/*
 * Counts a password hashing and gives a fixed answer that matches no
 * stored hash.
 */
char *
crypt(
	const char *key,
	const char *salt)
{
	/* Records the call. */
	(void)key;
	crypt_calls++;
	strncpy(crypt_salt, salt, sizeof(crypt_salt) - 1U);
	crypt_salt[sizeof(crypt_salt) - 1U] = '\0';

	/* Answers a hash that is no account's. */
	strcpy(crypt_answer, "$6$test$not-a-match");
	return crypt_answer;
}

/*
 * Keeps a log line instead of sending it to the host's log.
 */
void
syslog(
	int priority,
	const char *format,
	...)
{
	va_list arguments;

	/* Keeps the line. */
	(void)priority;
	log_count++;
	va_start(arguments, format);
	vsprintf(log_last, format, arguments);
	va_end(arguments);
}

/*
 * Runs the checks, or, started by the monitor, one of its fake children.
 */
int
main(
	int argc,
	char **argv)
{
	/* Plays the child the monitor started, which knows the same keys and paths. */
	setup(argv[0]);
	if (argc >= 2 && strcmp(argv[1], SSHD_EXEC_PREAUTH) == 0)
		fake_preauth();
	if (argc >= 2 && strcmp(argv[1], SSHD_EXEC_SESSION) == 0)
		fake_session();

	/* Prints each failure as it happens. */
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGPIPE, SIG_IGN);
	umask(022);
	setup_files();

	/* Runs each group of checks. */
	test_config_text();
	test_account_policy();
	test_account_allowed();
	test_password();
	test_authorized_path();
	test_key_authorized();
	test_tty();
	test_stages();
	test_sign();
	test_signed_data();
	test_login_malformed();
	test_attempts();
	test_password_requests();
	test_timing();
	test_session_sign();
	test_records();
	test_run();

	/* Reports the whole test. */
	return check_finish("monitor");
}

/* Loads the keys, finds the account, and names the work directory and the authorized_keys file. */
static void
setup(
	const char *argv0)
{
	struct passwd *self;
	char *resolved;
	char *slash;
	char directory[TEST_PATH_MAX];

	/* The keys of RFC 8032 7.1 and their blobs. */
	load_key(&host_key, host_seed_hex, host_public_hex);
	load_key(&user_key, user_seed_hex, user_public_hex);
	put_blob(&host_blob, &host_key);
	put_blob(&user_blob, &user_key);
	memset(session_h, 0x5a, sizeof(session_h));
	session_h[0] = 0x01;
	memset(other_h, 0xa5, sizeof(other_h));

	/* The account the test runs as. */
	self = getpwuid(getuid());
	CHECK(self != NULL);
	if (self == NULL)
		exit(1);
	strncpy(self_name, self->pw_name, sizeof(self_name) - 1U);

	/* This program's own path, for the monitor to start as its children. */
	resolved = realpath(argv0, NULL);
	CHECK(resolved != NULL);
	if (resolved == NULL)
		exit(1);
	strncpy(program_path, resolved, sizeof(program_path) - 1U);
	free(resolved);

	/* The work directory beside the program (run-unit's fresh run directory). */
	strncpy(directory, program_path, sizeof(directory) - 1U);
	slash = strrchr(directory, '/');
	if (slash != NULL)
		*slash = '\0';
	(void)snprintf(work, sizeof(work), "%s/monitor-work", directory);
	join(keys_path, work, "authorized_keys");

	/* The settings of the handler tests. */
	sshd_config_defaults(&base_config);
	strcpy(base_config.authorized_keys_file, keys_path);
	base_config.strict_modes = 0;
	base_config.password = 1;
	base_config.pubkey = 1;
	base_config.max_auth_tries = 6;
	(void)sshd_config_finish(&base_config);
}

/* Makes the work directory with the authorized_keys file of the handler tests (a comment and the user key). */
static void
setup_files(void)
{
	int made;

	/* The directory, new in run-unit's fresh run directory. */
	made = mkdir(work, 0755);
	CHECK(made == 0 || errno == EEXIST);

	/* The file. */
	wire_reset(&scratch);
	wire_bytes(&scratch, "# keys\n", 7);
	put_key_line(&scratch, &user_blob);
	write_file(keys_path, scratch.data, scratch.size, 0600);
}

/* Fills key from the hex of RFC 8032's seed and public key. */
static void
load_key(
	struct ssh_key *key,
	const char *seed_hex,
	const char *public_hex)
{
	size_t count;

	/* Reads both halves. */
	memset(key, 0, sizeof(*key));
	count = wire_hex(seed_hex, key->seed, sizeof(key->seed));
	CHECK_EQ(count, SSH_ED25519_SEED);
	count = wire_hex(public_hex, key->public_key, sizeof(key->public_key));
	CHECK_EQ(count, SSH_ED25519_PUBLIC);
	key->private = 1;
}

/* Builds the RFC 8709 4 public key blob of key: string "ssh-ed25519", string of the 32 bytes. */
static void
put_blob(
	struct wire *wire,
	const struct ssh_key *key)
{
	/* The two strings. */
	wire_reset(wire);
	wire_text(wire, "ssh-ed25519");
	wire_string(wire, key->public_key, SSH_ED25519_PUBLIC);
	CHECK_EQ(wire->size, SSH_KEY_BLOB_SIZE);
}

/* Appends the authorized_keys line "ssh-ed25519 <base64 of blob> test\n" to wire. */
static void
put_key_line(
	struct wire *wire,
	const struct wire *blob)
{
	char text[128];
	int encoded;

	/* Encodes the blob and writes the line. */
	encoded = ssh_base64_encode(blob->data, blob->size, text, sizeof(text), 1);
	CHECK_EQ(encoded, 0);
	wire_bytes(wire, "ssh-ed25519 ", 12);
	wire_bytes(wire, text, strlen(text));
	wire_bytes(wire, " test\n", 6);
}

/* Writes size bytes to a new file at path with mode (replacing an earlier one's contents). */
static void
write_file(
	const char *path,
	const void *data,
	size_t size,
	mode_t mode)
{
	int descriptor;
	ssize_t written;

	/* Creates or truncates the file, writes it, and sets the mode exactly. */
	descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	CHECK(descriptor >= 0);
	if (descriptor < 0)
		return;
	written = write(descriptor, data, size);
	CHECK_EQ(written, (long)size);
	(void)close(descriptor);
	(void)chmod(path, mode);
}

/* Writes "left/right" into out (TEST_PATH_MAX). */
static void
join(
	char *out,
	const char *left,
	const char *right)
{
	/* Joins with one slash. */
	(void)snprintf(out, TEST_PATH_MAX, "%s/%s", left, right);
}

/* Starts a monitor for the handler tests, without the waits (the timing tests set them back). */
static void
fresh(
	struct sshd_monitor *monitor,
	const struct sshd_config *config)
{
	/* A monitor of a host without root. */
	sshd_monitor_init(monitor, config, &host_key, 0);
	monitor->fail_delay_ms = 0;
	monitor->sign_interval_ms = 0;
}

/*
 * Hands one request to the monitor and reads its reply frame by hand: a
 * uint32 length that counts the type and the body, the type, the body.
 * *result is the uint32 of a RESULT (0xffffffff otherwise).  Returns
 * what the monitor returned.
 */
static int
call(
	struct sshd_monitor *monitor,
	unsigned type,
	const struct wire *body,
	unsigned *reply_type,
	uint32_t *result,
	struct wire *reply_body,
	enum sshd_monitor_next *next)
{
	struct ssh_buf reply;
	struct timespec arrived;
	enum sshd_monitor_next local_next;
	uint32_t length;
	int handled;

	/* Hands the request over as it arrived now. */
	ssh_buf_init(&reply, SSH_MON_FRAME_MAX + 16U, 0);
	(void)clock_gettime(CLOCK_MONOTONIC, &arrived);
	local_next = SSHD_NEXT_REPLY;
	handled = sshd_monitor_handle(monitor, (enum ssh_mon_type)type, body->data, body->size, &arrived, &reply, &local_next);
	*reply_type = 0;
	*result = 0xffffffffUL;
	if (reply_body != NULL)
		wire_reset(reply_body);
	if (next != NULL)
		*next = local_next;

	/* Reads the reply frame, which must be whole and alone. */
	if (handled == 0 && local_next == SSHD_NEXT_REPLY) {
		CHECK(reply.size >= 5);
		if (reply.size >= 5) {
			length = ((uint32_t)reply.data[0] << 24) | ((uint32_t)reply.data[1] << 16) | ((uint32_t)reply.data[2] << 8) | reply.data[3];
			CHECK_EQ(length, reply.size - 4);
			*reply_type = reply.data[4];
			if (reply_body != NULL)
				wire_bytes(reply_body, reply.data + 5, reply.size - 5);
			if (reply.data[4] == SSH_MON_RESULT && reply.size == 9)
				*result = ((uint32_t)reply.data[5] << 24) | ((uint32_t)reply.data[6] << 16) | ((uint32_t)reply.data[7] << 8) | reply.data[8];
		}
	}

	if (handled != 0 || local_next != SSHD_NEXT_REPLY)
		CHECK_EQ(reply.size, 0);
	ssh_buf_free(&reply);
	return handled;
}

/* Asks the monitor to sign size bytes of h; checks a SIGNATURE that verifies with the host key; returns the monitor's answer. */
static int
sign_h(
	struct sshd_monitor *monitor,
	const uint8_t *h,
	size_t size)
{
	static struct wire body;
	static struct wire reply;
	unsigned type;
	uint32_t result;
	uint32_t length;
	int handled;
	int verified;

	/* string h. */
	wire_reset(&body);
	wire_string(&body, h, size);
	handled = call(monitor, SSH_MON_SIGN, &body, &type, &result, &reply, NULL);
	if (handled != 0)
		return handled;

	/* SIGNATURE: string of the 83-byte blob, string "ssh-ed25519" and string of 64 bytes (RFC 8709 6). */
	CHECK_EQ(type, SSH_MON_SIGNATURE);
	CHECK_EQ(reply.size, 4 + SSH_KEY_SIGNATURE_BLOB_SIZE);
	if (reply.size != 4 + SSH_KEY_SIGNATURE_BLOB_SIZE)
		return EIO;
	length = ((uint32_t)reply.data[0] << 24) | ((uint32_t)reply.data[1] << 16) | ((uint32_t)reply.data[2] << 8) | reply.data[3];
	CHECK_EQ(length, SSH_KEY_SIGNATURE_BLOB_SIZE);
	CHECK_BYTES(reply.data + 4, "\0\0\0\013ssh-ed25519\0\0\0\100", 19);
	verified = ssh_key_verify(host_key.public_key, h, size, reply.data + 4, SSH_KEY_SIGNATURE_BLOB_SIZE);
	CHECK_EQ(verified, 0);
	return 0;
}

/*
 * Builds the data a publickey signature covers (RFC 4252 7) with the
 * given fields, so one at a time can be made wrong; trailing adds a byte.
 */
static void
build_signed(
	struct wire *wire,
	const uint8_t *sid,
	size_t sid_size,
	unsigned number,
	const char *user,
	const char *service,
	const char *method,
	unsigned flag,
	const char *algorithm,
	const struct wire *blob,
	int trailing)
{
	/* The fields in order. */
	wire_reset(wire);
	wire_string(wire, sid, sid_size);
	wire_u8(wire, number);
	wire_text(wire, user);
	wire_text(wire, service);
	wire_text(wire, method);
	wire_bool(wire, flag);
	wire_text(wire, algorithm);
	if (blob != NULL)
		wire_string(wire, blob->data, blob->size);
	if (trailing)
		wire_u8(wire, 0);
}

/* Builds a PUBKEY_LOGIN body: string user, string blob, string data, string the signature of data by signer. */
static void
build_login(
	struct wire *wire,
	const char *user,
	const struct wire *blob,
	const struct wire *data,
	const struct ssh_key *signer,
	int trailing)
{
	struct ssh_buf signature;
	int signed_data_status;

	/* Signs the data. */
	ssh_buf_init(&signature, 256, 0);
	signed_data_status = ssh_key_sign(signer, data->data, data->size, &signature);
	CHECK_EQ(signed_data_status, 0);

	/* The four strings. */
	wire_reset(wire);
	wire_text(wire, user);
	wire_string(wire, blob->data, blob->size);
	wire_string(wire, data->data, data->size);
	wire_string(wire, signature.data, signature.size);
	if (trailing)
		wire_u8(wire, 0);
	ssh_buf_free(&signature);
}

/* Builds a PUBKEY_QUERY body: string user, string blob. */
static void
build_query(
	struct wire *wire,
	const char *user,
	const struct wire *blob)
{
	/* The two strings. */
	wire_reset(wire);
	wire_text(wire, user);
	wire_string(wire, blob->data, blob->size);
}

/* Builds a PASSWORD_LOGIN body: string user, string the size bytes of password. */
static void
build_password(
	struct wire *wire,
	const char *user,
	const char *password,
	size_t size)
{
	/* The two strings. */
	wire_reset(wire);
	wire_text(wire, user);
	wire_string(wire, password, size);
}

/* Builds a LOGIN_RECORD or LOGOUT_RECORD body: string tty, uint32 pid. */
static void
build_record(
	struct wire *wire,
	const char *tty,
	size_t tty_size,
	uint32_t pid,
	int trailing)
{
	/* The two fields. */
	wire_reset(wire);
	wire_string(wire, tty, tty_size);
	wire_u32(wire, pid);
	if (trailing)
		wire_u8(wire, 0);
}

/* Builds in request a body of type that is right in itself (for the session identifier session_h), so only the order is wrong. */
static void
build_right(
	unsigned type)
{
	/* The body each request type takes. */
	build_signed(&signed_data, session_h, sizeof(session_h), 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
	if (type == SSH_MON_PUBKEY_LOGIN) {
		build_login(&request, self_name, &user_blob, &signed_data, &user_key, 0);
	} else if (type == SSH_MON_PUBKEY_QUERY) {
		build_query(&request, self_name, &user_blob);
	} else if (type == SSH_MON_PASSWORD_LOGIN) {
		build_password(&request, self_name, "pw", 2);
	} else if (type == SSH_MON_LOGIN_RECORD || type == SSH_MON_LOGOUT_RECORD) {
		build_record(&request, "/dev/pts/1", 10, 100, 0);
	} else if (type == SSH_MON_SIGN) {
		wire_reset(&request);
		wire_string(&request, other_h, sizeof(other_h));
	} else {
		wire_reset(&request);
		wire_text(&request, TEST_STATE);
	}
}

/* Signs the session identifier and logs in with the user key; returns the RESULT (or 0xffff on a monitor error). */
static int
login_good(
	struct sshd_monitor *monitor)
{
	unsigned type;
	uint32_t result;
	int handled;

	/* The exchange's signature, then the login. */
	handled = sign_h(monitor, session_h, sizeof(session_h));
	CHECK_EQ(handled, 0);
	build_signed(&signed_data, session_h, sizeof(session_h), 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
	build_login(&request, self_name, &user_blob, &signed_data, &user_key, 0);
	handled = call(monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
	if (handled != 0)
		return 0xffff;
	CHECK_EQ(type, SSH_MON_RESULT);
	return (int)result;
}

/* Returns the milliseconds since start (CLOCK_MONOTONIC). */
static long
elapsed_ms(
	const struct timespec *start)
{
	struct timespec now;

	/* Measures the time since start. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)(now.tv_sec - start->tv_sec) * 1000L + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

/* The settings text: the default text written by hand from the writer's rules, round trips, and what cannot be written. */
static void
test_config_text(void)
{
	static const char expected[] =
	    "Port 22\n"
	    "ListenAddress 0.0.0.0\n"
	    "AddressFamily inet\n"
	    "HostKey /etc/ssh/ssh_host_ed25519_key\n"
	    "PermitRootLogin prohibit-password\n"
	    "PasswordAuthentication yes\n"
	    "PermitEmptyPasswords no\n"
	    "PubkeyAuthentication yes\n"
	    "AuthorizedKeysFile .ssh/authorized_keys\n"
	    "StrictModes yes\n"
	    "MaxAuthTries 6\n"
	    "LoginGraceTime 120\n"
	    "MaxSessions 10\n"
	    "MaxStartups 10\n"
	    "ClientAliveInterval 0\n"
	    "ClientAliveCountMax 3\n";
	struct sshd_config config;
	struct sshd_config parsed;
	struct ssh_buf out;
	char error_text[SSH_CONFIG_ERROR_MAX];
	int status;

	/* The defaults, finished, as one exact text. */
	sshd_config_defaults(&config);
	(void)sshd_config_finish(&config);
	ssh_buf_init(&out, SSH_CONFIG_FILE_MAX, 0);
	status = sshd_config_put_text(&config, &out);
	CHECK_EQ(status, 0);
	CHECK_EQ(out.size, sizeof(expected) - 1U);
	if (out.size == sizeof(expected) - 1U)
		CHECK_BYTES(out.data, expected, out.size);

	/* Appended after earlier bytes, and read back from where it starts. */
	ssh_buf_reset(&out);
	ssh_buf_put_bytes(&out, "xyz", 3);
	status = sshd_config_put_text(&config, &out);
	CHECK_EQ(status, 0);
	CHECK_EQ(out.size, 3 + sizeof(expected) - 1U);

	/* Every setting changed from its default, with paths that need quotes, reads back the same. */
	config.port = 2222;
	strcpy(config.listen_address, "::1");
	config.inet6 = 1;
	strcpy(config.host_key[1], "/etc/ssh/with space key");
	strcpy(config.host_key[2], "#hash-start");
	strcpy(config.host_key[3], "=equal-start");
	strcpy(config.host_key[4], "/a\"quote-inside");
	config.host_key_count = 5;
	config.permit_root = SSHD_PERMIT_ROOT_YES;
	config.password = 0;
	config.permit_empty = 1;
	config.pubkey = 0;
	strcpy(config.authorized_keys_file, "%h/.ssh/keys %u");
	config.strict_modes = 0;
	config.max_auth_tries = 1000;
	config.login_grace_time = 0;
	config.max_sessions = 0;
	config.max_startups = 1000000;
	config.client_alive_interval = 2147483647U;
	config.client_alive_count = 0;
	strcpy(config.sftp_command, "/usr/libexec/sftp server");
	ssh_buf_reset(&out);
	status = sshd_config_put_text(&config, &out);
	CHECK_EQ(status, 0);
	sshd_config_defaults(&parsed);
	status = sshd_config_parse(&parsed, (const char *)out.data, out.size, error_text);
	CHECK_EQ(status, 0);
	CHECK_EQ(parsed.port, 2222);
	CHECK_EQ(parsed.host_key_count, 5);
	CHECK(strcmp(parsed.host_key[1], "/etc/ssh/with space key") == 0);
	CHECK(strcmp(parsed.host_key[4], "/a\"quote-inside") == 0);
	CHECK(strcmp(parsed.sftp_command, "/usr/libexec/sftp server") == 0);
	CHECK(strcmp(parsed.authorized_keys_file, "%h/.ssh/keys %u") == 0);
	CHECK_EQ(parsed.client_alive_interval, 2147483647U);
	CHECK(strstr((const char *)out.data, "PermitRootLogin yes\n") != NULL);
	CHECK(strstr((const char *)out.data, "AddressFamily inet6\n") != NULL);
	CHECK(strstr((const char *)out.data, "HostKey \"#hash-start\"\n") != NULL);
	CHECK(strstr((const char *)out.data, "Subsystem sftp \"/usr/libexec/sftp server\"\n") != NULL);
	ssh_buf_free(&out);

	/* A path with a blank and a double quote cannot be written: refused, nothing appended. */
	sshd_config_defaults(&config);
	strcpy(config.host_key[0], "/a \"b");
	config.host_key_count = 1;
	ssh_buf_init(&out, SSH_CONFIG_FILE_MAX, 0);
	ssh_buf_put_bytes(&out, "ab", 2);
	status = sshd_config_put_text(&config, &out);
	CHECK_EQ(status, EINVAL);
	CHECK_EQ(out.size, 2);

	/* A path that starts with a double quote, and one with a control byte. */
	strcpy(config.host_key[0], "\"start");
	status = sshd_config_put_text(&config, &out);
	CHECK_EQ(status, EINVAL);
	strcpy(config.host_key[0], "/a\nPort 23");
	status = sshd_config_put_text(&config, &out);
	CHECK_EQ(status, EINVAL);
	CHECK_EQ(out.size, 2);

	/* Settings the text reads back differently: a switch of 2, a PermitRootLogin out of range, a MaxAuthTries of 0, too many host keys. */
	sshd_config_defaults(&config);
	config.password = 2;
	status = sshd_config_put_text(&config, &out);
	CHECK_EQ(status, EINVAL);
	sshd_config_defaults(&config);
	config.permit_root = (enum sshd_permit_root)7;
	status = sshd_config_put_text(&config, &out);
	CHECK_EQ(status, EINVAL);
	sshd_config_defaults(&config);
	config.max_auth_tries = 0;
	status = sshd_config_put_text(&config, &out);
	CHECK_EQ(status, EINVAL);
	sshd_config_defaults(&config);
	config.host_key_count = SSH_CONFIG_KEYS_MAX + 1U;
	status = sshd_config_put_text(&config, &out);
	CHECK_EQ(status, EINVAL);
	CHECK_EQ(out.size, 2);

	/* An empty word (an empty listen address) cannot be written. */
	sshd_config_defaults(&config);
	config.listen_address[0] = '\0';
	status = sshd_config_put_text(&config, &out);
	CHECK_EQ(status, EINVAL);

	/* An out that has failed keeps its error. */
	sshd_config_defaults(&config);
	out.error = ENOMEM;
	status = sshd_config_put_text(&config, &out);
	CHECK_EQ(status, ENOMEM);
	ssh_buf_free(&out);

	/* An out too small for the text records the failure. */
	ssh_buf_init(&out, 64, 0);
	status = sshd_config_put_text(&config, &out);
	CHECK_EQ(status, EMSGSIZE);
	ssh_buf_free(&out);
}

/* The account rules on accounts made up here. */
static void
test_account_policy(void)
{
	struct sshd_config config;
	struct passwd account;
	int status;

	/* An ordinary account. */
	sshd_config_defaults(&config);
	memset(&account, 0, sizeof(account));
	account.pw_name = "alice";
	account.pw_uid = 1001;
	account.pw_gid = 1001;
	account.pw_dir = "/home/alice";
	account.pw_shell = "/bin/sh";

	/* As root: a hash is needed, and a locked one refuses every method. */
	status = sshd_account_policy(&config, &account, "$6$salt$hash", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, 0);
	status = sshd_account_policy(&config, &account, "$6$salt$hash", SSH_AUTH_PASSWORD, 1, 0);
	CHECK_EQ(status, 0);
	status = sshd_account_policy(&config, &account, "", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, 0);
	status = sshd_account_policy(&config, &account, NULL, SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, EACCES);
	status = sshd_account_policy(&config, &account, "!$6$salt$hash", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, EACCES);
	status = sshd_account_policy(&config, &account, "*", SSH_AUTH_PASSWORD, 1, 0);
	CHECK_EQ(status, EACCES);
	status = sshd_account_policy(&config, &account, "!", SSH_AUTH_PASSWORD, 1, 0);
	CHECK_EQ(status, EACCES);

	/* Without root: only the account sshd runs as, whatever the hash says. */
	status = sshd_account_policy(&config, &account, NULL, SSH_AUTH_PUBLICKEY, 0, 1001);
	CHECK_EQ(status, 0);
	status = sshd_account_policy(&config, &account, NULL, SSH_AUTH_PUBLICKEY, 0, 1000);
	CHECK_EQ(status, EACCES);
	status = sshd_account_policy(&config, &account, NULL, SSH_AUTH_PUBLICKEY, 0, 1002);
	CHECK_EQ(status, EACCES);

	/* The methods that are not logins. */
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_NONE, 1, 0);
	CHECK_EQ(status, EACCES);
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_OTHER, 1, 0);
	CHECK_EQ(status, EACCES);

	/* The nologin shells, and names that only look like them. */
	account.pw_shell = "/sbin/nologin";
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, EACCES);
	account.pw_shell = "/usr/sbin/nologin";
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, EACCES);
	account.pw_shell = "/sbin/nologinx";
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, 0);
	account.pw_shell = "/sbin/nologi";
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, 0);
	account.pw_shell = NULL;
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, 0);
	account.pw_shell = "";
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, 0);

	/* root, by PermitRootLogin. */
	account.pw_uid = 0;
	account.pw_shell = "/bin/sh";
	config.permit_root = SSHD_PERMIT_ROOT_YES;
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, 0);
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_PASSWORD, 1, 0);
	CHECK_EQ(status, 0);
	config.permit_root = SSHD_PERMIT_ROOT_PROHIBIT_PASSWORD;
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, 0);
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_PASSWORD, 1, 0);
	CHECK_EQ(status, EACCES);
	config.permit_root = SSHD_PERMIT_ROOT_NO;
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, EACCES);
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_PASSWORD, 1, 0);
	CHECK_EQ(status, EACCES);
	config.permit_root = (enum sshd_permit_root)7;
	status = sshd_account_policy(&config, &account, "$6$x", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, EACCES);

	/* A missing argument is a refusal. */
	status = sshd_account_policy(NULL, &account, "$6$x", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, EACCES);
	status = sshd_account_policy(&config, NULL, "$6$x", SSH_AUTH_PUBLICKEY, 1, 0);
	CHECK_EQ(status, EACCES);
}

/* The account check with the host's passwd: the test's own account, an absent one, root. */
static void
test_account_allowed(void)
{
	struct passwd account;
	char buffer[SSHD_ACCOUNT_BUFFER];
	char long_name[SSH_USER_MAX + 2];
	int status;

	/* The account the test runs as may log in with a key. */
	status = sshd_account_allowed(&base_config, self_name, SSH_AUTH_PUBLICKEY, &account, buffer, sizeof(buffer));
	CHECK_EQ(status, 0);
	CHECK_EQ(account.pw_uid, getuid());

	/* An account that does not exist, an empty name and one too long are ENOENT. */
	status = sshd_account_allowed(&base_config, "zz-no-such-user-0", SSH_AUTH_PUBLICKEY, &account, buffer, sizeof(buffer));
	CHECK_EQ(status, ENOENT);
	status = sshd_account_allowed(&base_config, "", SSH_AUTH_PUBLICKEY, &account, buffer, sizeof(buffer));
	CHECK_EQ(status, ENOENT);
	memset(long_name, 'a', sizeof(long_name) - 1U);
	long_name[sizeof(long_name) - 1U] = '\0';
	status = sshd_account_allowed(&base_config, long_name, SSH_AUTH_PUBLICKEY, &account, buffer, sizeof(buffer));
	CHECK_EQ(status, ENOENT);

	/* A name's line end, escape, DEL and bytes outside ASCII reach the log as '?' each. */
	status = sshd_account_allowed(&base_config, "zz\nforged: x\033[2J\177\302\233", SSH_AUTH_PUBLICKEY, &account, buffer, sizeof(buffer));
	CHECK_EQ(status, ENOENT);
	CHECK(strcmp(log_last, "invalid user zz?forged: x?[2J???") == 0);

	/* root is another account than the one sshd runs as. */
	status = sshd_account_allowed(&base_config, "root", SSH_AUTH_PUBLICKEY, &account, buffer, sizeof(buffer));
	CHECK_EQ(status, EACCES);

	/* Missing arguments are refusals. */
	status = sshd_account_allowed(NULL, self_name, SSH_AUTH_PUBLICKEY, &account, buffer, sizeof(buffer));
	CHECK_EQ(status, EACCES);
	status = sshd_account_allowed(&base_config, NULL, SSH_AUTH_PUBLICKEY, &account, buffer, sizeof(buffer));
	CHECK_EQ(status, EACCES);
}

/* The password check: always refused here, always one crypt on the dummy hash, the password always wiped. */
static void
test_password(void)
{
	struct sshd_config config;
	char password[SSH_PASSWORD_MAX + 1];
	unsigned long before;
	size_t index;
	int zero;
	int status;

	/* An absent account: crypt runs once on the dummy hash, and the password is wiped. */
	config = base_config;
	strcpy(password, "secret-password");
	before = crypt_calls;
	status = sshd_password_ok(&config, "zz-no-such-user-0", password);
	CHECK_EQ(status, EACCES);
	CHECK_EQ(crypt_calls - before, 1);
	CHECK(strncmp(crypt_salt, TEST_DUMMY_PREFIX, strlen(TEST_DUMMY_PREFIX)) == 0);
	CHECK_EQ(strlen(crypt_salt), 3 + 16 + 1 + 86);
	zero = 1;
	for (index = 0; index < strlen("secret-password"); index++) {
		if (password[index] != 0)
			zero = 0;
	}

	CHECK(zero);

	/* crypt's answer is wiped too. */
	CHECK_EQ(crypt_answer[0], 0);

	/* The test's own account without root: the same work, refused. */
	strcpy(password, "secret-password");
	before = crypt_calls;
	status = sshd_password_ok(&config, self_name, password);
	CHECK_EQ(status, EACCES);
	CHECK_EQ(crypt_calls - before, 1);
	CHECK_EQ(password[0], 0);

	/* An empty password without PermitEmptyPasswords: the same work. */
	password[0] = '\0';
	before = crypt_calls;
	status = sshd_password_ok(&config, self_name, password);
	CHECK_EQ(status, EACCES);
	CHECK_EQ(crypt_calls - before, 1);

	/* An empty password with it, without root: still the work, still refused. */
	config.permit_empty = 1;
	before = crypt_calls;
	status = sshd_password_ok(&config, self_name, password);
	CHECK_EQ(status, EACCES);
	CHECK_EQ(crypt_calls - before, 1);

	/* Password authentication off: refused without any account work, and wiped. */
	config.password = 0;
	strcpy(password, "secret-password");
	before = crypt_calls;
	status = sshd_password_ok(&config, self_name, password);
	CHECK_EQ(status, EACCES);
	CHECK_EQ(crypt_calls - before, 0);
	CHECK_EQ(password[0], 0);

	/* No password at all. */
	status = sshd_password_ok(&base_config, self_name, NULL);
	CHECK_EQ(status, EACCES);
}

/* The expansion of AuthorizedKeysFile. */
static void
test_authorized_path(void)
{
	struct passwd account;
	char out[64];
	int status;

	/* An account with a short home. */
	memset(&account, 0, sizeof(account));
	account.pw_name = "u";
	account.pw_dir = "/home/u";

	/* A relative path starts at the home directory. */
	status = sshd_authorized_path(".ssh/authorized_keys", &account, out, sizeof(out));
	CHECK_EQ(status, 0);
	CHECK(strcmp(out, "/home/u/.ssh/authorized_keys") == 0);

	/* The tokens. */
	status = sshd_authorized_path("%h/.ssh/k", &account, out, sizeof(out));
	CHECK_EQ(status, 0);
	CHECK(strcmp(out, "/home/u/.ssh/k") == 0);
	status = sshd_authorized_path("/etc/keys/%u", &account, out, sizeof(out));
	CHECK_EQ(status, 0);
	CHECK(strcmp(out, "/etc/keys/u") == 0);
	status = sshd_authorized_path("%%x", &account, out, sizeof(out));
	CHECK_EQ(status, 0);
	CHECK(strcmp(out, "/home/u/%x") == 0);
	status = sshd_authorized_path("%u/k", &account, out, sizeof(out));
	CHECK_EQ(status, 0);
	CHECK(strcmp(out, "/home/u/u/k") == 0);

	/* Another token, a '%' at the end. */
	status = sshd_authorized_path("/k/%x", &account, out, sizeof(out));
	CHECK_EQ(status, EINVAL);
	CHECK_EQ(out[0], 0);
	status = sshd_authorized_path("/k/%", &account, out, sizeof(out));
	CHECK_EQ(status, EINVAL);

	/* The edge of the room: "/home/u/.ssh/k" is 14 bytes, so 15 fit and 14 do not. */
	status = sshd_authorized_path(".ssh/k", &account, out, 15);
	CHECK_EQ(status, 0);
	status = sshd_authorized_path(".ssh/k", &account, out, 14);
	CHECK_EQ(status, EINVAL);
	status = sshd_authorized_path("%h/.ssh/k", &account, out, 14);
	CHECK_EQ(status, EINVAL);
	status = sshd_authorized_path("/a/%u", &account, out, 5);
	CHECK_EQ(status, 0);
	CHECK(strcmp(out, "/a/u") == 0);
	status = sshd_authorized_path("/a/%u", &account, out, 4);
	CHECK_EQ(status, EINVAL);

	/* A home that is not absolute, and an empty pattern. */
	account.pw_dir = "home/u";
	status = sshd_authorized_path(".ssh/k", &account, out, sizeof(out));
	CHECK_EQ(status, EINVAL);
	account.pw_dir = "/home/u";
	status = sshd_authorized_path("", &account, out, sizeof(out));
	CHECK_EQ(status, 0);
	CHECK(strcmp(out, "/home/u/") == 0);
	status = sshd_authorized_path("x", NULL, out, sizeof(out));
	CHECK_EQ(status, EINVAL);
}

/*
 * The reading child: a home made here with the test's own ids (a user
 * other than the one the test runs as cannot own files here without
 * root, so the other user's side is the refusal of an account that is
 * not the one sshd runs as).
 */
static void
test_key_authorized(void)
{
	static char big[SSH_HOSTFILE_MAX + 2];
	struct sshd_config config;
	struct passwd account;
	char home[TEST_PATH_MAX];
	char ssh_directory[TEST_PATH_MAX];
	char file[TEST_PATH_MAX];
	char path[TEST_PATH_MAX];
	struct timespec start;
	size_t line_size;
	int status;

	/* The home, ~/.ssh and ~/.ssh/authorized_keys with the user key. */
	join(home, work, "home");
	(void)mkdir(home, 0700);
	join(ssh_directory, home, ".ssh");
	(void)mkdir(ssh_directory, 0700);
	join(file, ssh_directory, "authorized_keys");
	wire_reset(&scratch);
	put_key_line(&scratch, &user_blob);
	write_file(file, scratch.data, scratch.size, 0600);
	line_size = scratch.size;
	memset(&account, 0, sizeof(account));
	account.pw_name = self_name;
	account.pw_uid = getuid();
	account.pw_gid = getgid();
	account.pw_dir = home;
	account.pw_shell = "/bin/sh";
	sshd_config_defaults(&config);

	/* The key is found with StrictModes; another key is not. */
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, 0);
	status = sshd_key_authorized(&config, &account, host_key.public_key);
	CHECK_EQ(status, EACCES);

	/* A doubled slash, a "." and a home with a trailing slash are the same file. */
	strcpy(config.authorized_keys_file, ".ssh//./authorized_keys");
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, 0);
	strcpy(config.authorized_keys_file, ".ssh/authorized_keys");
	strcat(home, "/");
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, 0);
	home[strlen(home) - 1U] = '\0';

	/* A file the group may write: refused with StrictModes, read without. */
	(void)chmod(file, 0620);
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	config.strict_modes = 0;
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, 0);
	config.strict_modes = 1;
	(void)chmod(file, 0602);
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	(void)chmod(file, 0600);

	/* A file the user cannot read is not read: the child has the user's rights, not root's. */
	(void)chmod(file, 0000);
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	(void)chmod(file, 0400);
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, 0);
	(void)chmod(file, 0600);

	/* ~/.ssh and the home the group or others may write. */
	(void)chmod(ssh_directory, 0770);
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	(void)chmod(ssh_directory, 0702);
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	(void)chmod(ssh_directory, 0755);
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, 0);
	(void)chmod(ssh_directory, 0700);
	(void)chmod(home, 0777);
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	(void)chmod(home, 0700);

	/* A link as the file: refused with and without StrictModes. */
	join(path, ssh_directory, "link_keys");
	(void)symlink(file, path);
	strcpy(config.authorized_keys_file, ".ssh/link_keys");
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	config.strict_modes = 0;
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	config.strict_modes = 1;

	/* A link as a directory below the home: refused with StrictModes. */
	join(path, home, "linked");
	(void)symlink(ssh_directory, path);
	strcpy(config.authorized_keys_file, "linked/authorized_keys");
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);

	/* A FIFO: refused at once, without waiting for a writer. */
	join(path, ssh_directory, "fifo");
	status = mkfifo(path, 0600);
	CHECK(status == 0 || errno == EEXIST);
	strcpy(config.authorized_keys_file, ".ssh/fifo");
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	config.strict_modes = 0;
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	CHECK(elapsed_ms(&start) < 5000L);
	config.strict_modes = 1;

	/* A directory. */
	join(path, ssh_directory, "directory");
	(void)mkdir(path, 0700);
	strcpy(config.authorized_keys_file, ".ssh/directory");
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);

	/* A device, outside the home (the walk starts at the root, whose directories are root's). */
	strcpy(config.authorized_keys_file, "/dev/zero");
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	config.strict_modes = 0;
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	config.strict_modes = 1;

	/* A path beside the home whose name starts with the home's is not walked from the home. */
	(void)snprintf(config.authorized_keys_file, sizeof(config.authorized_keys_file), "%sX/.ssh/authorized_keys", home);
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);

	/* A ".." that climbs, a missing file, a path ending in a slash, none. */
	strcpy(config.authorized_keys_file, ".ssh/../.ssh/authorized_keys");
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	strcpy(config.authorized_keys_file, ".ssh/missing");
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	strcpy(config.authorized_keys_file, ".ssh/");
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	strcpy(config.authorized_keys_file, "/dev/null");
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);

	/* A file of exactly the largest size, the key on its last line, is read; one byte more is not. */
	strcpy(config.authorized_keys_file, ".ssh/big");
	join(path, ssh_directory, "big");
	memset(big, '#', SSH_HOSTFILE_MAX - line_size);
	big[SSH_HOSTFILE_MAX - line_size - 1U] = '\n';
	memcpy(big + SSH_HOSTFILE_MAX - line_size, scratch.data, line_size);
	write_file(path, big, SSH_HOSTFILE_MAX, 0600);
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, 0);
	memmove(big + 1, big, SSH_HOSTFILE_MAX);
	big[0] = '#';
	write_file(path, big, SSH_HOSTFILE_MAX + 1U, 0600);
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);

	/* An account that is not the one sshd runs as: its file is never read without root. */
	strcpy(config.authorized_keys_file, ".ssh/authorized_keys");
	account.pw_uid = getuid() + 1U;
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	CHECK(strstr(log_last, "not the user sshd runs as") != NULL);
	account.pw_uid = getuid();

	/* A pattern that cannot be expanded, and missing arguments. */
	strcpy(config.authorized_keys_file, "%x");
	status = sshd_key_authorized(&config, &account, user_key.public_key);
	CHECK_EQ(status, EACCES);
	status = sshd_key_authorized(&config, &account, NULL);
	CHECK_EQ(status, EACCES);
}

/* The terminals a login record may name. */
static void
test_tty(void)
{
	char name[64];
	int master;
	int status;

	/* Paths that are not a pseudo-terminal's. */
	status = sshd_tty_check("/dev/tty1", getuid());
	CHECK_EQ(status, EACCES);
	status = sshd_tty_check("/dev/pts/", getuid());
	CHECK_EQ(status, EACCES);
	status = sshd_tty_check("/dev/pts/ptmx", getuid());
	CHECK_EQ(status, EACCES);
	status = sshd_tty_check("/dev/pts/1a", getuid());
	CHECK_EQ(status, EACCES);
	status = sshd_tty_check("/dev/pts/123456789", getuid());
	CHECK_EQ(status, EACCES);
	status = sshd_tty_check("/dev/pts/../tty", getuid());
	CHECK_EQ(status, EACCES);
	status = sshd_tty_check("/dev/pts", getuid());
	CHECK_EQ(status, EACCES);
	status = sshd_tty_check(NULL, getuid());
	CHECK_EQ(status, EACCES);

	/* A number that has no terminal. */
	status = sshd_tty_check("/dev/pts/99999999", getuid());
	CHECK_EQ(status, EACCES);

	/* A terminal the test opens is its own, and no other user's. */
	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0 && ptsname_r(master, name, sizeof(name)) == 0) {
		status = sshd_tty_check(name, getuid());
		CHECK_EQ(status, 0);
		status = sshd_tty_check(name, getuid() + 1U);
		CHECK_EQ(status, EACCES);
	} else {
		printf("note: no pseudo-terminal on this host; the owner check is not run\n");
	}

	if (master >= 0)
		(void)close(master);

	/* The record writer refuses what is not a pseudo-terminal before writing anything. */
	status = sshd_login_record(self_name, "/dev/tty1", getpid(), 1);
	CHECK_EQ(status, EIO);
	status = sshd_login_record(self_name, "/dev/pts/x", getpid(), 0);
	CHECK_EQ(status, EIO);
	status = sshd_login_record(NULL, "/dev/pts/1", getpid(), 1);
	CHECK_EQ(status, EIO);
}

/* Which requests each stage takes: the rest end the connection. */
static void
test_stages(void)
{
	static const unsigned refused_first[] = {
		SSH_MON_PUBKEY_QUERY, SSH_MON_PUBKEY_LOGIN, SSH_MON_PASSWORD_LOGIN, SSH_MON_AUTH_DONE,
		SSH_MON_LOGIN_RECORD, SSH_MON_LOGOUT_RECORD, SSH_MON_SIGNATURE, SSH_MON_RESULT,
		SSH_MON_CONFIG, SSH_MON_SESSION_START, 0, 12, 99
	};
	static const unsigned refused_granted[] = {
		SSH_MON_PUBKEY_QUERY, SSH_MON_PUBKEY_LOGIN, SSH_MON_PASSWORD_LOGIN, SSH_MON_LOGIN_RECORD,
		SSH_MON_LOGOUT_RECORD, SSH_MON_SIGNATURE, SSH_MON_RESULT, SSH_MON_CONFIG, SSH_MON_SESSION_START
	};
	static const unsigned refused_session[] = {
		SSH_MON_PUBKEY_QUERY, SSH_MON_PUBKEY_LOGIN, SSH_MON_PASSWORD_LOGIN, SSH_MON_AUTH_DONE,
		SSH_MON_SIGNATURE, SSH_MON_RESULT, SSH_MON_CONFIG, SSH_MON_SESSION_START
	};
	struct sshd_monitor monitor;
	struct wire body;
	char expected_log[160];
	enum sshd_monitor_next next;
	unsigned type;
	uint32_t result;
	size_t index;
	int status;

	/* Before any signature: the requests built right, but out of order. */
	for (index = 0; index < sizeof(refused_first) / sizeof(refused_first[0]); index++) {
		fresh(&monitor, &base_config);
		build_right(refused_first[index]);
		status = call(&monitor, refused_first[index], &request, &type, &result, NULL, NULL);
		CHECK_EQ(status, EPROTO);
		CHECK_EQ(monitor.stage, SSHD_STAGE_PREAUTH);
		CHECK_EQ(monitor.tries, 0);
		sshd_monitor_clear(&monitor);
	}

	/* After a granted login: only SIGN and AUTH_DONE. */
	for (index = 0; index < sizeof(refused_granted) / sizeof(refused_granted[0]); index++) {
		fresh(&monitor, &base_config);
		status = login_good(&monitor);
		CHECK_EQ(status, 0);
		CHECK_EQ(monitor.stage, SSHD_STAGE_GRANTED);
		build_right(refused_granted[index]);
		status = call(&monitor, refused_granted[index], &request, &type, &result, NULL, NULL);
		CHECK_EQ(status, EPROTO);
		sshd_monitor_clear(&monitor);
	}

	/* The hand-over: AUTH_DONE once, with its bytes kept for the session. */
	fresh(&monitor, &base_config);
	status = login_good(&monitor);
	CHECK_EQ(status, 0);
	CHECK_EQ(monitor.user_uid, getuid());

	/*
	 * The grant is logged in OpenSSH's form: "Accepted publickey for",
	 * the user, " ssh2: ED25519 " and the key's "SHA256:" fingerprint of
	 * 43 base64 characters (no peer: the monitor runs without one here).
	 */
	snprintf(expected_log, sizeof(expected_log), "Accepted publickey for %s ssh2: ED25519 SHA256:", self_name);
	CHECK(strncmp(log_last, expected_log, strlen(expected_log)) == 0);
	CHECK_EQ(strlen(log_last), strlen(expected_log) + 43U);
	status = sign_h(&monitor, other_h, sizeof(other_h));
	CHECK_EQ(status, 0);
	wire_reset(&body);
	wire_text(&body, TEST_STATE);
	status = call(&monitor, SSH_MON_AUTH_DONE, &body, &type, &result, NULL, &next);
	CHECK_EQ(status, 0);
	CHECK_EQ(next, SSHD_NEXT_START_SESSION);
	CHECK_EQ(monitor.stage, SSHD_STAGE_SESSION);
	CHECK_EQ(monitor.state.size, strlen(TEST_STATE));
	CHECK(monitor.state.secret);
	if (monitor.state.size == strlen(TEST_STATE))
		CHECK_BYTES(monitor.state.data, TEST_STATE, monitor.state.size);

	/* A second AUTH_DONE, and every pre-authentication request, in the session stage. */
	for (index = 0; index < sizeof(refused_session) / sizeof(refused_session[0]); index++) {
		build_right(refused_session[index]);
		status = call(&monitor, refused_session[index], &request, &type, &result, NULL, NULL);
		CHECK_EQ(status, EPROTO);
	}

	sshd_monitor_clear(&monitor);
	CHECK(monitor.state.data == NULL);

	/* AUTH_DONE with bytes after its field, or empty: refused. */
	fresh(&monitor, &base_config);
	status = login_good(&monitor);
	CHECK_EQ(status, 0);
	wire_reset(&body);
	wire_text(&body, TEST_STATE);
	wire_u8(&body, 0);
	status = call(&monitor, SSH_MON_AUTH_DONE, &body, &type, &result, NULL, &next);
	CHECK_EQ(status, EPROTO);
	wire_reset(&body);
	wire_string(&body, "", 0);
	status = call(&monitor, SSH_MON_AUTH_DONE, &body, &type, &result, NULL, &next);
	CHECK_EQ(status, EPROTO);
	CHECK_EQ(monitor.stage, SSHD_STAGE_GRANTED);
	sshd_monitor_clear(&monitor);
}

/* SIGN: the length of h, the field's framing, the count before the session. */
static void
test_sign(void)
{
	struct sshd_monitor monitor;
	struct wire body;
	uint8_t h[64];
	unsigned type;
	uint32_t result;
	unsigned index;
	int status;

	/* An h of 31 or 33 bytes, or of none. */
	memset(h, 7, sizeof(h));
	fresh(&monitor, &base_config);
	status = sign_h(&monitor, h, 31);
	CHECK_EQ(status, EPROTO);
	status = sign_h(&monitor, h, 33);
	CHECK_EQ(status, EPROTO);
	status = sign_h(&monitor, h, 0);
	CHECK_EQ(status, EPROTO);
	CHECK_EQ(monitor.have_session_id, 0);
	CHECK_EQ(monitor.preauth_signs, 0);

	/* A byte after the field, an empty body, a length past the end. */
	wire_reset(&body);
	wire_string(&body, h, 32);
	wire_u8(&body, 0);
	status = call(&monitor, SSH_MON_SIGN, &body, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	wire_reset(&body);
	status = call(&monitor, SSH_MON_SIGN, &body, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	wire_reset(&body);
	wire_u32(&body, 33);
	wire_bytes(&body, h, 32);
	status = call(&monitor, SSH_MON_SIGN, &body, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);

	/* SSHD_PREAUTH_SIGN_MAX signatures, then one more ends the connection. */
	for (index = 0; index < SSHD_PREAUTH_SIGN_MAX; index++) {
		h[0] = (uint8_t)index;
		status = sign_h(&monitor, h, 32);
		CHECK_EQ(status, 0);
	}

	CHECK_EQ(monitor.preauth_signs, SSHD_PREAUTH_SIGN_MAX);
	h[0] = 0x77;
	status = sign_h(&monitor, h, 32);
	CHECK_EQ(status, EPROTO);

	/* The session identifier is the first h. */
	h[0] = 0;
	CHECK(monitor.have_session_id);
	CHECK_BYTES(monitor.session_id, h, 32);
	sshd_monitor_clear(&monitor);

	/* The signatures after a grant count with the others. */
	fresh(&monitor, &base_config);
	status = login_good(&monitor);
	CHECK_EQ(status, 0);
	for (index = 1; index < SSHD_PREAUTH_SIGN_MAX; index++) {
		status = sign_h(&monitor, other_h, 32);
		CHECK_EQ(status, 0);
	}

	status = sign_h(&monitor, other_h, 32);
	CHECK_EQ(status, EPROTO);
	sshd_monitor_clear(&monitor);
}

/* The signed data of a publickey login: each field wrong in turn, and nothing granted. */
static void
test_signed_data(void)
{
	struct sshd_monitor monitor;
	struct wire wrong_blob;
	struct wire long_blob;
	uint8_t sid[SSH_SHA256_SIZE + 1];
	unsigned type;
	uint32_t result;
	unsigned variant;
	int status;

	/* The other key's blob, for a blob that differs from the request's, and the user's with a byte more. */
	put_blob(&wrong_blob, &host_key);
	put_blob(&long_blob, &user_key);
	wire_u8(&long_blob, 0);

	/* Each variant changes one field of the signed data the user key signs. */
	for (variant = 0; variant < 17; variant++) {
		fresh(&monitor, &base_config);
		status = sign_h(&monitor, session_h, sizeof(session_h));
		CHECK_EQ(status, 0);
		memcpy(sid, session_h, sizeof(session_h));
		sid[sizeof(session_h)] = 0;
		switch (variant) {
		case 0:
			/* The session identifier with its last bit changed. */
			sid[31] ^= 1;
			build_signed(&signed_data, sid, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
			break;
		case 1:
			/* The session identifier one byte short. */
			build_signed(&signed_data, sid, 31, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
			break;
		case 2:
			/* The session identifier one byte long. */
			build_signed(&signed_data, sid, 33, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
			break;
		case 3:
			/* Another message number. */
			build_signed(&signed_data, sid, 32, 51, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
			break;
		case 4:
			/* Another user. */
			build_signed(&signed_data, sid, 32, 50, "root", "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
			break;
		case 5:
			/* The user with a byte more. */
			strcpy((char *)scratch.data, self_name);
			strcat((char *)scratch.data, "x");
			build_signed(&signed_data, sid, 32, 50, (const char *)scratch.data, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
			break;
		case 6:
			/* Another service. */
			build_signed(&signed_data, sid, 32, 50, self_name, "ssh-userauth", "publickey", 1, "ssh-ed25519", &user_blob, 0);
			break;
		case 7:
			/* Another method. */
			build_signed(&signed_data, sid, 32, 50, self_name, "ssh-connection", "password", 1, "ssh-ed25519", &user_blob, 0);
			break;
		case 8:
			/* FALSE. */
			build_signed(&signed_data, sid, 32, 50, self_name, "ssh-connection", "publickey", 0, "ssh-ed25519", &user_blob, 0);
			break;
		case 9:
			/* Another algorithm. */
			build_signed(&signed_data, sid, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-rsa", &user_blob, 0);
			break;
		case 10:
			/* Another key's blob than the request's. */
			build_signed(&signed_data, sid, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &wrong_blob, 0);
			break;
		case 11:
			/* A byte after the blob. */
			build_signed(&signed_data, sid, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 1);
			break;
		case 12:
			/* No blob at all. */
			build_signed(&signed_data, sid, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", NULL, 0);
			break;
		case 13:
			/* The blob one byte short. */
			build_signed(&signed_data, sid, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
			signed_data.size--;
			signed_data.data[signed_data.size - SSH_KEY_BLOB_SIZE] = SSH_KEY_BLOB_SIZE - 1;
			break;
		case 14:
			/* The identifier of a later exchange instead of the first. */
			build_signed(&signed_data, other_h, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
			(void)sign_h(&monitor, other_h, sizeof(other_h));
			break;
		case 16:
			/* The blob of the request with a byte more. */
			build_signed(&signed_data, sid, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &long_blob, 0);
			break;
		default:
			/* All right, but signed by another key than the request names. */
			build_signed(&signed_data, sid, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
			break;
		}

		if (variant == 15)
			build_login(&request, self_name, &user_blob, &signed_data, &host_key, 0);
		else
			build_login(&request, self_name, &user_blob, &signed_data, &user_key, 0);
		status = call(&monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
		CHECK_EQ(status, 0);
		CHECK_EQ(type, SSH_MON_RESULT);
		CHECK_EQ(result, EACCES);
		CHECK_EQ(monitor.stage, SSHD_STAGE_PREAUTH);
		CHECK_EQ(monitor.tries, 1);
		sshd_monitor_clear(&monitor);
	}

	/* The same login with every field right is granted. */
	fresh(&monitor, &base_config);
	status = login_good(&monitor);
	CHECK_EQ(status, 0);
	CHECK_EQ(monitor.stage, SSHD_STAGE_GRANTED);
	sshd_monitor_clear(&monitor);

	/* A key that is not in the file, signed by itself, is refused. */
	fresh(&monitor, &base_config);
	status = sign_h(&monitor, session_h, sizeof(session_h));
	CHECK_EQ(status, 0);
	build_signed(&signed_data, session_h, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &wrong_blob, 0);
	build_login(&request, self_name, &wrong_blob, &signed_data, &host_key, 0);
	status = call(&monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	sshd_monitor_clear(&monitor);

	/* A signature with one byte changed. */
	fresh(&monitor, &base_config);
	status = sign_h(&monitor, session_h, sizeof(session_h));
	CHECK_EQ(status, 0);
	build_signed(&signed_data, session_h, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
	build_login(&request, self_name, &user_blob, &signed_data, &user_key, 0);
	request.data[request.size - 1] ^= 0x01;
	status = call(&monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	sshd_monitor_clear(&monitor);
}

/* PUBKEY_LOGIN and PUBKEY_QUERY bodies that no process that parsed a request would send. */
static void
test_login_malformed(void)
{
	struct sshd_monitor monitor;
	struct sshd_config config;
	struct wire rsa_blob;
	char long_user[SSH_USER_MAX + 2];
	unsigned type;
	uint32_t result;
	int status;

	/* A blob of another type. */
	wire_reset(&rsa_blob);
	wire_text(&rsa_blob, "ssh-rsa");
	wire_string(&rsa_blob, user_key.public_key, 32);

	/* A byte after the signature. */
	fresh(&monitor, &base_config);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	build_signed(&signed_data, session_h, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
	build_login(&request, self_name, &user_blob, &signed_data, &user_key, 1);
	status = call(&monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);

	/* The signature cut short. */
	build_login(&request, self_name, &user_blob, &signed_data, &user_key, 0);
	request.size--;
	status = call(&monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);

	/* A blob of another type, in a login and in a query. */
	build_login(&request, self_name, &rsa_blob, &signed_data, &user_key, 0);
	status = call(&monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	build_query(&request, self_name, &rsa_blob);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);

	/* A query with a byte after the blob. */
	build_query(&request, self_name, &user_blob);
	wire_u8(&request, 0);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);

	/* A user longer than SSH_USER_MAX, and one holding a NUL. */
	memset(long_user, 'a', sizeof(long_user) - 1U);
	long_user[sizeof(long_user) - 1U] = '\0';
	build_query(&request, long_user, &user_blob);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	wire_reset(&request);
	wire_string(&request, "ab\0c", 4);
	wire_string(&request, user_blob.data, user_blob.size);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	CHECK_EQ(monitor.tries, 0);
	CHECK_EQ(monitor.have_user, 0);

	/* A query that is answered, then a login for another user. */
	build_query(&request, self_name, &user_blob);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, 0);
	CHECK_EQ(monitor.granted_queries, 1);
	CHECK_EQ(monitor.tries, 0);
	build_signed(&signed_data, session_h, 32, 50, "root", "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
	build_login(&request, "root", &user_blob, &signed_data, &user_key, 0);
	status = call(&monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	build_password(&request, "root", "pw", 2);
	status = call(&monitor, SSH_MON_PASSWORD_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	sshd_monitor_clear(&monitor);

	/* A query for a key not in the file, and for an absent user. */
	fresh(&monitor, &base_config);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	build_query(&request, self_name, &host_blob);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK_EQ(monitor.tries, 1);
	sshd_monitor_clear(&monitor);
	fresh(&monitor, &base_config);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	build_query(&request, "zz-no-such-user-0", &user_blob);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	sshd_monitor_clear(&monitor);

	/* PubkeyAuthentication off refuses a query and a login that would be right. */
	config = base_config;
	config.pubkey = 0;
	fresh(&monitor, &config);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	build_query(&request, self_name, &user_blob);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	build_signed(&signed_data, session_h, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
	build_login(&request, self_name, &user_blob, &signed_data, &user_key, 0);
	status = call(&monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK_EQ(monitor.stage, SSHD_STAGE_PREAUTH);
	sshd_monitor_clear(&monitor);
}

/* MaxAuthTries over both methods, and the bound of granted queries. */
static void
test_attempts(void)
{
	struct sshd_monitor monitor;
	struct sshd_config config;
	unsigned type;
	uint32_t result;
	unsigned index;
	int status;

	/* Three passwords, then three bad keys: six attempts; the seventh ends the connection. */
	fresh(&monitor, &base_config);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	for (index = 0; index < 3; index++) {
		build_password(&request, self_name, "pw", 2);
		status = call(&monitor, SSH_MON_PASSWORD_LOGIN, &request, &type, &result, NULL, NULL);
		CHECK_EQ(status, 0);
		CHECK_EQ(result, EACCES);
	}

	for (index = 0; index < 3; index++) {
		build_signed(&signed_data, other_h, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
		build_login(&request, self_name, &user_blob, &signed_data, &user_key, 0);
		status = call(&monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
		CHECK_EQ(status, 0);
		CHECK_EQ(result, EACCES);
	}

	CHECK_EQ(monitor.tries, 6);
	build_signed(&signed_data, session_h, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
	build_login(&request, self_name, &user_blob, &signed_data, &user_key, 0);
	status = call(&monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	build_query(&request, self_name, &user_blob);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	build_password(&request, self_name, "pw", 2);
	status = call(&monitor, SSH_MON_PASSWORD_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	sshd_monitor_clear(&monitor);

	/* Five failures, then the right login as the sixth attempt: granted. */
	fresh(&monitor, &base_config);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	for (index = 0; index < 5; index++) {
		build_password(&request, self_name, "pw", 2);
		status = call(&monitor, SSH_MON_PASSWORD_LOGIN, &request, &type, &result, NULL, NULL);
		CHECK_EQ(status, 0);
	}

	build_signed(&signed_data, session_h, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
	build_login(&request, self_name, &user_blob, &signed_data, &user_key, 0);
	status = call(&monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, 0);
	CHECK_EQ(monitor.stage, SSHD_STAGE_GRANTED);
	sshd_monitor_clear(&monitor);

	/* Refused queries count as attempts. */
	config = base_config;
	config.max_auth_tries = 2;
	fresh(&monitor, &config);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	build_query(&request, self_name, &host_blob);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	sshd_monitor_clear(&monitor);

	/* Granted queries have their own bound; a login still follows them. */
	fresh(&monitor, &config);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	build_query(&request, self_name, &user_blob);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, 0);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, 0);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	CHECK_EQ(monitor.tries, 0);
	build_signed(&signed_data, session_h, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
	build_login(&request, self_name, &user_blob, &signed_data, &user_key, 0);
	status = call(&monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, 0);
	sshd_monitor_clear(&monitor);
}

/* PASSWORD_LOGIN: the field rules, and one crypt per request whoever the user is. */
static void
test_password_requests(void)
{
	static char long_password[SSH_PASSWORD_MAX + 2];
	struct sshd_monitor monitor;
	struct sshd_config config;
	unsigned type;
	uint32_t result;
	unsigned long before;
	int status;

	/* The test's own account and an absent one: refused after one crypt each. */
	fresh(&monitor, &base_config);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	before = crypt_calls;
	build_password(&request, self_name, "pw", 2);
	status = call(&monitor, SSH_MON_PASSWORD_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(type, SSH_MON_RESULT);
	CHECK_EQ(result, EACCES);
	CHECK_EQ(crypt_calls - before, 1);
	sshd_monitor_clear(&monitor);
	fresh(&monitor, &base_config);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	before = crypt_calls;
	build_password(&request, "zz-no-such-user-0", "pw", 2);
	status = call(&monitor, SSH_MON_PASSWORD_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK_EQ(crypt_calls - before, 1);

	/* SSH_PASSWORD_MAX bytes are taken; one more ends the connection, as does a NUL inside. */
	memset(long_password, 'p', sizeof(long_password));
	build_password(&request, "zz-no-such-user-0", long_password, SSH_PASSWORD_MAX);
	status = call(&monitor, SSH_MON_PASSWORD_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	build_password(&request, "zz-no-such-user-0", long_password, SSH_PASSWORD_MAX + 1U);
	status = call(&monitor, SSH_MON_PASSWORD_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	build_password(&request, "zz-no-such-user-0", "a\0b", 3);
	status = call(&monitor, SSH_MON_PASSWORD_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	build_password(&request, "zz-no-such-user-0", "pw", 2);
	wire_u8(&request, 0);
	status = call(&monitor, SSH_MON_PASSWORD_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	sshd_monitor_clear(&monitor);

	/* The request bytes the monitor was handed are not where the password is kept: they stay the caller's. */
	CHECK(request.data[request.size - 2] == 'w');

	/* A failed password for a name with a line end, escapes, DEL and a byte pair outside ASCII is logged with '?' for each byte. */
	fresh(&monitor, &base_config);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	build_password(&request, "zz\r\nforged\033]0;x\007\177\303\251", "pw", 2);
	status = call(&monitor, SSH_MON_PASSWORD_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK(strcmp(log_last, "Failed password for zz??forged?]0;x???? ssh2") == 0);
	sshd_monitor_clear(&monitor);

	/* PasswordAuthentication off: refused without crypt. */
	config = base_config;
	config.password = 0;
	fresh(&monitor, &config);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	before = crypt_calls;
	build_password(&request, self_name, "pw", 2);
	status = call(&monitor, SSH_MON_PASSWORD_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK_EQ(crypt_calls - before, 0);
	sshd_monitor_clear(&monitor);
}

/* The answers to authentication requests wait SSHD_FAIL_DELAY_MS from their arrival; SIGN does not. */
static void
test_timing(void)
{
	struct sshd_monitor monitor;
	struct ssh_buf reply;
	struct timespec start;
	struct timespec arrived;
	enum sshd_monitor_next next;
	unsigned long before;
	unsigned type;
	uint32_t result;
	long took;
	int status;

	/* The waits a new monitor has are the header's. */
	sshd_monitor_init(&monitor, &base_config, &host_key, 0);
	CHECK_EQ(monitor.fail_delay_ms, SSHD_FAIL_DELAY_MS);
	CHECK_EQ(monitor.sign_interval_ms, SSHD_SIGN_INTERVAL_MS);

	/* A SIGN of the pre-authentication stage is answered at once. */
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	status = sign_h(&monitor, session_h, sizeof(session_h));
	CHECK_EQ(status, 0);
	took = elapsed_ms(&start);
	CHECK(took < 500L);

	/* A refused query, a password for an absent user (with its crypt), and a granted login. */
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	build_query(&request, self_name, &host_blob);
	status = call(&monitor, SSH_MON_PUBKEY_QUERY, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	took = elapsed_ms(&start);
	CHECK(took >= (long)SSHD_FAIL_DELAY_MS);
	CHECK(took < (long)SSHD_FAIL_DELAY_MS + 3000L);
	sshd_monitor_clear(&monitor);

	sshd_monitor_init(&monitor, &base_config, &host_key, 0);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	before = crypt_calls;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	build_password(&request, "zz-no-such-user-0", "pw", 2);
	status = call(&monitor, SSH_MON_PASSWORD_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	took = elapsed_ms(&start);
	CHECK(took >= (long)SSHD_FAIL_DELAY_MS);
	CHECK_EQ(crypt_calls - before, 1);
	sshd_monitor_clear(&monitor);

	sshd_monitor_init(&monitor, &base_config, &host_key, 0);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	build_signed(&signed_data, session_h, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
	build_login(&request, self_name, &user_blob, &signed_data, &user_key, 0);
	status = call(&monitor, SSH_MON_PUBKEY_LOGIN, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(monitor.stage, SSHD_STAGE_GRANTED);
	took = elapsed_ms(&start);
	CHECK(took >= (long)SSHD_FAIL_DELAY_MS);
	sshd_monitor_clear(&monitor);

	/* A request that arrived longer ago than the delay is not held again. */
	sshd_monitor_init(&monitor, &base_config, &host_key, 0);
	(void)sign_h(&monitor, session_h, sizeof(session_h));
	ssh_buf_init(&reply, 64, 0);
	(void)clock_gettime(CLOCK_MONOTONIC, &arrived);
	arrived.tv_sec -= 2;
	build_query(&request, self_name, &host_blob);
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	status = sshd_monitor_handle(&monitor, SSH_MON_PUBKEY_QUERY, request.data, request.size, &arrived, &reply, &next);
	CHECK_EQ(status, 0);
	took = elapsed_ms(&start);
	CHECK(took < 500L);

	/* One that arrived half the delay ago waits the other half. */
	ssh_buf_reset(&reply);
	(void)clock_gettime(CLOCK_MONOTONIC, &arrived);
	arrived.tv_nsec -= 500000000L;
	if (arrived.tv_nsec < 0) {
		arrived.tv_nsec += 1000000000L;
		arrived.tv_sec--;
	}

	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	status = sshd_monitor_handle(&monitor, SSH_MON_PUBKEY_QUERY, request.data, request.size, &arrived, &reply, &next);
	CHECK_EQ(status, 0);
	took = elapsed_ms(&start);
	CHECK(took >= 400L);
	CHECK(took < 900L);

	/* A shorter delay, which is not a whole number of seconds, is kept to as well. */
	monitor.fail_delay_ms = 300;
	ssh_buf_reset(&reply);
	(void)clock_gettime(CLOCK_MONOTONIC, &arrived);
	status = sshd_monitor_handle(&monitor, SSH_MON_PUBKEY_QUERY, request.data, request.size, &arrived, &reply, &next);
	CHECK_EQ(status, 0);
	took = elapsed_ms(&arrived);
	CHECK(took >= 300L);
	CHECK(took < 900L);
	ssh_buf_free(&reply);
	sshd_monitor_clear(&monitor);
}

/* The session stage's SIGN: paced at SSHD_SIGN_INTERVAL_MS, h still 32 bytes, no count. */
static void
test_session_sign(void)
{
	struct sshd_monitor monitor;
	struct wire body;
	struct timespec start;
	uint8_t long_h[64];
	unsigned type;
	uint32_t result;
	unsigned index;
	long took;
	int status;

	/* A session reached by a login and the hand-over. */
	fresh(&monitor, &base_config);
	status = login_good(&monitor);
	CHECK_EQ(status, 0);
	wire_reset(&body);
	wire_text(&body, TEST_STATE);
	status = call(&monitor, SSH_MON_AUTH_DONE, &body, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);

	/* Without the pace, more signatures than the pre-authentication bound. */
	for (index = 0; index < SSHD_PREAUTH_SIGN_MAX + 3U; index++) {
		status = sign_h(&monitor, other_h, sizeof(other_h));
		CHECK_EQ(status, 0);
	}

	memset(long_h, 3, sizeof(long_h));
	status = sign_h(&monitor, long_h, 31);
	CHECK_EQ(status, EPROTO);
	status = sign_h(&monitor, long_h, 33);
	CHECK_EQ(status, EPROTO);

	/* With it, two in a row are at least the interval apart. */
	monitor.sign_interval_ms = SSHD_SIGN_INTERVAL_MS;
	status = sign_h(&monitor, other_h, sizeof(other_h));
	CHECK_EQ(status, 0);
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	status = sign_h(&monitor, other_h, sizeof(other_h));
	CHECK_EQ(status, 0);
	took = elapsed_ms(&start);
	CHECK(took >= (long)SSHD_SIGN_INTERVAL_MS - 50L);
	CHECK(took < (long)SSHD_SIGN_INTERVAL_MS + 2000L);
	sshd_monitor_clear(&monitor);
}

/* LOGIN_RECORD and LOGOUT_RECORD in the session stage, on a host where the parent check cannot be made; the log line says which check refused. */
static void
test_records(void)
{
	struct sshd_monitor monitor;
	struct sshd_config config;
	struct wire body;
	char name[64];
	char long_tty[40];
	unsigned type;
	uint32_t result;
	int master;
	int status;

	/* The path rule by itself: "/dev/pts/" and one to eight digits. */
	status = sshd_tty_path_ok("/dev/pts/0");
	CHECK_EQ(status, 0);
	status = sshd_tty_path_ok("/dev/pts/12345678");
	CHECK_EQ(status, 0);
	status = sshd_tty_path_ok("/dev/pts/123456789");
	CHECK_EQ(status, EINVAL);
	status = sshd_tty_path_ok("/dev/pts/");
	CHECK_EQ(status, EINVAL);
	status = sshd_tty_path_ok("/dev/pts/1a");
	CHECK_EQ(status, EINVAL);
	status = sshd_tty_path_ok("/dev/pts/a1");
	CHECK_EQ(status, EINVAL);
	status = sshd_tty_path_ok("/dev/pts/1/");
	CHECK_EQ(status, EINVAL);
	status = sshd_tty_path_ok("/dev/ptsx1");
	CHECK_EQ(status, EINVAL);
	status = sshd_tty_path_ok("/dev/tty1");
	CHECK_EQ(status, EINVAL);
	status = sshd_tty_path_ok(NULL);
	CHECK_EQ(status, EINVAL);

	/* A session reached by a login and the hand-over. */
	fresh(&monitor, &base_config);
	status = login_good(&monitor);
	CHECK_EQ(status, 0);
	wire_reset(&body);
	wire_text(&body, TEST_STATE);
	status = call(&monitor, SSH_MON_AUTH_DONE, &body, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	monitor.session_pid = getpid();

	/* Terminals that are not pseudo-terminals: refused, the connection kept. */
	build_record(&request, "/dev/tty1", 9, (uint32_t)getpid(), 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(type, SSH_MON_RESULT);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: not a pseudo-terminal") != NULL);
	build_record(&request, "/dev/pts/", 9, (uint32_t)getpid(), 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: not a pseudo-terminal") != NULL);
	build_record(&request, "/dev/pts/1a", 11, (uint32_t)getpid(), 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: not a pseudo-terminal") != NULL);

	/* A terminal field with a line end is logged with '?' for it. */
	build_record(&request, "/dev/pts/1\nx", 12, (uint32_t)getpid(), 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(result, EACCES);
	CHECK(strcmp(log_last, "monitor: login record of /dev/pts/1?x refused: not a pseudo-terminal") == 0);

	/* The longest path that fits (eight digits, 17 bytes) is read and checked further; one byte more is not read. */
	build_record(&request, "/dev/pts/12345678", 17, (uint32_t)getpid(), 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: not the user's terminal") != NULL);
	build_record(&request, "/dev/pts/123456789", 18, (uint32_t)getpid(), 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: not a terminal path and a process id") != NULL);

	/* A tty too long, holding a NUL, empty; a pid of 0 and one past 2^31 - 1. */
	memset(long_tty, '1', sizeof(long_tty));
	memcpy(long_tty, "/dev/pts/", 9);
	build_record(&request, long_tty, sizeof(long_tty), (uint32_t)getpid(), 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: not a terminal path and a process id") != NULL);
	build_record(&request, "/dev/pts/1\0", 11, (uint32_t)getpid(), 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: not a terminal path and a process id") != NULL);
	build_record(&request, "", 0, (uint32_t)getpid(), 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: not a terminal path and a process id") != NULL);
	build_record(&request, "/dev/pts/1", 10, 0, 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: not a terminal path and a process id") != NULL);
	build_record(&request, "/dev/pts/1", 10, 0x80000000UL, 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: not a terminal path and a process id") != NULL);
	build_record(&request, "/dev/pts/1", 10, 0x7fffffffUL, 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: not a terminal path and a process id") == NULL);

	/*
	 * The test's own terminal: refused for another user's uid (the owner
	 * check), and for the user by the parent check, which this host has
	 * no process list for.
	 */
	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0 && ptsname_r(master, name, sizeof(name)) == 0) {
		build_record(&request, name, strlen(name), (uint32_t)getpid(), 0);
		monitor.user_uid = getuid() + 1U;
		status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
		CHECK_EQ(status, 0);
		CHECK_EQ(result, EACCES);
		CHECK(strstr(log_last, "refused: not the user's terminal") != NULL);
		monitor.user_uid = getuid();
		status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
		CHECK_EQ(status, 0);
		CHECK_EQ(result, EACCES);
		CHECK(strstr(log_last, "refused: not a process of the session") != NULL);
	}

	if (master >= 0)
		(void)close(master);

	/* A body with a byte after the pid, or cut short, ends the connection. */
	build_record(&request, "/dev/pts/1", 10, 100, 1);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);
	build_record(&request, "/dev/pts/1", 10, 100, 0);
	request.size--;
	status = call(&monitor, SSH_MON_LOGOUT_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, EPROTO);

	/* A logout of a record this session did not write, and of one it did but with another pid or terminal. */
	build_record(&request, "/dev/pts/1", 10, 100, 0);
	status = call(&monitor, SSH_MON_LOGOUT_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: no such record") != NULL);
	strcpy(monitor.records[0].tty, "/dev/pts/1");
	monitor.records[0].pid = 100;
	monitor.records[0].used = 1;
	build_record(&request, "/dev/pts/1", 10, 101, 0);
	status = call(&monitor, SSH_MON_LOGOUT_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	build_record(&request, "/dev/pts/2", 10, 100, 0);
	status = call(&monitor, SSH_MON_LOGOUT_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK_EQ(monitor.records[0].used, 1);

	/* A second login record for a terminal that has one is refused before the terminal is looked at. */
	build_record(&request, "/dev/pts/1", 10, (uint32_t)getpid(), 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: the terminal has a record") != NULL);
	monitor.records[0].used = 0;
	sshd_monitor_clear(&monitor);

	/* With MaxSessions 0 there is no room for any record. */
	config = base_config;
	config.max_sessions = 0;
	fresh(&monitor, &config);
	status = login_good(&monitor);
	CHECK_EQ(status, 0);
	status = call(&monitor, SSH_MON_AUTH_DONE, &body, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	build_record(&request, "/dev/pts/1", 10, (uint32_t)getpid(), 0);
	status = call(&monitor, SSH_MON_LOGIN_RECORD, &request, &type, &result, NULL, NULL);
	CHECK_EQ(status, 0);
	CHECK_EQ(result, EACCES);
	CHECK(strstr(log_last, "refused: no room for another record") != NULL);
	sshd_monitor_clear(&monitor);
}

/*
 * The whole monitor with this program as its children: a login and a
 * session, a pre-authentication process that breaks the protocol, one
 * that overstays LoginGraceTime, one that ends without logging in, and a
 * session that breaks the protocol.
 */
static void
test_run(void)
{
	struct sshd_config config;
	char report[512];
	int grant;
	long took;
	int status;

	/* A login and a session: both children report, the listener is told once. */
	config = base_config;
	status = run_scenario("LL", &config, 0, report, sizeof(report), &grant, &took);
	CHECK_EQ(status, 0);
	CHECK(strstr(report, "P:config ok\n") != NULL);
	CHECK(strstr(report, "P:sent auth-done\n") != NULL);
	CHECK(strstr(report, "S:ok\n") != NULL);
	CHECK(strstr(report, "fail") == NULL);
	CHECK_EQ(grant, 1);
	if (strstr(report, "fail") != NULL)
		printf("report: %s", report);
	CHECK(took >= (long)SSHD_FAIL_DELAY_MS + (long)SSHD_SIGN_INTERVAL_MS);

	/* A pre-authentication process that asks for a login record: killed, no session, no grant. */
	status = run_scenario("VL", &config, 0, report, sizeof(report), &grant, &took);
	CHECK_EQ(status, 1);
	CHECK(strstr(report, "P:sent record\n") != NULL);
	CHECK(strstr(report, "P:answered") == NULL);
	CHECK(strstr(report, "S:") == NULL);
	CHECK_EQ(grant, 0);

	/* One that overstays a LoginGraceTime of one second. */
	config.login_grace_time = 1;
	status = run_scenario("GL", &config, 0, report, sizeof(report), &grant, &took);
	CHECK_EQ(status, 1);
	CHECK(took >= 1000L);
	CHECK(took < 8000L);
	CHECK(strstr(report, "P:woke") == NULL);
	CHECK_EQ(grant, 0);
	config.login_grace_time = 120;

	/* One that ends without a login. */
	status = run_scenario("EL", &config, 0, report, sizeof(report), &grant, &took);
	CHECK_EQ(status, 1);
	CHECK(strstr(report, "S:") == NULL);

	/* A pre-authentication process that sends a frame of no type: killed at once, long before LoginGraceTime. */
	config.login_grace_time = 3;
	status = run_scenario("BL", &config, 0, report, sizeof(report), &grant, &took);
	CHECK_EQ(status, 1);
	CHECK(strstr(report, "P:sent bad frame\n") != NULL);
	CHECK(strstr(report, "P:answered") == NULL);
	CHECK(took < 2000L);
	config.login_grace_time = 120;

	/* A context whose host key blob is not the host key's: no child is started. */
	status = run_scenario("LL", &config, 1, report, sizeof(report), &grant, &took);
	CHECK_EQ(status, 1);
	CHECK_EQ(report[0], 0);
	CHECK_EQ(grant, 0);

	/* A session that sends a second AUTH_DONE: killed after the grant. */
	status = run_scenario("LR", &config, 0, report, sizeof(report), &grant, &took);
	CHECK_EQ(status, 1);
	CHECK(strstr(report, "S:sent auth-done\n") != NULL);
	CHECK(strstr(report, "S:answered") == NULL);
	CHECK_EQ(grant, 1);
}

/*
 * Runs the monitor once: the scenario codes (one for the
 * pre-authentication child, one for the session child) go ahead on the
 * connection, which the children read them from and write their reports
 * to.  Returns the monitor's status; *grant_byte is the number of bytes
 * the listener's pipe got, *took the milliseconds the monitor ran.
 */
static int
run_scenario(
	const char *codes,
	const struct sshd_config *config,
	int bad_blob,
	char *report,
	size_t capacity,
	int *grant_byte,
	long *took)
{
	struct sshd_context context;
	struct ssh_key key;
	struct timespec start;
	struct pollfd waiting;
	int connection[2];
	int started[2];
	char mark[4];
	size_t used;
	ssize_t got;
	int status;
	int ready;
	int wiped;
	int ended;
	size_t index;

	/* The connection, and the listener's pipe. */
	status = socketpair(AF_UNIX, SOCK_STREAM, 0, connection);
	CHECK_EQ(status, 0);
	status = pipe(started);
	CHECK_EQ(status, 0);
	(void)fcntl(connection[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(started[0], F_SETFD, FD_CLOEXEC);
	got = write(connection[0], codes, 2);
	CHECK_EQ(got, 2);

	/* The context of a host without root, and a copy of the host key for the monitor to wipe. */
	memset(&context, 0, sizeof(context));
	context.config = *config;
	memcpy(context.host_key_blob, host_blob.data, SSH_KEY_BLOB_SIZE);
	if (bad_blob)
		context.host_key_blob[SSH_KEY_BLOB_SIZE - 1U] ^= 1;
	strcpy(context.peer, TEST_PEER);
	context.privileged = 0;
	key = host_key;

	/* Runs the monitor. */
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	status = sshd_monitor_run_program(&context, &key, connection[1], started[1], program_path);
	*took = elapsed_ms(&start);

	/* The host key is wiped. */
	wiped = 1;
	for (index = 0; index < sizeof(key.seed); index++) {
		if (key.seed[index] != 0)
			wiped = 0;
	}

	CHECK(wiped);
	CHECK_EQ(key.private, 0);

	/*
	 * Collects the reports until every copy of the connection is closed,
	 * which must be soon: no child outlives the monitor.  A close with the
	 * session's unread scenario byte still queued reads as a reset.
	 */
	used = 0;
	ended = 0;
	for (;;) {
		waiting.fd = connection[0];
		waiting.events = POLLIN;
		waiting.revents = 0;
		ready = poll(&waiting, 1, 5000);
		if (ready <= 0)
			break;
		got = read(connection[0], report + used, capacity - 1U - used);
		if (got == 0 || (got < 0 && errno == ECONNRESET))
			ended = 1;
		if (got <= 0)
			break;
		used += (size_t)got;
		if (used >= capacity - 1U)
			break;
	}

	report[used] = '\0';
	CHECK(ended);
	if (!ended)
		printf("scenario %s: no end of file; report: %s\n", codes, report);
	(void)close(connection[0]);

	/* Counts what the listener's pipe got; the monitor closed its end. */
	*grant_byte = 0;
	got = read(started[0], mark, sizeof(mark));
	if (got > 0) {
		*grant_byte = (int)got;
		CHECK_EQ(mark[0], 1);
	}

	(void)close(started[0]);
	return status;
}

/* Reads one whole frame from descriptor into type and body (a child's side; input keeps the bytes between calls). */
static int
child_read_frame(
	int descriptor,
	struct ssh_buf *input,
	enum ssh_mon_type *type,
	struct ssh_buf *body)
{
	uint8_t chunk[4096];
	ssize_t got;
	int taken;

	/* Reads until a frame is whole. */
	for (;;) {
		taken = ssh_mon_next_frame(input, type, body);
		if (taken != EAGAIN)
			return taken;
		got = read(descriptor, chunk, sizeof(chunk));
		if (got <= 0)
			return EIO;
		ssh_buf_put_bytes(input, chunk, (size_t)got);
	}
}

/* Checks a CONFIG body as a child would: the text parses, the blob is the host key's, the peer and the mode are the run's. */
static int
child_check_config(
	const struct ssh_buf *body)
{
	struct ssh_reader reader;
	struct sshd_config parsed;
	char error_text[SSH_CONFIG_ERROR_MAX];
	char peer[64];
	const uint8_t *text;
	const uint8_t *blob;
	size_t text_size;
	size_t blob_size;
	uint32_t privileged;
	int status;

	/* The four fields. */
	ssh_reader_init(&reader, body->data, body->size);
	ssh_reader_get_string(&reader, &text, &text_size);
	ssh_reader_get_string(&reader, &blob, &blob_size);
	ssh_reader_get_cstring(&reader, peer, sizeof(peer));
	privileged = ssh_reader_get_u32(&reader);
	status = ssh_reader_finish(&reader);
	if (status != 0)
		return 1;

	/* The text is settings; the file of the run is in them. */
	sshd_config_defaults(&parsed);
	status = sshd_config_parse(&parsed, (const char *)text, text_size, error_text);
	if (status != 0 || strcmp(parsed.authorized_keys_file, keys_path) != 0)
		return 2;

	/* The blob, the peer, the mode. */
	if (blob_size != SSH_KEY_BLOB_SIZE || memcmp(blob, host_blob.data, SSH_KEY_BLOB_SIZE) != 0)
		return 3;
	if (strcmp(peer, TEST_PEER) != 0 || privileged != 0)
		return 4;
	return 0;
}

/* Reports whether any descriptor above SSHD_FD_MONITOR, up to 255, is open (1 or 0). */
static int
child_other_descriptors(void)
{
	int descriptor;

	/* Asks each number for its flags, which only an open descriptor has. */
	for (descriptor = SSHD_FD_MONITOR + 1; descriptor < 256; descriptor++) {
		if (fcntl(descriptor, F_GETFD) != -1)
			return 1;
	}

	return 0;
}

/*
 * Reports whether the parent (the monitor) still holds the connection
 * this process has at SSHD_FD_CLIENT: some /proc/<parent>/fd entry names
 * the same socket (this is the host's Linux interface, used by the test
 * only).  Returns 1 or 0.
 */
static int
child_parent_holds_connection(void)
{
	struct stat own;
	struct stat other;
	struct dirent *entry;
	char directory[64];
	char path[128];
	DIR *listing;
	int held;

	/* What the connection is. */
	if (fstat(SSHD_FD_CLIENT, &own) != 0)
		return 0;

	/* Looks through the parent's descriptors. */
	(void)snprintf(directory, sizeof(directory), "/proc/%ld/fd", (long)getppid());
	listing = opendir(directory);
	if (listing == NULL)
		return 0;
	held = 0;
	while ((entry = readdir(listing)) != NULL) {
		(void)snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		if (stat(path, &other) == 0 && other.st_ino == own.st_ino && other.st_dev == own.st_dev)
			held = 1;
	}

	(void)closedir(listing);
	return held;
}

/* Writes one report line to the connection. */
static void
child_report(
	const char *text)
{
	size_t length;
	ssize_t written;

	/* The line goes to the test's end of the connection. */
	length = strlen(text);
	written = write(SSHD_FD_CLIENT, text, length);
	(void)written;
}

/*
 * The fake pre-authentication process: reads its scenario code from the
 * connection and the settings from the monitor, then L logs in and hands
 * over, V asks for a login record, G sleeps past LoginGraceTime, E ends.
 */
static void
fake_preauth(void)
{
	struct ssh_buf input;
	struct ssh_buf body;
	struct ssh_buf reply;
	enum ssh_mon_type type;
	char code;
	ssize_t got;
	int status;

	/* Takes the scenario and the settings. */
	signal(SIGPIPE, SIG_IGN);
	got = read(SSHD_FD_CLIENT, &code, 1);
	if (got != 1)
		_exit(2);
	ssh_buf_init(&input, 4U + SSH_MON_STATE_MAX, 1);
	ssh_buf_init(&body, SSH_MON_STATE_MAX, 1);
	ssh_buf_init(&reply, SSH_MON_STATE_MAX, 1);
	status = child_read_frame(SSHD_FD_MONITOR, &input, &type, &body);
	if (status != 0 || type != SSH_MON_CONFIG || child_check_config(&body) != 0) {
		child_report("P:config fail\n");
		_exit(3);
	}

	child_report("P:config ok\n");

	/* Only the two descriptors and the standard ones are open. */
	if (child_other_descriptors())
		child_report("P:descriptor fail\n");

	/* The scenarios without a login. */
	if (code == 'E')
		_exit(0);
	if (code == 'G') {
		sleep(20);
		child_report("P:woke\n");
		_exit(0);
	}

	if (code == 'B') {
		static const uint8_t bad[5] = { 0, 0, 0, 1, 99 };
		child_report("P:sent bad frame\n");
		got = write(SSHD_FD_MONITOR, bad, sizeof(bad));
		status = child_read_frame(SSHD_FD_MONITOR, &input, &type, &body);
		if (status == 0)
			child_report("P:answered\n");
		_exit(0);
	}

	if (code == 'V') {
		build_record(&request, "/dev/pts/1", 10, 1, 0);
		child_report("P:sent record\n");
		status = ssh_mon_call(SSHD_FD_MONITOR, SSH_MON_LOGIN_RECORD, request.data, request.size, &type, &reply);
		if (status == 0)
			child_report("P:answered\n");
		_exit(0);
	}

	/* L: the exchange's signature, the login, the hand-over. */
	wire_reset(&request);
	wire_string(&request, session_h, sizeof(session_h));
	status = ssh_mon_call(SSHD_FD_MONITOR, SSH_MON_SIGN, request.data, request.size, &type, &reply);
	if (status != 0 || type != SSH_MON_SIGNATURE)
		child_report("P:sign fail\n");
	build_signed(&signed_data, session_h, 32, 50, self_name, "ssh-connection", "publickey", 1, "ssh-ed25519", &user_blob, 0);
	build_login(&request, self_name, &user_blob, &signed_data, &user_key, 0);
	status = ssh_mon_call(SSHD_FD_MONITOR, SSH_MON_PUBKEY_LOGIN, request.data, request.size, &type, &reply);
	if (status != 0 || type != SSH_MON_RESULT || reply.size != 4 || reply.data[3] != 0)
		child_report("P:login fail\n");
	ssh_buf_reset(&reply);
	ssh_buf_put_string(&reply, TEST_STATE, strlen(TEST_STATE));
	ssh_buf_reset(&input);
	status = ssh_mon_put_frame(SSH_MON_AUTH_DONE, reply.data, reply.size, &input);
	child_report("P:sent auth-done\n");
	got = write(SSHD_FD_MONITOR, input.data, input.size);
	(void)got;

	/* The monitor ends this process now. */
	sleep(20);
	child_report("P:not killed fail\n");
	_exit(0);
}

/*
 * The fake session process: reads its scenario code, the settings and
 * SESSION_START, checks what it was started with, then L signs twice and
 * asks for a record, R sends AUTH_DONE.
 */
static void
fake_session(void)
{
	extern char **environ;
	struct ssh_buf input;
	struct ssh_buf body;
	struct ssh_buf reply;
	struct ssh_reader reader;
	struct timespec start;
	enum ssh_mon_type type;
	const uint8_t *state;
	size_t state_size;
	char user[SSH_USER_MAX + 1];
	char code;
	ssize_t got;
	int status;

	/* Takes the scenario, the settings, the user and the transport. */
	signal(SIGPIPE, SIG_IGN);
	got = read(SSHD_FD_CLIENT, &code, 1);
	if (got != 1)
		_exit(2);
	ssh_buf_init(&input, 4U + SSH_MON_STATE_MAX, 1);
	ssh_buf_init(&body, SSH_MON_STATE_MAX, 1);
	ssh_buf_init(&reply, SSH_MON_STATE_MAX, 1);
	status = child_read_frame(SSHD_FD_MONITOR, &input, &type, &body);
	if (status != 0 || type != SSH_MON_CONFIG || child_check_config(&body) != 0) {
		child_report("S:config fail\n");
		_exit(3);
	}

	status = child_read_frame(SSHD_FD_MONITOR, &input, &type, &body);
	if (status != 0 || type != SSH_MON_SESSION_START) {
		child_report("S:start fail\n");
		_exit(3);
	}

	ssh_reader_init(&reader, body.data, body.size);
	ssh_reader_get_cstring(&reader, user, sizeof(user));
	ssh_reader_get_string(&reader, &state, &state_size);
	status = ssh_reader_finish(&reader);
	if (status != 0 || strcmp(user, self_name) != 0 || state_size != strlen(TEST_STATE) || memcmp(state, TEST_STATE, state_size) != 0)
		child_report("S:start contents fail\n");

	/* Started with an empty environment and only its two descriptors above the standard ones. */
	if (environ != NULL && environ[0] != NULL)
		child_report("S:environment fail\n");
	if (child_other_descriptors())
		child_report("S:descriptor fail\n");

	/* The monitor let its copy of the connection go once this process had it. */
	if (child_parent_holds_connection())
		child_report("S:monitor kept connection fail\n");

	/* R: a second hand-over, which the monitor answers by ending this process. */
	if (code == 'R') {
		ssh_buf_reset(&reply);
		ssh_buf_put_string(&reply, TEST_STATE, strlen(TEST_STATE));
		child_report("S:sent auth-done\n");
		wire_reset(&request);
		wire_bytes(&request, reply.data, reply.size);
		status = ssh_mon_call(SSHD_FD_MONITOR, SSH_MON_AUTH_DONE, request.data, request.size, &type, &reply);
		if (status == 0)
			child_report("S:answered\n");
		_exit(0);
	}

	/* L: two re-exchange signatures, paced by the monitor. */
	wire_reset(&request);
	wire_string(&request, other_h, sizeof(other_h));
	status = ssh_mon_call(SSHD_FD_MONITOR, SSH_MON_SIGN, request.data, request.size, &type, &reply);
	if (status != 0 || type != SSH_MON_SIGNATURE)
		child_report("S:sign fail\n");
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	status = ssh_mon_call(SSHD_FD_MONITOR, SSH_MON_SIGN, request.data, request.size, &type, &reply);
	if (status != 0 || type != SSH_MON_SIGNATURE)
		child_report("S:sign fail\n");
	if (elapsed_ms(&start) < (long)SSHD_SIGN_INTERVAL_MS - 50L)
		child_report("S:interval fail\n");

	/* A login record this host cannot check: refused, the session kept. */
	build_record(&request, "/dev/tty1", 9, (uint32_t)getpid(), 0);
	status = ssh_mon_call(SSHD_FD_MONITOR, SSH_MON_LOGIN_RECORD, request.data, request.size, &type, &reply);
	if (status != 0 || type != SSH_MON_RESULT || reply.size != 4 || reply.data[3] != EACCES)
		child_report("S:record fail\n");
	child_report("S:ok\n");
	_exit(0);
}
