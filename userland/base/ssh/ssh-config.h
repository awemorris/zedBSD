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

/* The longest RemoteCommand and ProxyCommand held, in bytes. */
#define SSH_CONFIG_COMMAND_MAX	4096U

/* The most HostKey and IdentityFile entries. */
#define SSH_CONFIG_KEYS_MAX	8U

/* The longest error text. */
#define SSH_CONFIG_ERROR_MAX	256U

/* The longest host name or address held in a forwarding or a permission (as a DNS name, RFC 1035 2.3.4). */
#define SSH_CONFIG_HOST_MAX	255U

/* The most forwardings of each kind (local, remote) one connection takes. */
#define SSH_CONFIG_FORWARDS_MAX	16U

/* The most entries of sshd's PermitOpen and of its PermitListen. */
#define SSHD_PERMITS_MAX	16U

/* The bits of sshd's AllowTcpForwarding: direct-tcpip (local) and tcpip-forward (remote) allowed. */
#define SSHD_TCP_FORWARD_LOCAL	1U
#define SSHD_TCP_FORWARD_REMOTE	2U

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

/* The values of sshd's GatewayPorts (sshd_config(5)). */
enum sshd_gateway_ports {
	SSHD_GATEWAY_NO = 0,
	SSHD_GATEWAY_YES = 1,
	SSHD_GATEWAY_CLIENT_SPECIFIED = 2
};

/* What sshd's PermitOpen or PermitListen allows: any (the default), none, or the entries listed. */
enum sshd_permit_mode {
	SSHD_PERMIT_ANY = 0,
	SSHD_PERMIT_NONE = 1,
	SSHD_PERMIT_LIST = 2
};

/*
 * One entry of PermitOpen or PermitListen (sshd_config(5)): host as
 * written without the brackets of an IPv6 address ("*" for any host;
 * PermitListen's port alone is "*"), and port (0 for "*", any port).
 */
struct sshd_permit {
	char host[SSH_CONFIG_HOST_MAX + 1];
	uint16_t port;
};

/* The kinds of a forwarding (ssh(1) -L and -R). */
enum ssh_forward_kind {
	SSH_FORWARD_LOCAL = 0,
	SSH_FORWARD_REMOTE = 1
};

/*
 * One forwarding as written (ssh(1) -L, -R; ssh_config(5) LocalForward,
 * RemoteForward).  have_listen_host is 0 when no bind address was given;
 * listen_host is the bind address given ("" for "" or "*", which mean
 * every address).  listen_port is the port listened on (0 only for
 * REMOTE: the server's choice).  connect_host and connect_port are the
 * destination.
 */
struct ssh_forward {
	enum ssh_forward_kind kind;
	int have_listen_host;
	char listen_host[SSH_CONFIG_HOST_MAX + 1];
	uint16_t listen_port;
	char connect_host[SSH_CONFIG_HOST_MAX + 1];
	uint16_t connect_port;
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
 * max_sessions 10 (OpenSSH's; at most SSH_CHANNEL_MAX take effect),
 * max_startups 10 (connections not yet
 * authenticated), authorized_keys_file ".ssh/authorized_keys" (relative
 * to the home directory; %h and %u expand to the home and the user),
 * strict_modes 1, sftp_command "" (no sftp subsystem), client_alive
 * interval 0 (off) and count 3, and OpenSSH's for the forwardings:
 * tcp_forwarding both bits (AllowTcpForwarding yes), gateway_ports NO,
 * permit_open_mode and permit_listen_mode ANY (with no entries),
 * disable_forwarding 0.
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
	unsigned tcp_forwarding;
	enum sshd_gateway_ports gateway_ports;
	enum sshd_permit_mode permit_open_mode;
	struct sshd_permit permit_open[SSHD_PERMITS_MAX];
	unsigned permit_open_count;
	enum sshd_permit_mode permit_listen_mode;
	struct sshd_permit permit_listen[SSHD_PERMITS_MAX];
	unsigned permit_listen_count;
	int disable_forwarding;
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
 * host_name "" (the name given), forward_x11 0, forward_x11_trusted 0,
 * forward_x11_timeout 0 (seconds; 0 for none, where OpenSSH's default is
 * 20 minutes: the program that forwards X11 decides what the time means),
 * gateway_ports 0, exit_on_forward_failure 0, clear_all_forwardings 0,
 * session_type_none 0, remote_command "" (none: the shell, or the
 * command the user gave the program), proxy_command "" (none: the host is
 * connected to directly).  forward holds forward_count forwardings, from
 * every source in order (each kind at most SSH_CONFIG_FORWARDS_MAX, the
 * same one once); ssh_config_defaults empties it when
 * clear_all_forwardings is 1.
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
	int forward_x11;
	int forward_x11_trusted;
	unsigned forward_x11_timeout;
	int gateway_ports;
	int exit_on_forward_failure;
	int clear_all_forwardings;
	int session_type_none;
	unsigned forward_count;
	struct ssh_forward forward[2U * SSH_CONFIG_FORWARDS_MAX];
	char remote_command[SSH_CONFIG_COMMAND_MAX + 1];
	char proxy_command[SSH_CONFIG_COMMAND_MAX + 1];
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
 * ClientAliveCountMax, AllowTcpForwarding (yes, all, no, local, remote),
 * GatewayPorts (no, yes, clientspecified), PermitOpen (any, none, or up to
 * SSHD_PERMITS_MAX of host:port, [IPv6]:port, with "*" for any host or
 * port), PermitListen (any, none, or up to SSHD_PERMITS_MAX of port or
 * host:port, "*" for any port), DisableForwarding.  The value forms are
 * sshd_config(5)'s (yes/no; numbers in decimal; LoginGraceTime and
 * ClientAliveInterval in seconds with an optional s, m or h suffix).
 * Which of them a program acts on is the program's.
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
 * VERBOSE 2; DEBUG, DEBUG1 to DEBUG3 as 3), ForwardX11, ForwardX11Trusted
 * (yes/no), ForwardX11Timeout (a time as LoginGraceTime's),
 * LocalForward, RemoteForward ("[bind_address:]port host:hostport", as
 * ssh_config_parse_forward reads them joined by a colon), GatewayPorts,
 * ExitOnForwardFailure,
 * ClearAllForwardings (yes/no), SessionType (default or none),
 * RemoteCommand (the rest of the line as written, quotes and all, as
 * OpenSSH keeps it; "none" is no command; its %-tokens are left for the
 * program to expand), ProxyCommand (the same way; "none" is no proxy).  The
 * forwardings add up, from every line of every block that applies; the
 * other keywords keep their first value.  Which of them a program acts on
 * is the program's: one that does not forward X11 or ports refuses the
 * settings that ask for it.  Errors as sshd_config_parse.  Returns 0,
 * EINVAL or EMSGSIZE.
 */
int ssh_config_parse(struct ssh_config *config, const char *text, size_t size, const char *host, char *error);

/*
 * Applies one -o option ("Keyword=value" or "Keyword value"), as a line
 * outside every Host block.  Errors as ssh_config_parse without a line
 * number.  Returns 0 or EINVAL.
 */
int ssh_config_option(struct ssh_config *config, const char *option, char *error);

/* Fills every field not set with its default, and empties the forwardings when ClearAllForwardings is yes. */
void ssh_config_defaults(struct ssh_config *config);

/*
 * Reads one forwarding of kind from spec (ssh(1)'s -L or -R argument, or
 * a configuration line's values joined by a colon) into forward.
 *
 * The fields are separated by ':'; a field in square brackets is taken as
 * it is (an IPv6 address); a backslash takes the next character as it is.
 * Both kinds take "[bind_address:]port:host:hostport".  A port is a
 * decimal number from 1 to 65535 (a REMOTE listen port may be 0); a host
 * is 1 to SSH_CONFIG_HOST_MAX printable characters without blanks (a bind
 * address may be empty).
 *
 * Returns 0; EINVAL for a malformed spec or another kind; or ENOTSUP for
 * a form OpenSSH has but these programs do not: a Unix-domain socket (a
 * field with '/') and a REMOTE spec without a destination (the client as
 * a SOCKS proxy).  error (capacity SSH_CONFIG_ERROR_MAX, may be NULL)
 * says which.
 */
int ssh_config_parse_forward(enum ssh_forward_kind kind, const char *spec, struct ssh_forward *forward, char *error);

/*
 * Adds forward to config's list unless the same forwarding is there
 * already.  More than SSH_CONFIG_FORWARDS_MAX of its kind is EINVAL with
 * error (capacity SSH_CONFIG_ERROR_MAX, may be NULL) "too many
 * forwardings".  Returns 0 or EINVAL.
 */
int ssh_config_add_forward(struct ssh_config *config, const struct ssh_forward *forward, char *error);

/*
 * The function ssh_config_hosts gives each host name to: data as given,
 * the name (size bytes, not NUL-terminated, pointing into the text).  It
 * answers 0 to go on, or a nonzero value that stops the walk and is
 * passed back.
 */
typedef int (*ssh_config_host_handler)(void *data, const char *name, size_t size);

/*
 * Gives host each name of the Host lines of size bytes of ssh_config text,
 * in the order written (a name given twice comes twice): the patterns
 * without '*', '?' or '!' (ssh_config(5) PATTERNS), so the names of the
 * hosts the file has settings for, for a list a user picks from.  Lines
 * other than Host are not looked at, so this succeeds on a file whose
 * other keywords ssh_config_parse refuses.  A text it cannot split (a NUL,
 * a line too long, an unbalanced quote) is EINVAL with error (capacity
 * SSH_CONFIG_ERROR_MAX, may be NULL) set as ssh_config_parse sets it;
 * size above SSH_CONFIG_FILE_MAX is EMSGSIZE.  Returns 0, EINVAL,
 * EMSGSIZE, or the nonzero answer with which host stopped.
 */
int ssh_config_hosts(const char *text, size_t size, ssh_config_host_handler host, void *data, char *error);

#endif
