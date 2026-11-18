/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The monitor of one connection: the privileged half of sshd.
 *
 * The monitor starts the pre-authentication process, answers its
 * requests (sshd-monitor.h says which and in what order), starts the
 * session process once a login is granted and the transport is handed
 * over, and answers the session's requests until it ends.  It never reads
 * the client's connection itself.  Every request is checked against the
 * state the monitor keeps, never against what the child says about
 * itself.
 *
 * The settings travel to the children as sshd_config text that the
 * monitor writes from the parsed settings it was given and reads back
 * before it sends it (sshd_config_put_text), so a child sees exactly the
 * settings the monitor applies.  The accepted environment of a session
 * (LANG, LC_* and TERM) is not a setting: the session process fixes it,
 * and nothing of AcceptEnv travels.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#if defined(__ZEDBSD__)
#include <sys/ioctl.h>
#include <uapi/system.h>
#endif

#include "sshd-monitor.h"

/* The bytes of a RESULT body: one uint32. */
#define MONITOR_RESULT_SIZE	4U

/* The largest signature blob body (a string of the 83-byte blob, with room). */
#define MONITOR_SIGNATURE_MAX	128U

/* The most bytes one read of the monitor socket asks for. */
#define MONITOR_READ_CHUNK	4096U

/* The most bytes of frames waiting to be handled: one frame at the largest limit with its length. */
#define MONITOR_INPUT_MAX	(4U + SSH_MON_STATE_MAX)

/* The largest reply frame the monitor sends to a request. */
#define MONITOR_REPLY_MAX	(SSH_MON_FRAME_MAX + 8U)

/* The first descriptor number a starting child parks its two descriptors at, above the ones it moves them to. */
#define MONITOR_PARK_DESCRIPTOR	10

/* The longest wait of one poll, so that a long LoginGraceTime cannot overflow the poll's argument. */
#define MONITOR_POLL_MAX_MS	60000L

/* The nanoseconds of one second and of one millisecond. */
#define MONITOR_NS_PER_SECOND	1000000000L
#define MONITOR_NS_PER_MS	1000000L

/* The room for the decimal digits of an unsigned long, with the NUL. */
#define MONITOR_DECIMAL_MAX	24U

/* How a value is written as a word of sshd_config text. */
#define MONITOR_WORD_PLAIN	0
#define MONITOR_WORD_QUOTED	1
#define MONITOR_WORD_NONE	2

/*
 * Values referenced: RFC 4252 section 7, the data a publickey signature
 * covers: string session identifier, byte SSH_MSG_USERAUTH_REQUEST,
 * string user name, string service name, string "publickey", boolean
 * TRUE, string public key algorithm name, string public key blob.
 */
#define MONITOR_METHOD_PUBLICKEY	"publickey"

/* The room for the " from <address> port <port>" part of a log line, with the NUL. */
#define MONITOR_FROM_MAX	96U

/*
 * The connection's peer as "address port" for the log lines of logins,
 * empty until sshd_monitor_run_program sets it before the first request.
 * A monitor process serves one connection, so it is set once and only
 * read afterwards.
 */
static char monitor_peer[64];

static int monitor_sign(struct sshd_monitor *monitor, const uint8_t *body, size_t size, struct ssh_buf *reply);
static int monitor_pubkey_query(struct sshd_monitor *monitor, const uint8_t *body, size_t size, const struct timespec *arrived, struct ssh_buf *reply);
static int monitor_pubkey_login(struct sshd_monitor *monitor, const uint8_t *body, size_t size, const struct timespec *arrived, struct ssh_buf *reply);
static int monitor_password_login(struct sshd_monitor *monitor, const uint8_t *body, size_t size, const struct timespec *arrived, struct ssh_buf *reply);
static int monitor_auth_done(struct sshd_monitor *monitor, const uint8_t *body, size_t size);
static int monitor_login_record(struct sshd_monitor *monitor, const uint8_t *body, size_t size, struct ssh_buf *reply);
static int monitor_logout_record(struct sshd_monitor *monitor, const uint8_t *body, size_t size, struct ssh_buf *reply);
static int monitor_read_record(const uint8_t *body, size_t size, char *tty, pid_t *pid, int *valid);
static int monitor_begin_attempt(struct sshd_monitor *monitor, int query);
static int monitor_take_user(struct sshd_monitor *monitor, const char *user);
static int monitor_signed_data_ok(const struct sshd_monitor *monitor, const uint8_t *data, size_t size, const uint8_t *blob, size_t blob_size);
static int monitor_string_is(const uint8_t *data, size_t size, const char *text);
static int monitor_key_allowed(const struct sshd_monitor *monitor, const uint8_t *public_key, uid_t *uid);
static int monitor_pid_is_child(pid_t pid, pid_t parent, uid_t uid);
static void monitor_grant(struct sshd_monitor *monitor, uid_t uid);
static int monitor_put_result(uint32_t result, struct ssh_buf *reply);
static void monitor_time_add_ms(struct timespec *time, unsigned ms);
static int monitor_time_before(const struct timespec *left, const struct timespec *right);
static void monitor_wait_until(const struct timespec *start, unsigned ms);
static int monitor_word_kind(const char *text);
static int monitor_put_line(struct ssh_buf *out, const char *keyword, const char *first, const char *second);
static int monitor_put_number_line(struct ssh_buf *out, const char *keyword, unsigned long value);
static void monitor_decimal(unsigned long value, char *text);
static int monitor_config_equal(const struct sshd_config *left, const struct sshd_config *right);
static int monitor_config_body(const struct sshd_context *context, const struct ssh_key *host_key, struct ssh_buf *body);
static int monitor_start_child(const char *program, const char *mode, int client, pid_t *child, int *socket_end);
static void monitor_exec_child(const char *program, const char *mode, int client, int socket_end);
static int monitor_send_frame(int descriptor, enum ssh_mon_type type, const struct ssh_buf *body);
static int monitor_write_all(int descriptor, const uint8_t *data, size_t size);
static int monitor_serve(struct sshd_monitor *monitor, int socket_end, const struct timespec *deadline, int *started);
static int monitor_poll_ms(const struct timespec *deadline);
static void monitor_report_grant(int *started);
static void monitor_stop_child(pid_t child, int socket_end);
static void monitor_reap(pid_t child);
static void monitor_log_text(const char *text, char *out, size_t size);
static void monitor_log_from(char *out, size_t size);

/*
 * Runs the monitor of one connection on descriptor client until the
 * connection ends, starting the children from SSHD_PROGRAM.
 */
int
sshd_monitor_run(
	const struct sshd_context *context,
	struct ssh_key *host_key,
	int client,
	int started)
{
	int status;

	/* Runs the monitor with the installed program. */
	status = sshd_monitor_run_program(context, host_key, client, started, SSHD_PROGRAM);

	/* Reports a monitor that did not see its session end normally. */
	if (status != 0)
		return status;

	/* Succeeded: the session ran and ended. */
	return 0;
}

/*
 * Runs the monitor of one connection, starting the children from
 * program.
 *
 * The pre-authentication process gets LoginGraceTime in all (0: no
 * limit), then is killed; it is killed as well right after AUTH_DONE, so
 * it can never touch the connection once the session process has it.
 * The session process is waited for; when it ends, the login records it
 * left open are closed.  The host key is wiped before the function
 * returns.  Returns 0 when a session ran and ended, 1 otherwise.
 */
int
sshd_monitor_run_program(
	const struct sshd_context *context,
	struct ssh_key *host_key,
	int client,
	int started,
	const char *program)
{
	struct sshd_monitor monitor;
	struct ssh_buf config_body;
	struct ssh_buf start_body;
	struct timespec deadline;
	struct timespec *limit;
	pid_t child;
	int socket_end;
	int prepared;
	int launched;
	int sent;
	int served;
	int exit_status;
	int privileged;
	uid_t self;

	/* Starts with nothing to undo. */
	exit_status = 1;
	child = -1;
	socket_end = -1;
	ssh_buf_init(&config_body, SSH_MON_STATE_MAX, 0);
	ssh_buf_init(&start_body, SSH_MON_STATE_MAX, 1);

	/* Refuses a call that could only run wrongly. */
	if (context == NULL ||
	    host_key == NULL ||
	    program == NULL ||
	    client < 0 ||
	    !host_key->private)
		goto done;

	/* Refuses a context that does not say how sshd runs. */
	self = geteuid();
	privileged = 0;
	if (self == 0)
		privileged = 1;
	if (privileged != (context->privileged != 0)) {
		syslog(LOG_AUTH | LOG_ERR, "monitor: the context does not match the user sshd runs as");
		goto done;
	}

	/* Keeps the peer for the log lines of the logins, as the log may show it. */
	monitor_log_text(context->peer, monitor_peer, sizeof(monitor_peer));

	/* A child that closes its socket must not end the monitor with SIGPIPE. */
	(void)signal(SIGPIPE, SIG_IGN);

	/* Builds the settings the children are started with. */
	prepared = monitor_config_body(context, host_key, &config_body);
	if (prepared != 0) {
		syslog(LOG_AUTH | LOG_ERR, "monitor: the settings cannot be passed on");
		goto done;
	}

	/* Sets up the monitor state with the settings and the host key. */
	sshd_monitor_init(&monitor, &context->config, host_key, context->privileged);

	/* Starts the pre-authentication process and gives it its settings. */
	launched = monitor_start_child(program, SSHD_EXEC_PREAUTH, client, &child, &socket_end);
	if (launched != 0)
		goto finish;
	sent = monitor_send_frame(socket_end, SSH_MON_CONFIG, &config_body);
	if (sent != 0) {
		monitor_stop_child(child, socket_end);
		goto finish;
	}

	/* Answers it until a login is granted and the transport handed over, within LoginGraceTime. */
	limit = NULL;
	if (context->config.login_grace_time != 0) {
		(void)clock_gettime(CLOCK_MONOTONIC, &deadline);
		deadline.tv_sec += (time_t)context->config.login_grace_time;
		limit = &deadline;
	}

	/* Serves the pre-authentication process until it ends or the grace time runs out. */
	served = monitor_serve(&monitor, socket_end, limit, &started);

	/* Ends the pre-authentication process whatever happened: it has nothing more to do. */
	monitor_stop_child(child, socket_end);
	child = -1;
	socket_end = -1;
	if (served != 0) {
		syslog(LOG_AUTH | LOG_NOTICE, "monitor: connection from %.63s ended before a session (%d)", context->peer, served);
		goto finish;
	}

	/* Starts the session process and gives it its settings, the user and the transport. */
	launched = monitor_start_child(program, SSHD_EXEC_SESSION, client, &child, &socket_end);
	if (launched != 0)
		goto finish;
	ssh_buf_put_cstring(&start_body, monitor.user);
	ssh_buf_put_string(&start_body, monitor.state.data, monitor.state.size);
	ssh_buf_free(&monitor.state);
	sent = monitor_send_frame(socket_end, SSH_MON_CONFIG, &config_body);
	if (sent == 0)
		sent = monitor_send_frame(socket_end, SSH_MON_SESSION_START, &start_body);
	ssh_buf_free(&start_body);
	if (sent != 0) {
		monitor_stop_child(child, socket_end);
		goto finish;
	}

	/* The session process holds the connection now; the monitor lets its copy go. */
	(void)close(client);
	client = -1;
	monitor.session_pid = child;

	/* Answers the session process until it ends. */
	served = monitor_serve(&monitor, socket_end, NULL, &started);
	if (served == EPIPE) {
		(void)close(socket_end);
		monitor_reap(child);
		exit_status = 0;
	} else {
		syslog(LOG_AUTH | LOG_NOTICE, "monitor: session of %.64s ended by the monitor (%d)", monitor.user, served);
		monitor_stop_child(child, socket_end);
	}

	/* Closes the login records the session left open. */
	sshd_monitor_end_session(&monitor);

finish:
	sshd_monitor_clear(&monitor);

done:
	/* Wipes the host key and the transport, and lets the descriptors go. */
	ssh_key_clear(host_key);
	ssh_buf_free(&config_body);
	ssh_buf_free(&start_body);
	if (client >= 0)
		(void)close(client);
	if (started >= 0)
		(void)close(started);

	/* Reports a connection that did not end with its session. */
	if (exit_status != 0)
		return exit_status;

	/* Succeeded: a session ran and ended. */
	return 0;
}

/*
 * Starts the state of a new connection's monitor in the PREAUTH stage.
 */
void
sshd_monitor_init(
	struct sshd_monitor *monitor,
	const struct sshd_config *config,
	const struct ssh_key *host_key,
	int privileged)
{
	/* Starts with nothing known about the connection. */
	memset(monitor, 0, sizeof(*monitor));
	monitor->config = config;
	monitor->host_key = host_key;
	monitor->privileged = privileged;
	monitor->stage = SSHD_STAGE_PREAUTH;
	monitor->fail_delay_ms = SSHD_FAIL_DELAY_MS;
	monitor->sign_interval_ms = SSHD_SIGN_INTERVAL_MS;
	monitor->session_pid = -1;

	/* The exported transport carries the session keys: a secret. */
	ssh_buf_init(&monitor->state, SSH_MON_STATE_MAX, 1);
}

/*
 * Checks and carries out one request frame of a child.
 */
int
sshd_monitor_handle(
	struct sshd_monitor *monitor,
	enum ssh_mon_type type,
	const uint8_t *body,
	size_t size,
	const struct timespec *arrived,
	struct ssh_buf *reply,
	enum sshd_monitor_next *next)
{
	int handled;

	/* Most requests are answered. */
	*next = SSHD_NEXT_REPLY;

	/* Refuses a request the stage does not allow, then carries out the one it does. */
	handled = EPROTO;
	switch (monitor->stage) {
	case SSHD_STAGE_PREAUTH:
		if (type == SSH_MON_SIGN) {
			handled = monitor_sign(monitor, body, size, reply);
		} else if (type == SSH_MON_PUBKEY_QUERY) {
			handled = monitor_pubkey_query(monitor, body, size, arrived, reply);
		} else if (type == SSH_MON_PUBKEY_LOGIN) {
			handled = monitor_pubkey_login(monitor, body, size, arrived, reply);
		} else if (type == SSH_MON_PASSWORD_LOGIN) {
			handled = monitor_password_login(monitor, body, size, arrived, reply);
		}

		break;
	case SSHD_STAGE_GRANTED:
		if (type == SSH_MON_SIGN) {
			handled = monitor_sign(monitor, body, size, reply);
		} else if (type == SSH_MON_AUTH_DONE) {
			handled = monitor_auth_done(monitor, body, size);
			if (handled == 0)
				*next = SSHD_NEXT_START_SESSION;
		}

		break;
	case SSHD_STAGE_SESSION:
		if (type == SSH_MON_SIGN) {
			handled = monitor_sign(monitor, body, size, reply);
		} else if (type == SSH_MON_LOGIN_RECORD) {
			handled = monitor_login_record(monitor, body, size, reply);
		} else if (type == SSH_MON_LOGOUT_RECORD) {
			handled = monitor_logout_record(monitor, body, size, reply);
		}

		break;
	default:
		break;
	}

	/* Reports a request that ends the connection. */
	if (handled != 0) {
		syslog(LOG_AUTH | LOG_WARNING, "monitor: refused request %d in stage %d (%d)", (int)type, (int)monitor->stage, handled);
		return handled;
	}

	/* Succeeded: the request is carried out. */
	return 0;
}

/*
 * Writes the DEAD_PROCESS record of every login record still open.
 */
void
sshd_monitor_end_session(
	struct sshd_monitor *monitor)
{
	unsigned index;
	int written;

	/* Closes each record the session left open. */
	for (index = 0; index < SSH_CHANNEL_MAX; index++) {
		if (!monitor->records[index].used)
			continue;
		written = sshd_login_record(monitor->user, monitor->records[index].tty, monitor->records[index].pid, 0);
		if (written != 0)
			syslog(LOG_AUTH | LOG_ERR, "monitor: logout record of %.17s not written", monitor->records[index].tty);
		monitor->records[index].used = 0;
	}
}

/*
 * Wipes the monitor's secrets (the exported transport) and clears it.
 */
void
sshd_monitor_clear(
	struct sshd_monitor *monitor)
{
	/* Wipes the exported transport, then the rest of what was known. */
	ssh_buf_free(&monitor->state);
	ssh_wipe(monitor->session_id, sizeof(monitor->session_id));
	memset(monitor, 0, sizeof(*monitor));
	monitor->session_pid = -1;
}

/*
 * Appends to out the sshd_config text the monitor sends its children in
 * CONFIG, checking that sshd_config_parse reads it back into the same
 * settings.
 */
int
sshd_config_put_text(
	const struct sshd_config *config,
	struct ssh_buf *out)
{
	struct sshd_config parsed;
	char error_text[SSH_CONFIG_ERROR_MAX];
	const char *name;
	size_t start;
	unsigned index;
	int status;
	int read_back;
	int equal;

	/* Refuses a call without its arguments, and an out that has failed. */
	if (config == NULL || out == NULL)
		return EINVAL;
	if (out->error != 0)
		return out->error;
	start = out->size;

	/* Writes where sshd listens. */
	status = monitor_put_number_line(out, "Port", config->port);
	if (status == 0)
		status = monitor_put_line(out, "ListenAddress", config->listen_address, NULL);
	if (status == 0) {
		if (config->inet6) {
			status = monitor_put_line(out, "AddressFamily", "inet6", NULL);
		} else {
			status = monitor_put_line(out, "AddressFamily", "inet", NULL);
		}
	}

	/* Writes the host keys in their order. */
	for (index = 0; status == 0 && index < config->host_key_count && index < SSH_CONFIG_KEYS_MAX; index++)
		status = monitor_put_line(out, "HostKey", config->host_key[index], NULL);
	if (config->host_key_count > SSH_CONFIG_KEYS_MAX)
		status = EINVAL;

	/* Writes who may log in, and how. */
	name = NULL;
	if (config->permit_root == SSHD_PERMIT_ROOT_YES) {
		name = "yes";
	} else if (config->permit_root == SSHD_PERMIT_ROOT_NO) {
		name = "no";
	} else if (config->permit_root == SSHD_PERMIT_ROOT_PROHIBIT_PASSWORD) {
		name = "prohibit-password";
	}

	/* Refuses a setting that has no name. */
	if (name == NULL)
		status = EINVAL;
	if (status == 0)
		status = monitor_put_line(out, "PermitRootLogin", name, NULL);
	if (status == 0)
		status = monitor_put_line(out, "PasswordAuthentication", config->password ? "yes" : "no", NULL);
	if (status == 0)
		status = monitor_put_line(out, "PermitEmptyPasswords", config->permit_empty ? "yes" : "no", NULL);
	if (status == 0)
		status = monitor_put_line(out, "PubkeyAuthentication", config->pubkey ? "yes" : "no", NULL);
	if (status == 0)
		status = monitor_put_line(out, "AuthorizedKeysFile", config->authorized_keys_file, NULL);
	if (status == 0)
		status = monitor_put_line(out, "StrictModes", config->strict_modes ? "yes" : "no", NULL);

	/* Writes the limits. */
	if (status == 0)
		status = monitor_put_number_line(out, "MaxAuthTries", config->max_auth_tries);
	if (status == 0)
		status = monitor_put_number_line(out, "LoginGraceTime", config->login_grace_time);
	if (status == 0)
		status = monitor_put_number_line(out, "MaxSessions", config->max_sessions);
	if (status == 0)
		status = monitor_put_number_line(out, "MaxStartups", config->max_startups);
	if (status == 0)
		status = monitor_put_number_line(out, "ClientAliveInterval", config->client_alive_interval);
	if (status == 0)
		status = monitor_put_number_line(out, "ClientAliveCountMax", config->client_alive_count);

	/* Writes the sftp subsystem when there is one. */
	if (status == 0 && config->sftp_command[0] != '\0')
		status = monitor_put_line(out, "Subsystem", "sftp", config->sftp_command);

	/* Takes back what was written when a setting cannot be carried. */
	if (status == 0)
		status = ssh_buf_error(out);
	if (status != 0) {
		if (out->error == 0)
			out->size = start;
		return status;
	}

	/* Reads the text back as a child will, and requires the same settings. */
	sshd_config_defaults(&parsed);
	read_back = sshd_config_parse(&parsed, (const char *)out->data + start, out->size - start, error_text);
	if (read_back != 0) {
		out->size = start;
		return EINVAL;
	}

	/* Compares the parsed settings with the ones that were written. */
	equal = monitor_config_equal(config, &parsed);
	if (!equal) {
		out->size = start;
		return EINVAL;
	}

	/* Succeeded: out ends with text a child reads into these settings. */
	return 0;
}

/*
 * Signs h for the transport: the first h becomes the session identifier,
 * at most SSHD_PREAUTH_SIGN_MAX before the session process, and in the
 * session stage no sooner than sign_interval_ms after the last.
 */
static int
monitor_sign(
	struct sshd_monitor *monitor,
	const uint8_t *body,
	size_t size,
	struct ssh_buf *reply)
{
	struct ssh_reader reader;
	struct ssh_buf signature;
	struct ssh_buf signature_body;
	const uint8_t *h;
	size_t h_size;
	int finished;
	int signed_h;
	int framed;

	/* Reads the one field, with nothing after it. */
	ssh_reader_init(&reader, body, size);
	ssh_reader_get_string(&reader, &h, &h_size);
	finished = ssh_reader_finish(&reader);
	if (finished != 0)
		return EPROTO;

	/* Refuses an h that is not an exchange hash of the one method (SHA-256). */
	if (h_size != SSH_SHA256_SIZE)
		return EPROTO;

	/* Bounds the signatures of the pre-authentication process, and paces those of the session. */
	if (monitor->stage == SSHD_STAGE_SESSION) {
		if (monitor->have_last_sign)
			monitor_wait_until(&monitor->last_sign, monitor->sign_interval_ms);
	} else {
		if (monitor->preauth_signs >= SSHD_PREAUTH_SIGN_MAX)
			return EPROTO;
		monitor->preauth_signs++;
	}

	/* The first exchange hash signed is the session identifier, which no later request can change. */
	if (!monitor->have_session_id) {
		memcpy(monitor->session_id, h, SSH_SHA256_SIZE);
		monitor->have_session_id = 1;
	}

	/* Signs h with the host key. */
	ssh_buf_init(&signature, MONITOR_SIGNATURE_MAX, 0);
	signed_h = ssh_key_sign(monitor->host_key, h, h_size, &signature);
	if (signed_h != 0) {
		ssh_buf_free(&signature);
		return EIO;
	}

	/* Records when the host key signed last. */
	(void)clock_gettime(CLOCK_MONOTONIC, &monitor->last_sign);
	monitor->have_last_sign = 1;

	/* Answers with the signature blob. */
	ssh_buf_init(&signature_body, MONITOR_SIGNATURE_MAX + 4U, 0);
	ssh_buf_put_string(&signature_body, signature.data, signature.size);
	framed = ssh_buf_error(&signature_body);
	if (framed == 0)
		framed = ssh_mon_put_frame(SSH_MON_SIGNATURE, signature_body.data, signature_body.size, reply);
	ssh_buf_free(&signature);
	ssh_buf_free(&signature_body);
	if (framed != 0)
		return EIO;

	/* Succeeded: the signature is the reply. */
	return 0;
}

/*
 * Answers whether a key would be accepted for the user (RFC 4252 7, the
 * query without a signature): the account may log in with a key, and
 * the key is in its authorized_keys file.
 */
static int
monitor_pubkey_query(
	struct sshd_monitor *monitor,
	const uint8_t *body,
	size_t size,
	const struct timespec *arrived,
	struct ssh_buf *reply)
{
	struct ssh_reader reader;
	char user[SSH_USER_MAX + 1];
	uint8_t public_key[SSH_ED25519_PUBLIC];
	const uint8_t *blob;
	size_t blob_size;
	uid_t uid;
	int finished;
	int parsed;
	int status;
	int allowed;

	/* Reads the user and the key blob, with nothing after them. */
	ssh_reader_init(&reader, body, size);
	ssh_reader_get_cstring(&reader, user, sizeof(user));
	ssh_reader_get_string(&reader, &blob, &blob_size);
	finished = ssh_reader_finish(&reader);
	if (finished != 0)
		return EPROTO;

	/* Refuses a blob that is not an ssh-ed25519 key: the process only asks about keys it parsed. */
	parsed = ssh_key_parse_public_blob(blob, blob_size, public_key);
	if (parsed != 0)
		return EPROTO;

	/* Refuses a request out of order, over the limit, or for another user. */
	status = monitor_begin_attempt(monitor, 1);
	if (status != 0)
		return status;
	status = monitor_take_user(monitor, user);
	if (status != 0)
		return status;

	/*
	 * Looks the key up.  A refusal counts one attempt; a key that would be
	 * accepted counts against the bound of granted queries instead, since
	 * a login with it follows.
	 */
	allowed = monitor_key_allowed(monitor, public_key, &uid);
	if (allowed == 0) {
		monitor->granted_queries++;
	} else {
		monitor->tries++;
	}

	/* Answers no sooner than the delay after the request arrived. */
	monitor_wait_until(arrived, monitor->fail_delay_ms);
	if (allowed == 0)
		status = monitor_put_result(0, reply);
	else
		status = monitor_put_result(EACCES, reply);
	if (status != 0)
		return EIO;

	/* Succeeded: the answer is the reply. */
	return 0;
}

/*
 * Checks a signed publickey login: the signed data is exactly the one the
 * monitor expects for its session identifier, user and key; the account
 * may log in with a key; the key is authorized; the signature verifies.
 */
static int
monitor_pubkey_login(
	struct sshd_monitor *monitor,
	const uint8_t *body,
	size_t size,
	const struct timespec *arrived,
	struct ssh_buf *reply)
{
	struct ssh_reader reader;
	char user[SSH_USER_MAX + 1];
	uint8_t public_key[SSH_ED25519_PUBLIC];
	const uint8_t *blob;
	const uint8_t *signed_data;
	const uint8_t *signature;
	size_t blob_size;
	size_t signed_size;
	size_t signature_size;
	uid_t uid;
	int finished;
	int parsed;
	int status;
	int granted;
	int verified;
	int printed;
	char shown[SSH_USER_MAX + 1];
	char from[MONITOR_FROM_MAX];
	char fingerprint[SSH_KEY_FINGERPRINT_SIZE];

	/* Reads the user, the key blob, the signed data and the signature, with nothing after them. */
	ssh_reader_init(&reader, body, size);
	ssh_reader_get_cstring(&reader, user, sizeof(user));
	ssh_reader_get_string(&reader, &blob, &blob_size);
	ssh_reader_get_string(&reader, &signed_data, &signed_size);
	ssh_reader_get_string(&reader, &signature, &signature_size);
	finished = ssh_reader_finish(&reader);
	if (finished != 0)
		return EPROTO;

	/* Refuses a blob that is not an ssh-ed25519 key. */
	parsed = ssh_key_parse_public_blob(blob, blob_size, public_key);
	if (parsed != 0)
		return EPROTO;

	/* Refuses a request out of order, over the limit, or for another user; counts the attempt. */
	status = monitor_begin_attempt(monitor, 0);
	if (status != 0)
		return status;
	status = monitor_take_user(monitor, user);
	if (status != 0)
		return status;
	monitor->tries++;

	/* Takes the user as the log may show it. */
	monitor_log_text(user, shown, sizeof(shown));

	/* Checks, in order, what was signed, who logs in, the key, and the signature. */
	granted = 0;
	status = monitor_signed_data_ok(monitor, signed_data, signed_size, blob, blob_size);
	if (status != 0) {
		syslog(LOG_AUTH | LOG_WARNING, "monitor: signed data for %.64s is not this session's", shown);
	} else {
		status = monitor_key_allowed(monitor, public_key, &uid);
		if (status == 0) {
			verified = ssh_key_verify(public_key, signed_data, signed_size, signature, signature_size);
			if (verified == 0)
				granted = 1;
		}
	}

	/* Names the key and the peer for the log line, as OpenSSH's sshd writes it, so the lines can be counted. */
	monitor_log_from(from, sizeof(from));
	printed = ssh_key_fingerprint(public_key, fingerprint, sizeof(fingerprint));
	if (printed != 0)
		strcpy(fingerprint, "?");

	/* Answers no sooner than the delay after the request arrived. */
	monitor_wait_until(arrived, monitor->fail_delay_ms);
	if (granted) {
		syslog(LOG_AUTH | LOG_INFO, "Accepted publickey for %.64s%s ssh2: ED25519 %s", shown, from, fingerprint);
		status = monitor_put_result(0, reply);
	} else {
		syslog(LOG_AUTH | LOG_NOTICE, "Failed publickey for %.64s%s ssh2: ED25519 %s", shown, from, fingerprint);
		status = monitor_put_result(EACCES, reply);
	}

	/* Fails the monitor when the reply could not be built. */
	if (status != 0)
		return EIO;

	/* A granted login moves the connection on to the hand-over. */
	if (granted)
		monitor_grant(monitor, uid);

	/* Succeeded: the answer is the reply. */
	return 0;
}

/*
 * Checks a password login.  The account check and the password check
 * both run whatever the other answers, so the work does not tell an
 * account that exists from one that does not.
 */
static int
monitor_password_login(
	struct sshd_monitor *monitor,
	const uint8_t *body,
	size_t size,
	const struct timespec *arrived,
	struct ssh_buf *reply)
{
	struct ssh_reader reader;
	struct passwd account;
	char account_buffer[SSHD_ACCOUNT_BUFFER];
	char user[SSH_USER_MAX + 1];
	char password[SSH_PASSWORD_MAX + 1];
	const uint8_t *secret;
	const void *nul;
	size_t secret_size;
	uid_t uid;
	int finished;
	int status;
	int allowed;
	int correct;
	char shown[SSH_USER_MAX + 1];
	char from[MONITOR_FROM_MAX];

	/* Reads the user and the password, with nothing after them. */
	ssh_reader_init(&reader, body, size);
	ssh_reader_get_cstring(&reader, user, sizeof(user));
	ssh_reader_get_string(&reader, &secret, &secret_size);
	finished = ssh_reader_finish(&reader);
	if (finished != 0)
		return EPROTO;

	/* Refuses a password the protocol's parser would not have taken: too long, or holding a NUL. */
	if (secret_size > SSH_PASSWORD_MAX)
		return EPROTO;
	nul = memchr(secret, 0, secret_size);
	if (nul != NULL)
		return EPROTO;

	/* Refuses a request out of order, over the limit, or for another user; counts the attempt. */
	status = monitor_begin_attempt(monitor, 0);
	if (status != 0)
		return status;
	status = monitor_take_user(monitor, user);
	if (status != 0)
		return status;
	monitor->tries++;

	/* Takes a copy of the password to check, which the checks wipe. */
	memcpy(password, secret, secret_size);
	password[secret_size] = '\0';

	/* Checks the account, then the password whatever the account check said. */
	allowed = EACCES;
	uid = 0;
	if (monitor->config->password) {
		allowed = sshd_account_allowed(monitor->config, user, SSH_AUTH_PASSWORD, &account, account_buffer, sizeof(account_buffer));
		if (allowed == 0)
			uid = account.pw_uid;
	}

	/* Checks the password and wipes it. */
	correct = sshd_password_ok(monitor->config, user, password);
	ssh_wipe(password, sizeof(password));

	/* Names the user and the peer for the log line, as OpenSSH's sshd writes it. */
	monitor_log_text(user, shown, sizeof(shown));
	monitor_log_from(from, sizeof(from));

	/* Answers no sooner than the delay after the request arrived. */
	monitor_wait_until(arrived, monitor->fail_delay_ms);
	if (allowed == 0 && correct == 0) {
		syslog(LOG_AUTH | LOG_INFO, "Accepted password for %.64s%s ssh2", shown, from);
		status = monitor_put_result(0, reply);
	} else {
		syslog(LOG_AUTH | LOG_NOTICE, "Failed password for %.64s%s ssh2", shown, from);
		status = monitor_put_result(EACCES, reply);
	}

	/* Fails the monitor when the reply could not be built. */
	if (status != 0)
		return EIO;

	/* A granted login moves the connection on to the hand-over. */
	if (allowed == 0 && correct == 0)
		monitor_grant(monitor, uid);

	/* Succeeded: the answer is the reply. */
	return 0;
}

/*
 * Takes the exported transport of a granted login, once: the stage moves
 * to SESSION, so no second AUTH_DONE is ever allowed.  The bytes are not
 * read here; the session process's import checks them.
 */
static int
monitor_auth_done(
	struct sshd_monitor *monitor,
	const uint8_t *body,
	size_t size)
{
	struct ssh_reader reader;
	const uint8_t *state;
	size_t state_size;
	int finished;
	int kept;

	/* Reads the one field, with nothing after it. */
	ssh_reader_init(&reader, body, size);
	ssh_reader_get_string(&reader, &state, &state_size);
	finished = ssh_reader_finish(&reader);
	if (finished != 0 || state_size == 0)
		return EPROTO;

	/* Keeps the transport for the session process. */
	ssh_buf_reset(&monitor->state);
	ssh_buf_put_bytes(&monitor->state, state, state_size);
	kept = ssh_buf_error(&monitor->state);
	if (kept != 0) {
		ssh_buf_free(&monitor->state);
		return EPROTO;
	}

	/* The session stage starts: the pre-authentication process is done. */
	monitor->stage = SSHD_STAGE_SESSION;

	/* Succeeded: the transport waits for the session process. */
	return 0;
}

/*
 * Writes the USER_PROCESS record of a terminal of the session: the tty is
 * a pseudo-terminal owned by the user, the pid a process of the user
 * whose parent is the session process, and the terminal has no open
 * record of this session.  Refusals are answered with EACCES.
 */
static int
monitor_login_record(
	struct sshd_monitor *monitor,
	const uint8_t *body,
	size_t size,
	struct ssh_buf *reply)
{
	char tty[SSHD_TTY_MAX];
	char shown[SSHD_TTY_MAX];
	const char *reason;
	pid_t pid;
	unsigned index;
	unsigned slot;
	unsigned slots;
	int valid;
	int status;
	int same;
	uint32_t result;

	/* Reads the request, which ends the connection only when it is malformed. */
	status = monitor_read_record(body, size, tty, &pid, &valid);
	if (status != 0)
		return status;

	/* Refuses a field that cannot be a terminal or a process, and a path that is not a pseudo-terminal's. */
	reason = NULL;
	if (!valid)
		reason = "not a terminal path and a process id";
	if (reason == NULL) {
		status = sshd_tty_path_ok(tty);
		if (status != 0)
			reason = "not a pseudo-terminal";
	}

	/* Finds that the terminal has no open record and that there is room for one. */
	slots = monitor->config->max_sessions;
	if (slots > SSH_CHANNEL_MAX)
		slots = SSH_CHANNEL_MAX;
	slot = slots;
	for (index = 0; reason == NULL && index < slots; index++) {
		if (!monitor->records[index].used) {
			if (slot == slots)
				slot = index;
			continue;
		}

		/* Compares the terminal with the one of the record. */
		same = strcmp(monitor->records[index].tty, tty);
		if (same == 0)
			reason = "the terminal has a record";
	}

	/* Refuses a new record when every slot is in use. */
	if (reason == NULL && slot == slots)
		reason = "no room for another record";

	/* Checks the terminal and the process against the user and the session. */
	if (reason == NULL) {
		status = sshd_tty_check(tty, monitor->user_uid);
		if (status != 0)
			reason = "not the user's terminal";
	}

	/* Refuses a process that is not a child of the session. */
	if (reason == NULL) {
		status = monitor_pid_is_child(pid, monitor->session_pid, monitor->user_uid);
		if (status != 0)
			reason = "not a process of the session";
	}

	/* Writes the record and keeps it for its logout, or says why not. */
	result = 0;
	if (reason != NULL) {
		/* Logs the refusal with the terminal as the log may show it. */
		monitor_log_text(tty, shown, sizeof(shown));
		syslog(LOG_AUTH | LOG_NOTICE, "monitor: login record of %.17s refused: %s", shown, reason);
		result = EACCES;
	} else {
		status = sshd_login_record(monitor->user, tty, pid, 1);
		if (status != 0) {
			result = EIO;
		} else {
			memcpy(monitor->records[slot].tty, tty, sizeof(tty));
			monitor->records[slot].pid = pid;
			monitor->records[slot].used = 1;
		}
	}

	/* Answers with the outcome. */
	status = monitor_put_result(result, reply);
	if (status != 0)
		return EIO;

	/* Succeeded: the answer is the reply. */
	return 0;
}

/*
 * Writes the DEAD_PROCESS record of a terminal whose USER_PROCESS record
 * this session had written, and forgets it; another is refused with
 * EACCES.
 */
static int
monitor_logout_record(
	struct sshd_monitor *monitor,
	const uint8_t *body,
	size_t size,
	struct ssh_buf *reply)
{
	char tty[SSHD_TTY_MAX];
	char shown[SSHD_TTY_MAX];
	pid_t pid;
	unsigned index;
	int valid;
	int status;
	int same;
	uint32_t result;

	/* Reads the request, which ends the connection only when it is malformed. */
	status = monitor_read_record(body, size, tty, &pid, &valid);
	if (status != 0)
		return status;

	/* Finds the open record of the same terminal and process. */
	result = EACCES;
	for (index = 0; valid && index < SSH_CHANNEL_MAX; index++) {
		if (!monitor->records[index].used || monitor->records[index].pid != pid)
			continue;
		same = strcmp(monitor->records[index].tty, tty);
		if (same != 0)
			continue;

		/* Writes the logout and forgets the record, written or not, so it is closed once. */
		status = sshd_login_record(monitor->user, tty, pid, 0);
		result = 0;
		if (status != 0)
			result = EIO;
		monitor->records[index].used = 0;
		break;
	}

	/* Logs a refusal. */
	if (result == EACCES) {
		/* Logs the refusal with the terminal as the log may show it. */
		monitor_log_text(tty, shown, sizeof(shown));
		syslog(LOG_AUTH | LOG_NOTICE, "monitor: logout record of %.17s refused: no such record", shown);
	}

	/* Answers with the outcome. */
	status = monitor_put_result(result, reply);
	if (status != 0)
		return EIO;

	/* Succeeded: the answer is the reply. */
	return 0;
}

/*
 * Reads a LOGIN_RECORD or LOGOUT_RECORD body: string tty, uint32 pid,
 * nothing after.  A tty that does not fit tty (SSHD_TTY_MAX with its
 * NUL), holds a NUL, or a pid that is not a positive process id reads
 * with *valid 0 (refused, not malformed).  Returns 0 or EPROTO.
 */
static int
monitor_read_record(
	const uint8_t *body,
	size_t size,
	char *tty,
	pid_t *pid,
	int *valid)
{
	struct ssh_reader reader;
	const uint8_t *path;
	const void *nul;
	size_t path_size;
	uint32_t number;
	int finished;

	/* Reads the two fields, with nothing after them. */
	ssh_reader_init(&reader, body, size);
	ssh_reader_get_string(&reader, &path, &path_size);
	number = ssh_reader_get_u32(&reader);
	finished = ssh_reader_finish(&reader);
	if (finished != 0)
		return EPROTO;

	/* Takes the path as text when it can be one of a terminal. */
	*valid = 1;
	tty[0] = '\0';
	nul = NULL;
	if (path_size > 0)
		nul = memchr(path, 0, path_size);
	if (path_size == 0 ||
	    path_size >= SSHD_TTY_MAX ||
	    nul != NULL) {
		*valid = 0;
	} else {
		memcpy(tty, path, path_size);
		tty[path_size] = '\0';
	}

	/* Takes the pid when it is one: positive and within a signed 32-bit id. */
	*pid = 0;
	if (number == 0 || number > 0x7fffffffUL)
		*valid = 0;
	else
		*pid = (pid_t)number;

	/* Succeeded: the request is read. */
	return 0;
}

/*
 * Checks that an authentication request may be taken now: after the
 * session identifier exists, and before the attempts (or, for a query,
 * the granted queries) reach MaxAuthTries.  Returns 0 or EPROTO.
 */
static int
monitor_begin_attempt(
	struct sshd_monitor *monitor,
	int query)
{
	/* Refuses an authentication before any key exchange. */
	if (!monitor->have_session_id)
		return EPROTO;

	/* Refuses an attempt after the last one allowed. */
	if (monitor->tries >= monitor->config->max_auth_tries)
		return EPROTO;

	/* Refuses one more query for a key that is already known to be accepted. */
	if (query && monitor->granted_queries >= monitor->config->max_auth_tries)
		return EPROTO;

	/* Succeeded: the attempt may be made. */
	return 0;
}

/*
 * Takes the user of the first authentication request, and checks that
 * every later one names the same.  Returns 0 or EPROTO.
 */
static int
monitor_take_user(
	struct sshd_monitor *monitor,
	const char *user)
{
	size_t length;
	int same;

	/* Keeps the first user. */
	if (!monitor->have_user) {
		length = strlen(user);
		memcpy(monitor->user, user, length + 1U);
		monitor->have_user = 1;
		return 0;
	}

	/* Refuses another. */
	same = strcmp(monitor->user, user);
	if (same != 0)
		return EPROTO;

	/* Succeeded: the request is for the user being authenticated. */
	return 0;
}

/*
 * Reads the signed data of a publickey login and checks every field
 * against what the monitor knows (RFC 4252 7): its session identifier,
 * the message number, the user being authenticated, "ssh-connection",
 * "publickey", TRUE, "ssh-ed25519", the key blob of the request, and
 * nothing after.  Returns 0 or EACCES.
 */
static int
monitor_signed_data_ok(
	const struct sshd_monitor *monitor,
	const uint8_t *data,
	size_t size,
	const uint8_t *blob,
	size_t blob_size)
{
	struct ssh_reader reader;
	const uint8_t *field;
	size_t field_size;
	uint8_t number;
	int flag;
	int same;
	int finished;

	/* The session identifier is the one the monitor signed first. */
	ssh_reader_init(&reader, data, size);
	ssh_reader_get_string(&reader, &field, &field_size);
	if (field_size != SSH_SHA256_SIZE)
		return EACCES;
	same = ssh_ct_equal(field, monitor->session_id, SSH_SHA256_SIZE);
	if (!same)
		return EACCES;

	/* The message is a user authentication request. */
	number = ssh_reader_get_u8(&reader);
	if (number != SSH_MSG_USERAUTH_REQUEST)
		return EACCES;

	/* For the user being authenticated. */
	ssh_reader_get_string(&reader, &field, &field_size);
	same = monitor_string_is(field, field_size, monitor->user);
	if (!same)
		return EACCES;

	/* For the connection service. */
	ssh_reader_get_string(&reader, &field, &field_size);
	same = monitor_string_is(field, field_size, SSH_SERVICE_CONNECTION);
	if (!same)
		return EACCES;

	/* By the publickey method, with a signature. */
	ssh_reader_get_string(&reader, &field, &field_size);
	same = monitor_string_is(field, field_size, MONITOR_METHOD_PUBLICKEY);
	if (!same)
		return EACCES;
	flag = ssh_reader_get_bool(&reader);
	if (!flag)
		return EACCES;

	/* With the one algorithm and the key of the request. */
	ssh_reader_get_string(&reader, &field, &field_size);
	same = monitor_string_is(field, field_size, SSH_KEY_NAME);
	if (!same)
		return EACCES;
	ssh_reader_get_string(&reader, &field, &field_size);
	if (field_size != blob_size || field_size == 0)
		return EACCES;
	same = memcmp(field, blob, blob_size);
	if (same != 0)
		return EACCES;

	/* And nothing after, nor any field that ran past the end. */
	finished = ssh_reader_finish(&reader);
	if (finished != 0)
		return EACCES;

	/* Succeeded: the data is this session's request for this user and key. */
	return 0;
}

/* Reports whether size bytes at data are exactly the text (1 or 0). */
static int
monitor_string_is(
	const uint8_t *data,
	size_t size,
	const char *text)
{
	size_t length;
	int same;

	/* Compares the length, then the bytes. */
	length = strlen(text);
	if (data == NULL || size != length)
		return 0;
	same = memcmp(data, text, length);
	if (same != 0)
		return 0;

	/* The bytes are the text. */
	return 1;
}

/*
 * Checks that the user may log in with a key and that public_key is in
 * the account's authorized_keys file; *uid receives the account's uid.
 * Returns 0 or EACCES.
 */
static int
monitor_key_allowed(
	const struct sshd_monitor *monitor,
	const uint8_t *public_key,
	uid_t *uid)
{
	struct passwd account;
	char buffer[SSHD_ACCOUNT_BUFFER];
	int allowed;
	int authorized;

	/* Refuses every key when publickey authentication is off. */
	*uid = 0;
	if (!monitor->config->pubkey)
		return EACCES;

	/* Checks the account. */
	allowed = sshd_account_allowed(monitor->config, monitor->user, SSH_AUTH_PUBLICKEY, &account, buffer, sizeof(buffer));
	if (allowed != 0)
		return EACCES;

	/* Checks the key against the account's file. */
	authorized = sshd_key_authorized(monitor->config, &account, public_key);
	if (authorized != 0)
		return EACCES;

	/* Succeeded: the key may log in to the account. */
	*uid = account.pw_uid;
	return 0;
}

#if defined(__ZEDBSD__)

/*
 * Checks with the system's process list (/dev/system) that pid is a
 * process of uid whose parent is the session process.  Returns 0 or
 * EACCES.
 */
static int
monitor_pid_is_child(
	pid_t pid,
	pid_t parent,
	uid_t uid)
{
	struct process_info info;
	int descriptor;
	int asked;

	/* Refuses before a session process exists. */
	if (pid <= 0 || parent <= 0)
		return EACCES;

	/* Asks for the first process after pid - 1, which is pid when it exists. */
	descriptor = open("/dev/system", O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return EACCES;
	memset(&info, 0, sizeof(info));
	info.pid = (int32_t)(pid - 1);
	asked = ioctl(descriptor, KERN_SYSTEM_GET_PROCESS, &info);
	(void)close(descriptor);
	if (asked != 0)
		return EACCES;

	/* Refuses an answer of another layout, and another process. */
	if (info.version != KERN_SYSTEM_PROCESS_INFO_VERSION || info.struct_size != sizeof(info))
		return EACCES;
	if (info.pid != (int32_t)pid)
		return EACCES;

	/* Refuses a process that is not the session's child, or not the user's. */
	if (info.ppid != (int32_t)parent)
		return EACCES;
	if (info.uid != (uint32_t)uid)
		return EACCES;

	/* Succeeded: pid is the session's child. */
	return 0;
}

#else

/*
 * Refuses: without zedBSD's process list there is no way to learn a
 * process's parent, so no record is written that could not be checked.
 */
static int
monitor_pid_is_child(
	pid_t pid,
	pid_t parent,
	uid_t uid)
{
	/* Nothing of the request can be checked here. */
	(void)pid;
	(void)parent;
	(void)uid;

	/* Reports the refusal. */
	return EACCES;
}

#endif

/*
 * Moves the connection to the GRANTED stage for the user with uid: only
 * AUTH_DONE (and a SIGN) may follow.
 */
static void
monitor_grant(
	struct sshd_monitor *monitor,
	uid_t uid)
{
	/* Remembers whom the session belongs to. */
	monitor->user_uid = uid;
	monitor->stage = SSHD_STAGE_GRANTED;
}

/* Appends a RESULT frame with result to reply; 0 or the frame's error. */
static int
monitor_put_result(
	uint32_t result,
	struct ssh_buf *reply)
{
	uint8_t body[MONITOR_RESULT_SIZE];
	int framed;

	/* Writes the uint32 and frames it. */
	ssh_store_be32(body, result);
	framed = ssh_mon_put_frame(SSH_MON_RESULT, body, sizeof(body), reply);
	if (framed != 0)
		return framed;

	/* Succeeded: the frame is in reply. */
	return 0;
}

/* Adds ms milliseconds to time, keeping tv_nsec below a second. */
static void
monitor_time_add_ms(
	struct timespec *time,
	unsigned ms)
{
	long nanoseconds;

	/* Adds the whole seconds, then the rest as nanoseconds, carrying one second at most. */
	time->tv_sec += (time_t)(ms / 1000U);
	nanoseconds = time->tv_nsec + (long)(ms % 1000U) * MONITOR_NS_PER_MS;
	if (nanoseconds >= MONITOR_NS_PER_SECOND) {
		nanoseconds -= MONITOR_NS_PER_SECOND;
		time->tv_sec++;
	}

	/* Stores the nanoseconds. */
	time->tv_nsec = nanoseconds;
}

/* Reports whether left is earlier than right (1 or 0). */
static int
monitor_time_before(
	const struct timespec *left,
	const struct timespec *right)
{
	/* Compares the seconds, then the nanoseconds. */
	if (left->tv_sec != right->tv_sec) {
		if (left->tv_sec < right->tv_sec)
			return 1;
		return 0;
	}

	/* Compares the nanoseconds when the seconds are equal. */
	if (left->tv_nsec < right->tv_nsec)
		return 1;

	/* left is not earlier. */
	return 0;
}

/*
 * Sleeps until ms milliseconds after start (CLOCK_MONOTONIC); returns at
 * once when that time has passed.
 */
static void
monitor_wait_until(
	const struct timespec *start,
	unsigned ms)
{
	struct timespec target;
	struct timespec now;
	struct timespec rest;
	int before;

	/* Finds when the wait ends. */
	target = *start;
	monitor_time_add_ms(&target, ms);

	/* Sleeps in steps, since a signal can cut a sleep short. */
	for (;;) {
		(void)clock_gettime(CLOCK_MONOTONIC, &now);
		before = monitor_time_before(&now, &target);
		if (!before)
			break;
		rest.tv_sec = target.tv_sec - now.tv_sec;
		rest.tv_nsec = target.tv_nsec - now.tv_nsec;
		if (rest.tv_nsec < 0) {
			rest.tv_nsec += MONITOR_NS_PER_SECOND;
			rest.tv_sec--;
		}

		/* Sleeps for the rest of the time. */
		(void)nanosleep(&rest, NULL);
	}
}

/*
 * Says how text is written as a word of sshd_config: plainly when the
 * parser takes it back as it is, in double quotes when it holds a blank
 * or starts with a byte the parser would read otherwise ('#', '=', '"'),
 * MONITOR_WORD_NONE when neither works (empty, a control byte, or a
 * double quote together with a reason to quote).
 */
static int
monitor_word_kind(
	const char *text)
{
	size_t index;
	int blank;
	int quote;
	unsigned char byte;

	/* An empty word cannot be written. */
	if (text[0] == '\0')
		return MONITOR_WORD_NONE;

	/* Looks for blanks, double quotes, and bytes no line may hold. */
	blank = 0;
	quote = 0;
	for (index = 0; text[index] != '\0'; index++) {
		byte = (unsigned char)text[index];
		if (byte == ' ' || byte == '\t' || byte == '\v' || byte == '\f')
			blank = 1;
		else if (byte == '"')
			quote = 1;
		else if (byte < 0x20 || byte == 0x7f)
			return MONITOR_WORD_NONE;
	}

	/* A word the parser would split, read as a comment or eat as the separator is quoted. */
	if (blank || text[0] == '#' || text[0] == '=' || text[0] == '"') {
		if (quote)
			return MONITOR_WORD_NONE;
		return MONITOR_WORD_QUOTED;
	}

	/* Any other word is taken back as it is. */
	return MONITOR_WORD_PLAIN;
}

/*
 * Appends the line "keyword first[ second]" to out, each word written as
 * monitor_word_kind says.  Returns 0 or EINVAL (a word that cannot be
 * written; out unchanged).
 */
static int
monitor_put_line(
	struct ssh_buf *out,
	const char *keyword,
	const char *first,
	const char *second)
{
	const char *words[2];
	unsigned count;
	unsigned index;
	int kind;

	/* Collects the words. */
	words[0] = first;
	words[1] = second;
	count = 1;
	if (second != NULL)
		count = 2;

	/* Refuses a word the text cannot carry before anything is written. */
	for (index = 0; index < count; index++) {
		kind = monitor_word_kind(words[index]);
		if (kind == MONITOR_WORD_NONE)
			return EINVAL;
	}

	/* Writes the keyword, then each word behind a space. */
	ssh_buf_put_bytes(out, keyword, strlen(keyword));
	for (index = 0; index < count; index++) {
		kind = monitor_word_kind(words[index]);
		ssh_buf_put_u8(out, ' ');
		if (kind == MONITOR_WORD_QUOTED)
			ssh_buf_put_u8(out, '"');
		ssh_buf_put_bytes(out, words[index], strlen(words[index]));
		if (kind == MONITOR_WORD_QUOTED)
			ssh_buf_put_u8(out, '"');
	}

	/* Ends the line. */
	ssh_buf_put_u8(out, '\n');

	/* Succeeded: the line is appended (a failure of out is recorded in it). */
	return 0;
}

/* Appends the line "keyword <decimal value>" to out; 0 or EINVAL. */
static int
monitor_put_number_line(
	struct ssh_buf *out,
	const char *keyword,
	unsigned long value)
{
	char text[MONITOR_DECIMAL_MAX];
	int written;

	/* Writes the number in decimal, then the line. */
	monitor_decimal(value, text);
	written = monitor_put_line(out, keyword, text, NULL);
	if (written != 0)
		return written;

	/* Succeeded: the line is appended. */
	return 0;
}

/* Writes value in decimal into text (MONITOR_DECIMAL_MAX bytes with the NUL). */
static void
monitor_decimal(
	unsigned long value,
	char *text)
{
	char reversed[MONITOR_DECIMAL_MAX];
	size_t count;
	size_t index;

	/* Takes the digits from the lowest, at least one. */
	count = 0;
	do {
		reversed[count++] = (char)('0' + (int)(value % 10UL));
		value /= 10UL;
	} while (value != 0 && count < sizeof(reversed) - 1U);

	/* Puts them in reading order and ends the text. */
	for (index = 0; index < count; index++)
		text[index] = reversed[count - 1U - index];
	text[count] = '\0';
}

/* Reports whether two sets of sshd settings are the same in every field (1 or 0). */
static int
monitor_config_equal(
	const struct sshd_config *left,
	const struct sshd_config *right)
{
	unsigned index;
	int same;

	/* Compares the numbers and switches. */
	if (left->port != right->port || left->inet6 != right->inet6)
		return 0;
	if (left->host_key_count != right->host_key_count || left->permit_root != right->permit_root)
		return 0;
	if (left->password != right->password || left->pubkey != right->pubkey)
		return 0;
	if (left->permit_empty != right->permit_empty || left->strict_modes != right->strict_modes)
		return 0;
	if (left->max_auth_tries != right->max_auth_tries || left->login_grace_time != right->login_grace_time)
		return 0;
	if (left->max_sessions != right->max_sessions || left->max_startups != right->max_startups)
		return 0;
	if (left->client_alive_interval != right->client_alive_interval || left->client_alive_count != right->client_alive_count)
		return 0;

	/* Compares the texts. */
	same = strcmp(left->listen_address, right->listen_address);
	if (same != 0)
		return 0;
	same = strcmp(left->authorized_keys_file, right->authorized_keys_file);
	if (same != 0)
		return 0;
	same = strcmp(left->sftp_command, right->sftp_command);
	if (same != 0)
		return 0;
	for (index = 0; index < left->host_key_count && index < SSH_CONFIG_KEYS_MAX; index++) {
		same = strcmp(left->host_key[index], right->host_key[index]);
		if (same != 0)
			return 0;
	}

	/* Every field is the same. */
	return 1;
}

/*
 * Builds the body of CONFIG: the settings as text, the host key blob,
 * the peer and whether sshd runs as root.  The blob is made from the
 * host key itself and must be the one the context names.  Returns 0 or
 * EINVAL.
 */
static int
monitor_config_body(
	const struct sshd_context *context,
	const struct ssh_key *host_key,
	struct ssh_buf *body)
{
	struct ssh_buf text;
	struct ssh_buf blob;
	const void *end;
	int written;
	int same;

	/* Refuses a peer that is not a terminated text. */
	end = memchr(context->peer, 0, sizeof(context->peer));
	if (end == NULL)
		return EINVAL;

	/* Makes the public blob of the host key and checks it against the context's. */
	ssh_buf_init(&blob, SSH_KEY_BLOB_SIZE + 8U, 0);
	ssh_key_put_public_blob(host_key->public_key, &blob);
	written = ssh_buf_error(&blob);
	if (written != 0 || blob.size != SSH_KEY_BLOB_SIZE) {
		ssh_buf_free(&blob);
		return EINVAL;
	}

	/* Compares the key with the host key of the monitor. */
	same = memcmp(blob.data, context->host_key_blob, SSH_KEY_BLOB_SIZE);
	if (same != 0) {
		ssh_buf_free(&blob);
		return EINVAL;
	}

	/* Writes the settings as text a child reads back into the same settings. */
	ssh_buf_init(&text, SSH_CONFIG_FILE_MAX, 0);
	written = sshd_config_put_text(&context->config, &text);
	if (written != 0) {
		ssh_buf_free(&text);
		ssh_buf_free(&blob);
		return EINVAL;
	}

	/* Puts the four fields together. */
	ssh_buf_put_string(body, text.data, text.size);
	ssh_buf_put_string(body, blob.data, blob.size);
	ssh_buf_put_cstring(body, context->peer);
	ssh_buf_put_u32(body, context->privileged ? 1U : 0U);
	ssh_buf_free(&text);
	ssh_buf_free(&blob);
	written = ssh_buf_error(body);
	if (written != 0)
		return EINVAL;

	/* Succeeded: body is CONFIG's. */
	return 0;
}

/*
 * Starts a child: a socketpair, then fork, and in the child
 * monitor_exec_child.  *child and *socket_end receive the child and the
 * monitor's end of the pair.  Returns 0 or EIO.
 */
static int
monitor_start_child(
	const char *program,
	const char *mode,
	int client,
	pid_t *child,
	int *socket_end)
{
	int pair[2];
	int made;
	pid_t forked;

	/* Makes the pair; neither end reaches a program by accident. */
	made = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
	if (made != 0)
		return EIO;
	(void)fcntl(pair[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(pair[1], F_SETFD, FD_CLOEXEC);

	/* Starts the child. */
	forked = fork();
	if (forked < 0) {
		(void)close(pair[0]);
		(void)close(pair[1]);
		return EIO;
	}

	/* Runs the program in the child. */
	if (forked == 0) {
		(void)close(pair[0]);
		monitor_exec_child(program, mode, client, pair[1]);
	}

	/* Keeps the monitor's end. */
	(void)close(pair[1]);
	*child = forked;
	*socket_end = pair[0];

	/* Succeeded: the child runs. */
	return 0;
}

/*
 * Runs in a new child: clears the signal mask and SIGPIPE's disposition,
 * puts the connection at SSHD_FD_CLIENT and the socket at
 * SSHD_FD_MONITOR (parking both above them first, so neither move
 * overwrites the other), closes every other descriptor above them, and
 * starts program with mode and an empty environment.  Never returns.
 */
static void
monitor_exec_child(
	const char *program,
	const char *mode,
	int client,
	int socket_end)
{
	char *arguments[3];
	char *environment[1];
	sigset_t none;
	int parked_client;
	int parked_socket;
	int moved;

	/* Starts the program with no signal blocked and SIGPIPE as a new program expects it. */
	(void)sigemptyset(&none);
	(void)sigprocmask(SIG_SETMASK, &none, NULL);
	(void)signal(SIGPIPE, SIG_DFL);

	/* Parks the two descriptors above the numbers they move to. */
	parked_client = fcntl(client, F_DUPFD, MONITOR_PARK_DESCRIPTOR);
	if (parked_client < 0)
		_exit(127);
	parked_socket = fcntl(socket_end, F_DUPFD, MONITOR_PARK_DESCRIPTOR);
	if (parked_socket < 0)
		_exit(127);

	/* Moves them to their numbers, which the program keeps (dup2 clears close-on-exec). */
	moved = dup2(parked_client, SSHD_FD_CLIENT);
	if (moved != SSHD_FD_CLIENT)
		_exit(127);
	moved = dup2(parked_socket, SSHD_FD_MONITOR);
	if (moved != SSHD_FD_MONITOR)
		_exit(127);

	/* Closes everything else above them: nothing of the monitor's reaches the child. */
	(void)closefrom(SSHD_FD_MONITOR + 1);

	/* Starts the program. */
	arguments[0] = (char *)program;
	arguments[1] = (char *)mode;
	arguments[2] = NULL;
	environment[0] = NULL;
	(void)execve(program, arguments, environment);
	_exit(127);
}

/* Sends one frame of type with body on descriptor; 0 or EIO. */
static int
monitor_send_frame(
	int descriptor,
	enum ssh_mon_type type,
	const struct ssh_buf *body)
{
	struct ssh_buf frame;
	int framed;
	int written;

	/* Builds the frame in a secret buffer: SESSION_START carries the session keys. */
	ssh_buf_init(&frame, 4U + (size_t)SSH_MON_STATE_MAX, 1);
	framed = ssh_mon_put_frame(type, body->data, body->size, &frame);
	if (framed != 0) {
		ssh_buf_free(&frame);
		return EIO;
	}

	/* Sends it whole. */
	written = monitor_write_all(descriptor, frame.data, frame.size);
	ssh_buf_free(&frame);
	if (written != 0)
		return EIO;

	/* Succeeded: the frame is sent. */
	return 0;
}

/* Writes all size bytes to descriptor, retrying after a signal; 0 or EIO. */
static int
monitor_write_all(
	int descriptor,
	const uint8_t *data,
	size_t size)
{
	size_t done;
	ssize_t written;

	/* Writes until every byte has gone. */
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

/*
 * Answers the child on socket_end, one frame at a time, until AUTH_DONE
 * is taken (0), the child closes the socket (EPIPE), deadline passes
 * (ETIMEDOUT; NULL is no deadline), the socket fails (EIO), or a request
 * ends the connection (its error).  When a login is granted, the
 * listener is told through *started.
 */
static int
monitor_serve(
	struct sshd_monitor *monitor,
	int socket_end,
	const struct timespec *deadline,
	int *started)
{
	struct ssh_buf input;
	struct ssh_buf body;
	struct ssh_buf reply;
	struct timespec arrived;
	struct pollfd waiting;
	uint8_t chunk[MONITOR_READ_CHUNK];
	enum ssh_mon_type type;
	enum sshd_monitor_stage before;
	enum sshd_monitor_next next;
	ssize_t got;
	int taken;
	int handled;
	int timeout;
	int ready;
	int status;

	/* The frames may carry a password or the session keys: every buffer is secret. */
	ssh_buf_init(&input, MONITOR_INPUT_MAX, 1);
	ssh_buf_init(&body, SSH_MON_STATE_MAX, 1);
	ssh_buf_init(&reply, MONITOR_REPLY_MAX, 0);

	/* Takes each whole frame, reading more when none is whole. */
	for (;;) {
		taken = ssh_mon_next_frame(&input, &type, &body);
		if (taken == 0) {
			/* Handles the request and notices a grant it made. */
			(void)clock_gettime(CLOCK_MONOTONIC, &arrived);
			before = monitor->stage;
			ssh_buf_reset(&reply);
			handled = sshd_monitor_handle(monitor, type, body.data, body.size, &arrived, &reply, &next);
			ssh_buf_reset(&body);
			if (handled != 0) {
				status = handled;
				break;
			}

			/* Reports the grant when this message moved the stage from the pre-authentication to granted. */
			if (before == SSHD_STAGE_PREAUTH && monitor->stage == SSHD_STAGE_GRANTED)
				monitor_report_grant(started);

			/* AUTH_DONE has no reply: the pre-authentication process is done. */
			if (next == SSHD_NEXT_START_SESSION) {
				status = 0;
				break;
			}

			/* Sends the reply. */
			status = monitor_write_all(socket_end, reply.data, reply.size);
			if (status != 0)
				break;
			continue;
		}

		/* Refuses a frame the parser refused. */
		if (taken != EAGAIN) {
			status = EPROTO;
			break;
		}

		/* Waits for bytes, no longer than the deadline. */
		timeout = monitor_poll_ms(deadline);
		if (timeout == 0) {
			status = ETIMEDOUT;
			break;
		}

		/* Waits for the socket to be readable within the timeout. */
		waiting.fd = socket_end;
		waiting.events = POLLIN;
		waiting.revents = 0;
		ready = poll(&waiting, 1, timeout);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0) {
			status = EIO;
			break;
		}

		/* Keeps waiting when no input arrived. */
		if (ready == 0)
			continue;

		/* Takes what came; nothing at all is the child closing its end. */
		got = read(socket_end, chunk, sizeof(chunk));
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			status = EIO;
			break;
		}

		/* Treats the end of the input as a closed peer. */
		if (got == 0) {
			status = EPIPE;
			break;
		}

		/* Appends the bytes that were read to the input. */
		ssh_buf_put_bytes(&input, chunk, (size_t)got);
		status = ssh_buf_error(&input);
		if (status != 0) {
			status = EPROTO;
			break;
		}
	}

	/* Wipes what was read. */
	ssh_wipe(chunk, sizeof(chunk));
	ssh_buf_free(&input);
	ssh_buf_free(&body);
	ssh_buf_free(&reply);

	/* Reports how the serving ended. */
	if (status != 0)
		return status;

	/* Succeeded: the transport was handed over. */
	return 0;
}

/*
 * Returns how long a poll may wait for deadline: -1 without one, 0 when
 * it has passed, otherwise the milliseconds left (at least 1, at most
 * MONITOR_POLL_MAX_MS).
 */
static int
monitor_poll_ms(
	const struct timespec *deadline)
{
	struct timespec now;
	long left;
	int before;

	/* Waits as long as it takes without a deadline. */
	if (deadline == NULL)
		return -1;

	/* Reports a deadline that has passed. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	before = monitor_time_before(&now, deadline);
	if (!before)
		return 0;

	/* Counts the milliseconds left, rounded up, within one poll's longest wait. */
	if (deadline->tv_sec - now.tv_sec >= (time_t)(MONITOR_POLL_MAX_MS / 1000L))
		return (int)MONITOR_POLL_MAX_MS;
	left = (long)(deadline->tv_sec - now.tv_sec) * 1000L + (deadline->tv_nsec - now.tv_nsec) / MONITOR_NS_PER_MS + 1L;
	if (left < 1L)
		left = 1L;

	/* Reports the wait. */
	return (int)left;
}

/*
 * Tells the listener that this connection is authenticated: one byte on
 * the MaxStartups pipe, which is then closed, so it is told once.
 */
static void
monitor_report_grant(
	int *started)
{
	static const uint8_t mark[1] = { 1 };

	/* Without a pipe there is no listener to tell. */
	if (*started < 0)
		return;

	/* Writes the byte; a listener that is gone does not matter. */
	(void)monitor_write_all(*started, mark, sizeof(mark));
	(void)close(*started);
	*started = -1;
}

/* Ends a child: closes the monitor's end of its socket, kills it and reaps it. */
static void
monitor_stop_child(
	pid_t child,
	int socket_end)
{
	/* Closes the socket so a living child sees the end. */
	if (socket_end >= 0)
		(void)close(socket_end);

	/* Kills and reaps it. */
	if (child > 0) {
		(void)kill(child, SIGKILL);
		monitor_reap(child);
	}
}

/* Reaps child, retrying after a signal. */
static void
monitor_reap(
	pid_t child)
{
	pid_t reaped;
	int status;

	/* Waits until the child is gone. */
	do {
		reaped = waitpid(child, &status, 0);
	} while (reaped < 0 && errno == EINTR);
}

/*
 * Copies text into out, of size bytes, for a line of the log: every byte
 * that is not printable ASCII (a control byte, DEL, or a byte of a
 * character outside ASCII) becomes '?', so that a name the peer chose cannot
 * end the line, start a forged one, or send a terminal that shows the log
 * a control sequence.  A longer text is cut to size - 1 bytes.
 */
static void
monitor_log_text(
	const char *text,
	char *out,
	size_t size)
{
	size_t index;
	unsigned char byte;

	/* Copies the text up to the room, replacing what is not printable. */
	for (index = 0;
	     index + 1U < size && text[index] != '\0';
	     index++) {
		byte = (unsigned char)text[index];
		if (byte < 0x20U || byte > 0x7eU)
			byte = '?';
		out[index] = (char)byte;
	}

	/* Ends the copy as a string. */
	out[index] = '\0';
}

/*
 * Writes " from <address> port <port>" for a log line from the peer
 * sshd_monitor_run_program kept ("address port"), or nothing when there
 * is no peer (the monitor run without a connection, as in a test).
 */
static void
monitor_log_from(
	char *out,
	size_t size)
{
	const char *space;
	size_t address_length;

	/* Nothing when no peer is known. */
	out[0] = '\0';
	space = strrchr(monitor_peer, ' ');
	if (space == NULL)
		return;

	/* The address before the last blank, the port after it. */
	address_length = (size_t)(space - monitor_peer);
	(void)snprintf(out, size, " from %.*s port %s", (int)address_length, monitor_peer, space + 1);
}
