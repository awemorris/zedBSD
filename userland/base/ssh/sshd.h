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
 * key, listens, and forks one monitor per accepted connection.  The
 * monitor (sshd-monitor.c, root) of a connection makes a socketpair and
 * starts the pre-authentication process by fork and execve of the sshd
 * program itself with SSHD_EXEC_PREAUTH (a fresh address space: nothing
 * of the listener's or the monitor's memory, not the host key's seed, not
 * a shadow entry a lookup left behind, reaches it).  That process takes
 * its settings from the monitor (SSH_MON_CONFIG, ssh-privsep.h), makes one
 * call of each crypto operation it will need while it still can read the
 * file system, then changes its root to /var/empty, drops to the user
 * sshd (setgroups, setresgid, setresuid), and runs the transport and the
 * user authentication, asking the monitor for every privileged step.  It
 * sends USERAUTH_SUCCESS itself, writes all its pending output, then
 * exports the transport (AUTH_DONE) and exits.  The monitor then starts
 * the session process the same way with SSHD_EXEC_SESSION; it takes the
 * user's groups and ids before it reads anything, imports the transport
 * and runs the connection protocol: the channels, the pseudo-terminals it
 * opens itself (a terminal opened by the user's process belongs to the
 * user), the shells and commands, and the sftp subsystem.  The monitor
 * stays to sign re-exchanges and to write the login records, and ends
 * when the session process does.  The kernel refuses to let one process
 * trace another that changed its identity (src/kern/cred.c), so the
 * pre-authentication processes of two connections, both uid sshd, cannot
 * read each other.
 *
 * Descriptors.  Every descriptor sshd opens is close-on-exec, except the
 * two a child is started with (SSHD_FD_CLIENT and SSHD_FD_MONITOR, moved
 * there with dup2 just before execve) and, in the session process, the
 * pseudo-terminal or the pipes a shell or command is given.  A started
 * child calls closefrom(SSHD_FD_MONITOR + 1) first.  The monitor's socket
 * never reaches a shell or a command.
 *
 * MaxStartups.  The listener keeps, for every monitor, the read end of a
 * pipe whose write end the monitor holds; the monitor writes one byte to
 * it when it grants a login.  A monitor that has not written is a
 * connection not yet authenticated; a connection above MaxStartups of
 * those is closed at once after accept.  The listener reaps monitors on
 * SIGCHLD.
 *
 * The account checks (sshd-login.c) are done by the monitor: the account
 * exists in passwd and is not locked in shadow (a "!" or "*" in front of
 * the hash, as login does) and its shell is not /sbin/nologin; root only
 * as PermitRootLogin allows; the authorized_keys file is read by a child
 * the monitor forks with the user's ids (never by root: the path is the
 * user's to choose), opened with O_NONBLOCK and O_NOFOLLOW and checked
 * with fstat on the open descriptor (a regular file, owned by the user or
 * root, not writable by group or others, and the same for every directory
 * above it up to the home directory, when StrictModes); a password is
 * checked with the same rule as the console login (login_verify), and an
 * empty password is refused unless PermitEmptyPasswords.
 *
 * Timing.  Every answer to an authentication request (granted or not) is
 * sent no earlier than SSHD_FAIL_DELAY_MS after the request arrived, the
 * monitor waiting out the rest; a check that took longer is not answered
 * sooner.  For a user that does not exist or is locked, the password
 * check still runs crypt on a fixed dummy hash of the same kind, so the
 * work done does not depend on whether the account exists.  A connection
 * gets MaxAuthTries attempts and LoginGraceTime seconds in all.
 *
 * Without root (a test on a development host): the same three processes
 * and the same socketpair and messages, but no chroot and no change of
 * ids, only the user sshd runs as is served, and password authentication
 * is off.  The host key is the one named by the configuration.  This mode
 * exists so that the programs and the monitor protocol can be run against
 * OpenSSH on a host without privileges; the image runs sshd as root.
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

/* The first argument that starts sshd as one of the monitor's children. */
#define SSHD_EXEC_PREAUTH	"-Zpreauth"
#define SSHD_EXEC_SESSION	"-Zsession"

/* The descriptor numbers a started child finds its connection and its monitor socket at. */
#define SSHD_FD_CLIENT		3
#define SSHD_FD_MONITOR		4

/* The least time from an authentication request to its answer, in milliseconds. */
#define SSHD_FAIL_DELAY_MS	1000U

/*
 * The least time between two signatures the monitor makes for one
 * session process (re-exchanges), in milliseconds; a request that comes
 * sooner waits.  A count per hour would cut off a long fast transfer
 * (both sides re-key every gigabyte), and a signature of an exchange hash
 * is no more useful to a thief the second time, so only the rate is
 * bounded, to keep a session from spending the monitor's time.
 */
#define SSHD_SIGN_INTERVAL_MS	1000U

/* The most signatures for one pre-authentication process (the initial exchange and a few re-exchanges during login). */
#define SSHD_PREAUTH_SIGN_MAX	4U

/*
 * What the monitor knows about its connection and hands, without
 * secrets, to its children (the settings travel in SSH_MON_CONFIG).
 *
 * host_key_blob is the public key blob (ssh-key.h); the seed is not here:
 * the monitor keeps the private key in its own struct ssh_key.  peer is
 * the address and port for the log.  privileged is 1 when sshd runs as
 * root.
 */
struct sshd_context {
	struct sshd_config config;
	uint8_t host_key_blob[SSH_KEY_BLOB_SIZE];
	char peer[64];
	int privileged;
};

/*
 * Runs the monitor of one connection on descriptor client until the
 * connection ends.  host_key is the private host key (wiped by the
 * monitor when it ends); started is the write end of the listener's
 * MaxStartups pipe.  Returns the exit status of the monitor process.
 */
int sshd_monitor_run(const struct sshd_context *context, struct ssh_key *host_key, int client, int started);

/*
 * Runs the pre-authentication process (sshd started with
 * SSHD_EXEC_PREAUTH): reads the settings from the monitor, warms the
 * crypto up, confines itself, then runs the transport and the user
 * authentication.  Never returns (the process exits: 0 after AUTH_DONE, 1
 * otherwise).
 */
void sshd_preauth_run(void);

/*
 * Runs the session process (sshd started with SSHD_EXEC_SESSION): reads
 * the settings, the user and the exported transport from the monitor,
 * takes the user's ids, imports the transport and runs the connection
 * protocol.  Never returns.
 */
void sshd_session_run(void);

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
 * (AuthorizedKeysFile, expanded): a child with the account's ids reads the
 * file as described above and passes at most SSH_HOSTFILE_MAX bytes back
 * over a pipe, and the monitor looks for the key with
 * ssh_authorized_keys_find.  Returns 0 (authorized), EACCES (not listed,
 * or the file or a directory is unsafe; the reason goes to the log), or
 * EIO.
 */
int sshd_key_authorized(const struct sshd_config *config, const struct passwd *account, const uint8_t *public_key);

/*
 * Checks a password with the console login's rule (login_verify),
 * refusing an empty one unless PermitEmptyPasswords; for an account that
 * does not exist or is locked, crypt still runs on a dummy hash.  The
 * password is wiped whatever the answer.  Returns 0 or EACCES.
 */
int sshd_password_ok(const struct sshd_config *config, const char *user, char *password);

/*
 * Writes the login record (login 1: USER_PROCESS) or the logout record
 * (login 0: DEAD_PROCESS) of a session on tty (a /dev/pts path) for pid,
 * with pututxline as login does.  Returns 0 or EIO.
 */
int sshd_login_record(const char *user, const char *tty, pid_t pid, int login);

#endif
