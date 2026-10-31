/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The configuration of sshd (/etc/ssh/sshd_config) and of ssh
 * (/etc/ssh/ssh_config, ~/.ssh/config and -o options), in OpenSSH's
 * syntax (sshd_config(5), ssh_config(5)) for the keywords these programs
 * act on.
 *
 * Syntax: one "Keyword value" per line (or "Keyword=value"), keywords
 * without regard to case, '#' starting a comment line, blank lines
 * ignored, a value in double quotes may hold spaces.  The first value
 * given for a keyword is the one used (both files, as OpenSSH; for sshd
 * the HostKey keyword adds to a list instead).
 *
 * Keywords these programs do not act on: a set of OpenSSH keywords whose
 * absence of effect is harmless is accepted and ignored (the lists are in
 * the .c file and in the design); any other keyword is an error with the
 * line number, as OpenSSH reports one, so that a setting that would not
 * take effect is noticed.  sshd's Match blocks are not supported (an
 * error).  ssh_config's Host blocks are: a line "Host <pattern list>"
 * starts a block whose lines apply only when the host the user named
 * matches (ssh_pattern_list_match, fold 1); "Match" in ssh_config is an
 * error.
 */

#ifndef SSH_SSH_CONFIG_H
#define SSH_SSH_CONFIG_H

#include "ssh-common.h"

/* The largest configuration file read, in bytes. */
#define SSH_CONFIG_FILE_MAX	(256U * 1024U)

/* The longest path held. */
#define SSH_CONFIG_PATH_MAX	1024U

/* The most HostKey and IdentityFile entries. */
#define SSH_CONFIG_KEYS_MAX	8U

/* The longest error text. */
#define SSH_CONFIG_ERROR_MAX	256U

/* The values of PermitRootLogin (sshd_config(5)). */
enum sshd_permit_root {
	SSHD_PERMIT_ROOT_NO = 0,
	SSHD_PERMIT_ROOT_YES = 1,
	SSHD_PERMIT_ROOT_PROHIBIT_PASSWORD = 2
};

/* The values of StrictHostKeyChecking (ssh_config(5)). */
enum ssh_strict_host {
	SSH_STRICT_ASK = 0,
	SSH_STRICT_YES = 1,
	SSH_STRICT_NO = 2,
	SSH_STRICT_ACCEPT_NEW = 3
};

/* The values of RequestTTY (ssh_config(5)). */
enum ssh_request_tty {
	SSH_TTY_AUTO = 0,
	SSH_TTY_NO = 1,
	SSH_TTY_YES = 2,
	SSH_TTY_FORCE = 3
};

/*
 * sshd's settings.
 *
 * Defaults (sshd_config_defaults): port 22, listen_address "0.0.0.0"
 * with address_family "inet" (zedBSD's sshd_config of today), host_key
 * "/etc/ssh/ssh_host_ed25519_key", permit_root PROHIBIT_PASSWORD
 * (OpenSSH's default; the image's file sets yes), password and pubkey 1,
 * permit_empty 0, max_auth_tries 6, login_grace_time 120 seconds,
 * max_sessions SSH_CHANNEL_MAX, max_startups 10 (connections not yet
 * authenticated), authorized_keys_file ".ssh/authorized_keys" (relative
 * to the home directory; %h and %u expand to the home and the user),
 * strict_modes 1, sftp_command "" (no sftp subsystem), client_alive
 * interval 0 (off) and count 3.
 */
struct sshd_config {
	uint16_t port;
	char listen_address[64];
	int inet6;
	char host_key[SSH_CONFIG_KEYS_MAX][SSH_CONFIG_PATH_MAX];
	unsigned host_key_count;
	enum sshd_permit_root permit_root;
	int password;
	int pubkey;
	int permit_empty;
	unsigned max_auth_tries;
	unsigned login_grace_time;
	unsigned max_sessions;
	unsigned max_startups;
	char authorized_keys_file[SSH_CONFIG_PATH_MAX];
	int strict_modes;
	char sftp_command[SSH_CONFIG_PATH_MAX];
	unsigned client_alive_interval;
	unsigned client_alive_count;
};

/*
 * ssh's settings for one destination.
 *
 * set_mask has one bit per field that a source has set (bit numbers in
 * the .c file), so that a later source does not override an earlier one.
 * Defaults (ssh_config_defaults, applied after every source): user ""
 * (the caller uses the login name), port 22, identity "~/.ssh/id_ed25519",
 * strict ASK, user_known_hosts "~/.ssh/known_hosts", global_known_hosts
 * "/etc/ssh/ssh_known_hosts", batch_mode 0, connect_timeout 0 (none),
 * server_alive_interval 0, server_alive_count 3, request_tty AUTO,
 * password and pubkey 1, log_level 1 (INFO; 0 QUIET, 2 VERBOSE, 3 DEBUG),
 * host_name "" (the name given).
 */
struct ssh_config {
	uint32_t set_mask;
	char user[SSH_USER_MAX + 1];
	char host_name[256];
	uint16_t port;
	char identity[SSH_CONFIG_KEYS_MAX][SSH_CONFIG_PATH_MAX];
	unsigned identity_count;
	enum ssh_strict_host strict;
	char user_known_hosts[SSH_CONFIG_PATH_MAX];
	char global_known_hosts[SSH_CONFIG_PATH_MAX];
	int batch_mode;
	unsigned connect_timeout;
	unsigned server_alive_interval;
	unsigned server_alive_count;
	enum ssh_request_tty request_tty;
	int password;
	int pubkey;
	int log_level;
};

/* Fills config with sshd's defaults (host_key_count 0: the default key is added by sshd_config_finish when no HostKey line gave one). */
void sshd_config_defaults(struct sshd_config *config);

/*
 * Reads size bytes of sshd_config text into config (which holds the
 * defaults or earlier lines).
 *
 * Keywords acted on: Port, ListenAddress, AddressFamily, HostKey,
 * PermitRootLogin, PasswordAuthentication, PermitEmptyPasswords,
 * PubkeyAuthentication, AuthorizedKeysFile, MaxAuthTries,
 * LoginGraceTime, MaxSessions, MaxStartups (the first number of the
 * "start:rate:full" form), StrictModes, Subsystem (only "sftp <command>";
 * another subsystem name is an error), ClientAliveInterval,
 * ClientAliveCountMax.  The value forms are sshd_config(5)'s (yes/no;
 * numbers in decimal; LoginGraceTime and ClientAliveInterval in seconds
 * with an optional s, m or h suffix).
 *
 * size above SSH_CONFIG_FILE_MAX is EMSGSIZE.  An unknown keyword, a bad
 * value or a Match line is EINVAL with error (capacity
 * SSH_CONFIG_ERROR_MAX) set to "line N: <what>".  Returns 0, EINVAL or
 * EMSGSIZE.
 */
int sshd_config_parse(struct sshd_config *config, const char *text, size_t size, char *error);

/* Adds the default host key when none was given.  Returns 0. */
int sshd_config_finish(struct sshd_config *config);

/* Clears config (set_mask 0, every field unset). */
void ssh_config_init(struct ssh_config *config);

/*
 * Reads size bytes of ssh_config text for destination host (the name the
 * user gave) into config, setting only fields not yet set.
 *
 * Keywords acted on: Host, HostName, User, Port, IdentityFile,
 * StrictHostKeyChecking (yes, no, ask, accept-new, off as no),
 * UserKnownHostsFile (the first path; "/dev/null" allowed),
 * GlobalKnownHostsFile, BatchMode, ConnectTimeout, ServerAliveInterval,
 * ServerAliveCountMax, RequestTTY, PasswordAuthentication,
 * PubkeyAuthentication, LogLevel (QUIET, FATAL, ERROR as 0; INFO 1;
 * VERBOSE 2; DEBUG, DEBUG1 to DEBUG3 as 3).  Errors as
 * sshd_config_parse.  Returns 0, EINVAL or EMSGSIZE.
 */
int ssh_config_parse(struct ssh_config *config, const char *text, size_t size, const char *host, char *error);

/*
 * Applies one -o option ("Keyword=value" or "Keyword value"), as a line
 * outside every Host block.  Errors as ssh_config_parse without a line
 * number.  Returns 0 or EINVAL.
 */
int ssh_config_option(struct ssh_config *config, const char *option, char *error);

/* Fills every field not set with its default. */
void ssh_config_defaults(struct ssh_config *config);

#endif
