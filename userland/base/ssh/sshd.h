/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The processes of sshd and what they hand each other.
 *
 * The listener (sshd-main.c, root) reads the configuration and the host
 * key, listens, and forks one monitor per accepted connection, refusing
 * connections above MaxStartups not yet authenticated.  The monitor
 * (sshd-monitor.c, root) of a connection makes a socketpair and forks the
 * pre-authentication process (sshd-preauth.c), which changes its root to
 * /var/empty, drops to the user sshd, and runs the transport and the
 * user authentication, asking the monitor for every privileged step
 * (ssh-privsep.h).  When the monitor has granted a login, the pre-
 * authentication process exports the transport and exits; the monitor
 * forks the session process (sshd-session.c), which takes the user's
 * groups and ids, imports the transport and runs the connection protocol:
 * the channels, the pseudo-terminals it opens itself (a terminal opened by
 * the user's process belongs to the user), the shells and commands, and
 * the sftp subsystem.  The monitor stays to sign re-exchanges and to write
 * the login records, and ends when the session process does.
 *
 * Without root (a test on a development host), the listener serves only
 * its own user, with no monitor process: one process per connection does
 * everything, password authentication is off, and the host key is the
 * one named by the configuration.  This mode exists so that the programs
 * can be run against OpenSSH on a host without privileges; the image runs
 * sshd as root.
 *
 * The account checks (sshd-login.c) are done by the monitor: the account
 * exists in passwd and is not locked in shadow (a "!" or "*" in front of
 * the hash, as login does) and its shell is not /sbin/nologin; root only
 * as PermitRootLogin allows; the authorized_keys file and the directories
 * above it are owned by the user or root and not writable by group or
 * others (StrictModes); a password is checked with the same rule as the
 * console login (userland/base/login/verify.c), and an empty password is
 * refused unless PermitEmptyPasswords.  Every failed attempt waits a
 * fixed time before the reply, and a connection gets MaxAuthTries
 * attempts and LoginGraceTime seconds in all.
 */

#ifndef SSH_SSHD_H
#define SSH_SSHD_H

#include <pwd.h>
#include <sys/types.h>

#include "ssh-config.h"
#include "ssh-key.h"

/* The directory the pre-authentication process changes its root to, and its user. */
#define SSHD_EMPTY_DIRECTORY	"/var/empty"
#define SSHD_PRIVSEP_USER	"sshd"

/* The time a failed authentication waits before its reply, in milliseconds. */
#define SSHD_FAIL_DELAY_MS	1000U

/* The most signatures the monitor makes for one session process in an hour (re-exchanges). */
#define SSHD_SIGN_PER_HOUR	64U

/*
 * What the listener hands every monitor: the configuration and the host
 * key (with its seed: the monitor signs), and the peer's address for the
 * log.  The monitor's copy is wiped when it ends.
 */
struct sshd_context {
	struct sshd_config config;
	struct ssh_key host_key;
	char peer[64];
	int privileged;
};

/*
 * Runs the monitor of one connection on descriptor client until the
 * connection ends.  Returns the exit status of the monitor process.
 */
int sshd_monitor_run(struct sshd_context *context, int client);

/*
 * Runs the pre-authentication process: the transport and the user
 * authentication on client, asking the monitor on monitor.  Never returns
 * (the process exits: 0 after AUTH_DONE, 1 otherwise).
 */
void sshd_preauth_run(const struct sshd_context *context, int client, int monitor);

/*
 * Runs the session process for the authenticated user: imports the
 * transport from size bytes at state, then the connection protocol on
 * client.  Never returns.
 */
void sshd_session_run(const struct sshd_context *context, int client, int monitor, const char *user, const uint8_t *state, size_t size);

/*
 * Checks that user may log in at all with method (SSH_AUTH_PUBLICKEY or
 * SSH_AUTH_PASSWORD of ssh-auth.h): the account exists (getpwnam_r into
 * account, its strings in buffer of size bytes), is not locked, has a
 * shell other than /sbin/nologin, and is root only as PermitRootLogin
 * allows for the method.  Returns 0, EACCES, or ENOENT (no such user; the
 * caller fails it like EACCES and spends the same time).
 */
int sshd_account_allowed(const struct sshd_config *config, const char *user, int method, struct passwd *account, char *buffer, size_t size);

/*
 * Reports whether public_key is in the account's authorized_keys file
 * (AuthorizedKeysFile, expanded), after the StrictModes checks of the file
 * and of every directory from it up to the home directory.  Returns 0
 * (authorized), EACCES (not listed, or the file or a directory is unsafe;
 * the reason goes to the log), or EIO.
 */
int sshd_key_authorized(const struct sshd_config *config, const struct passwd *account, const uint8_t *public_key);

/*
 * Checks a password with the console login's rule (login_verify of
 * userland/base/login/verify.h), refusing an empty one unless
 * PermitEmptyPasswords.  The password is wiped whatever the answer.
 * Returns 0 or EACCES.
 */
int sshd_password_ok(const struct sshd_config *config, const char *user, char *password);

/*
 * Writes the login record (login 1: USER_PROCESS) or the logout record
 * (login 0: DEAD_PROCESS) of a session on tty (a /dev/pts path) for pid,
 * with pututxline as login does.  Returns 0 or EIO.
 */
int sshd_login_record(const char *user, const char *tty, pid_t pid, int login);

#endif
