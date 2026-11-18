/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of sshd's monitor, for the sshd program's own files and its
 * tests: the state the monitor keeps about one connection, the check of
 * one request from a child against that state, the configuration text the
 * monitor sends its children, and the account checks below the ones
 * sshd.h declares.
 *
 * The monitor's state.  The stage says which requests a child may make:
 * PREAUTH (the pre-authentication process before a login is granted:
 * SIGN, PUBKEY_QUERY, PUBKEY_LOGIN, PASSWORD_LOGIN), GRANTED (after a
 * RESULT 0 of a login: SIGN, then AUTH_DONE once), SESSION (the session
 * process: SIGN, LOGIN_RECORD, LOGOUT_RECORD).  A request of another type
 * ends the connection.  The user is the one the first authentication
 * request named; a request that names another ends the connection (a
 * pre-authentication process keeps the first user as well and
 * disconnects a client that changes it).  The session identifier is the
 * first h signed.  No authentication request is taken before it exists.
 *
 * Attempts.  Every PUBKEY_LOGIN and PASSWORD_LOGIN counts one attempt,
 * and so does a PUBKEY_QUERY that is refused; a query that is granted
 * counts against a separate bound of the same size.  The request after
 * MaxAuthTries attempts ends the connection, so a pre-authentication
 * process counts the same way and disconnects the client itself before
 * it asks again.
 *
 * Errors of sshd_monitor_handle: EPROTO is a request the child may not
 * make (of a type the stage does not allow, out of order, malformed, with
 * bytes after its last field, an h that is not 32 bytes, one SIGN too
 * many, another user, an attempt over the limit): the caller ends the
 * connection.  EIO and ENOMEM are the monitor's own failures, which end
 * it too.  A request that is well formed but not granted is answered
 * with a RESULT, never an error.
 */

#ifndef SSH_SSHD_MONITOR_H
#define SSH_SSHD_MONITOR_H

#include <pwd.h>
#include <sys/types.h>
#include <time.h>

#include "ssh-auth.h"
#include "ssh-channel.h"
#include "ssh-privsep.h"
#include "sshd.h"

/* The sshd program the monitor starts its children from, unless its caller names another. */
#define SSHD_PROGRAM		"/usr/sbin/sshd"

/* The directory of the pseudo-terminals a login record may name. */
#define SSHD_PTS_DIRECTORY	"/dev/pts/"

/* The most digits of a pseudo-terminal number in a login record. */
#define SSHD_PTS_DIGITS_MAX	8U

/* The longest tty path a login record may name, with its NUL. */
#define SSHD_TTY_MAX		(sizeof(SSHD_PTS_DIRECTORY) + SSHD_PTS_DIGITS_MAX)

/* The room for the strings of one passwd entry. */
#define SSHD_ACCOUNT_BUFFER	4096U

/* The longest authorized_keys path after its tokens are expanded, with its NUL. */
#define SSHD_KEYS_PATH_MAX	4096U

/* The longest time the child that reads an authorized_keys file may take, in milliseconds. */
#define SSHD_KEYS_TIMEOUT_MS	10000U

/* Which requests the children of a connection may make now. */
enum sshd_monitor_stage {
	SSHD_STAGE_PREAUTH = 0,
	SSHD_STAGE_GRANTED = 1,
	SSHD_STAGE_SESSION = 2
};

/* What the caller does after a request was handled. */
enum sshd_monitor_next {
	/* Sends the reply frame the handler appended. */
	SSHD_NEXT_REPLY = 0,

	/* Takes the exported transport (no reply), ends the pre-authentication process and starts the session process. */
	SSHD_NEXT_START_SESSION = 1
};

/*
 * One login record the session process had the monitor write: the tty
 * and the pid of its USER_PROCESS record, so that its DEAD_PROCESS record
 * is written once, by the session's request or when the session ends.
 */
struct sshd_login_entry {
	char tty[SSHD_TTY_MAX];
	pid_t pid;
	int used;
};

/*
 * What the monitor of one connection knows, against which it checks every
 * request of its children.
 *
 * config and host_key belong to the caller and outlive the monitor.
 * privileged is 1 when sshd runs as root.  fail_delay_ms and
 * sign_interval_ms are SSHD_FAIL_DELAY_MS and SSHD_SIGN_INTERVAL_MS
 * (sshd_monitor_init sets them; a test may shorten them for the checks
 * that are not about time).  session_id is valid when have_session_id.
 * preauth_signs counts the signatures before the session process.
 * last_sign is when the last signature was made (CLOCK_MONOTONIC), valid
 * when have_last_sign.  user is valid when have_user; user_uid is its uid
 * once a login is granted.  tries and granted_queries are the attempts
 * of the header comment.  session_pid is the session process the caller
 * started.  records are the login records written and not yet closed.
 * state holds the exported transport from AUTH_DONE until the caller
 * sends it on (secret).
 */
struct sshd_monitor {
	const struct sshd_config *config;
	const struct ssh_key *host_key;
	int privileged;
	enum sshd_monitor_stage stage;
	unsigned fail_delay_ms;
	unsigned sign_interval_ms;
	uint8_t session_id[SSH_SHA256_SIZE];
	int have_session_id;
	unsigned preauth_signs;
	struct timespec last_sign;
	int have_last_sign;
	char user[SSH_USER_MAX + 1];
	int have_user;
	uid_t user_uid;
	unsigned tries;
	unsigned granted_queries;
	pid_t session_pid;
	struct sshd_login_entry records[SSH_CHANNEL_MAX];
	struct ssh_buf state;
};

/*
 * Starts the state of a new connection's monitor in the PREAUTH stage.
 * Allocates nothing; sshd_monitor_clear ends it.
 */
void sshd_monitor_init(struct sshd_monitor *monitor, const struct sshd_config *config, const struct ssh_key *host_key, int privileged);

/*
 * Checks and carries out one request frame of a child: type and size
 * bytes of body as ssh_mon_next_frame took them, arrived the time it was
 * taken (CLOCK_MONOTONIC).  An authentication request is answered no
 * earlier than fail_delay_ms after arrived; a SIGN of the session stage
 * no earlier than sign_interval_ms after the signature before.  On 0,
 * *next says what to do, and for SSHD_NEXT_REPLY the reply frame is
 * appended to reply.  Returns 0, EPROTO, EIO or ENOMEM (the header
 * comment).
 */
int sshd_monitor_handle(struct sshd_monitor *monitor, enum ssh_mon_type type, const uint8_t *body, size_t size, const struct timespec *arrived, struct ssh_buf *reply, enum sshd_monitor_next *next);

/* Writes the DEAD_PROCESS record of every login record still open, as when the session process ends. */
void sshd_monitor_end_session(struct sshd_monitor *monitor);

/* Wipes the monitor's secrets (the exported transport) and clears it. */
void sshd_monitor_clear(struct sshd_monitor *monitor);

/*
 * Runs the monitor of one connection as sshd_monitor_run does, starting
 * the children from program (an absolute path, or one the working
 * directory resolves) instead of SSHD_PROGRAM.
 */
int sshd_monitor_run_program(const struct sshd_context *context, struct ssh_key *host_key, int client, int started, const char *program);

/*
 * Appends to out the sshd_config text the monitor sends its children in
 * CONFIG: one line for each setting of config, which sshd_config_parse
 * reads back into the same settings (checked here: a setting the text
 * cannot carry, such as a path with both a blank and a double quote, is
 * EINVAL).  Returns 0, EINVAL or the error recorded in out.
 */
int sshd_config_put_text(const struct sshd_config *config, struct ssh_buf *out);

/*
 * Decides whether an account may log in with method, given what passwd
 * and shadow hold: shadow_hash is the account's shadow hash (NULL when
 * there is no entry), privileged is 1 when sshd runs as root, self the
 * effective uid of sshd.  As root: an entry is needed and must not be
 * locked ("!" or "*" in front).  Without root: only the account whose
 * uid is self.  The shell must not be /sbin/nologin (or
 * /usr/sbin/nologin).  root only as PermitRootLogin allows the method.
 * Returns 0 or EACCES.
 */
int sshd_account_policy(const struct sshd_config *config, const struct passwd *account, const char *shadow_hash, int method, int privileged, uid_t self);

/*
 * Expands the AuthorizedKeysFile pattern for account into out (capacity
 * bytes with the NUL): %h is the home directory, %u the user name, %% a
 * '%'; a path that is not absolute after the expansion is taken relative
 * to the home directory.  Returns 0 or EINVAL (another token, a home
 * directory that is not absolute, or a result that does not fit).
 */
int sshd_authorized_path(const char *pattern, const struct passwd *account, char *out, size_t capacity);

/*
 * Checks that tty names a pseudo-terminal: "/dev/pts/" and 1 to
 * SSHD_PTS_DIGITS_MAX digits, nothing else.  Returns 0 or EINVAL.
 */
int sshd_tty_path_ok(const char *tty);

/*
 * Checks that tty names a pseudo-terminal (sshd_tty_path_ok) and that it
 * is a character device owned by uid (lstat, so a link is not followed).
 * Returns 0 or EACCES.
 */
int sshd_tty_check(const char *tty, uid_t uid);

#endif
