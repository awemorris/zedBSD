/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of the ssh program that its files share: the command line as
 * read, the state of one connection, and the functions each file offers
 * the others.
 *
 * The program runs one connection in one process.  The library layers do
 * no I/O; the functions here own the socket, the terminal and the
 * standard streams, wait with poll, and feed bytes to the transport and
 * take messages from it.  Messages to the user go to the standard error
 * stream; while the terminal is in raw mode a line ends with CR LF.
 */

#ifndef SSH_SSH_CLIENT_COMMON_H
#define SSH_SSH_CLIENT_COMMON_H

#include <stdarg.h>
#include <termios.h>

#include "ssh-auth.h"
#include "ssh-channel.h"
#include "ssh-config.h"
#include "ssh-hostfile.h"
#include "ssh-transport.h"

/* The exit value of a connection that failed or ended without an exit status (ssh(1) EXIT STATUS). */
#define SSH_CLIENT_EXIT_FAILURE		255

/* The longest path the program builds. */
#define SSH_CLIENT_PATH_MAX		4096U

/* The most settings the command line may give (-o, -l, -p, -i and the tty and log flags). */
#define SSH_CLIENT_SETTINGS_MAX		64U

/* The longest setting line built from a command-line flag. */
#define SSH_CLIENT_SETTING_MAX		(SSH_CONFIG_PATH_MAX + 32U)

/* The longest command sent to the server (the remaining arguments joined by spaces). */
#define SSH_CLIENT_COMMAND_MAX		SSH_CHANNEL_COMMAND_MAX

/*
 * The bytes read from the socket at once, and the most fed to the
 * transport at once (the transport's input keeps room for at least that
 * much beside one partial packet).
 */
#define SSH_CLIENT_RECEIVE_SIZE		16384U
#define SSH_CLIENT_FEED_SIZE		4096U

/*
 * The most bytes written to a stream that is not a regular file in one
 * call: poll reports a pipe writable when this much fits (POSIX's
 * smallest PIPE_BUF), so the write does not block.
 */
#define SSH_CLIENT_STREAM_CHUNK		512U

/* The most bytes written to a regular file in one call. */
#define SSH_CLIENT_FILE_CHUNK		65536U

/* The bytes read from the standard input at once. */
#define SSH_CLIENT_INPUT_CHUNK		16384U

/*
 * The transport output above which the standard input is not read, so
 * that the program's own replies (window adjustments, keepalives) always
 * fit below the transport's limit.
 */
#define SSH_CLIENT_OUTPUT_HIGH		(1024U * 1024U)

/* The most bytes of upper-layer messages kept while the transport refuses more. */
#define SSH_CLIENT_BACKLOG_MAX		(1024U * 1024U)

/*
 * The most bytes of channel data kept for one output stream: the window
 * is given back only while less than half a window waits, so at most one
 * and a half windows wait, beside at most half a window already written
 * but not yet removed.
 */
#define SSH_CLIENT_STREAM_MAX		(2U * SSH_CHANNEL_WINDOW)

/* How many times a password is asked for (OpenSSH's NumberOfPasswordPrompts default). */
#define SSH_CLIENT_PASSWORD_TRIES	3U

/* The longest time spent writing the last bytes (the DISCONNECT) after the end, in milliseconds. */
#define SSH_CLIENT_CLOSE_WAIT_MS	2000U

/* The log levels of ssh_config's LogLevel as ssh-config.h maps them. */
#define SSH_CLIENT_LOG_QUIET		0
#define SSH_CLIENT_LOG_INFO		1
#define SSH_CLIENT_LOG_VERBOSE		2
#define SSH_CLIENT_LOG_DEBUG		3

/* What one command-line setting gives. */
enum ssh_client_setting_kind {
	/* A -o option, given to ssh_config_option as it is. */
	SSH_CLIENT_SETTING_OPTION = 0,

	/* -l: the user. */
	SSH_CLIENT_SETTING_USER = 1,

	/* -p: the port. */
	SSH_CLIENT_SETTING_PORT = 2,

	/* -i: an identity file. */
	SSH_CLIENT_SETTING_IDENTITY = 3
};

/*
 * One setting from the command line, in the order given.
 *
 * value points into argv.
 */
struct ssh_client_setting {
	enum ssh_client_setting_kind kind;
	const char *value;
};

/*
 * The command line as read.
 *
 * user is the user of a "user@host" destination (empty when none) and
 * host the host part.  config_file is the -F file (NULL when none).
 * command and command_count are the arguments after the destination.
 * tty_count counts -t, no_tty is -T, null_stdin -n, no_command -N,
 * quiet -q, and verbose counts -v.
 */
struct ssh_client_args {
	struct ssh_client_setting settings[SSH_CLIENT_SETTINGS_MAX];
	unsigned setting_count;
	char user[SSH_USER_MAX + 1];
	char host[SSH_HOST_MAX + 1];
	const char *config_file;
	char **command;
	int command_count;
	unsigned tty_count;
	int no_tty;
	int null_stdin;
	int no_command;
	int quiet;
	unsigned verbose;
};

/*
 * One connection of the program.
 *
 * config is the settings for the destination, host the name the user
 * gave, host_name the name connected to and looked up in known_hosts,
 * user the remote user and port the port.  socket is the connected
 * socket (-1 before and after).  payload holds the message the transport
 * last handed up.  backlog keeps upper-layer messages (uint32 length and
 * the bytes) that the transport refused while its output was full, so
 * that they go out in order.  receive keeps bytes read from the socket
 * that the transport has not taken yet (receive_offset to receive_size).
 * peer_closed is 1 once the socket reached its end or failed.  host_key
 * is the server's key blob accepted in the first exchange (have_host_key
 * 1 after it): every later exchange must show the same key.  raw is 1
 * while the terminal is in raw mode, so that messages end with CR LF.
 * The owner calls ssh_client_free.
 */
struct ssh_client {
	struct ssh_config config;
	char host[SSH_HOST_MAX + 1];
	char host_name[SSH_HOST_MAX + 1];
	char user[SSH_USER_MAX + 1];
	uint16_t port;
	int log_level;
	int socket;
	int transport_started;
	struct ssh_transport transport;
	struct ssh_buf payload;
	struct ssh_buf backlog;
	uint8_t receive[SSH_CLIENT_RECEIVE_SIZE];
	size_t receive_size;
	size_t receive_offset;
	int peer_closed;
	uint8_t host_key[SSH_KEY_BLOB_SIZE];
	int have_host_key;
	int raw;
	int identities_named;
};

/* What to do with a host key after looking it up (ssh_client_host_decide). */
enum ssh_client_host_action {
	/* The key is known for the host: go on. */
	SSH_CLIENT_HOST_ACCEPT = 0,

	/* The host is not known: go on and add the key to the user's known_hosts. */
	SSH_CLIENT_HOST_ADD = 1,

	/* The host is not known: ask the user. */
	SSH_CLIENT_HOST_ASK = 2,

	/* The host is known with another key: refuse. */
	SSH_CLIENT_HOST_REFUSE_CHANGED = 3,

	/* The key is revoked: refuse. */
	SSH_CLIENT_HOST_REFUSE_REVOKED = 4,

	/* The host is not known and the settings forbid adding it: refuse. */
	SSH_CLIENT_HOST_REFUSE_UNKNOWN = 5
};

/*
 * The escape sequences of the user's input on a terminal (ssh(1) ESCAPE
 * CHARACTERS: "~." ends the connection, "~~" sends one '~'; the escape
 * character counts only at the start of a line).
 *
 * line_start is 1 at the start of a line, pending 1 when a '~' at the
 * start of a line waits for the next byte.
 */
struct ssh_client_escape {
	int line_start;
	int pending;
};

/*
 * Reads the command line into args (argv[0] is the program name).
 *
 * Options come before the destination; the first argument that is not an
 * option is the destination, and everything after it is the command.  An
 * unknown option, a missing argument, an empty destination, or a user or
 * host part too long is EINVAL with error (capacity bytes) describing it.
 * Returns 0 or EINVAL.
 */
int ssh_client_parse_args(int argc, char **argv, struct ssh_client_args *args, char *error, size_t capacity);

/*
 * Joins the command arguments with single spaces into out (as the
 * command string of an exec request).  Returns 0, EMSGSIZE (longer than
 * SSH_CLIENT_COMMAND_MAX) or the error recorded in out.
 */
int ssh_client_join_command(const struct ssh_client_args *args, struct ssh_buf *out);

/*
 * Applies the command-line settings to config: the destination's user,
 * then the tty and log flags, then -l, -p, -i and -o in the order given
 * (the first value of a keyword wins, as in a configuration file).
 * Returns 0 or EINVAL with error (capacity SSH_CONFIG_ERROR_MAX) set.
 */
int ssh_client_apply_args(const struct ssh_client_args *args, struct ssh_config *config, char *error);

/*
 * Writes the home directory (HOME, or the account's when HOME is not set)
 * into out.  Returns 0, ENOENT or ENAMETOOLONG.
 */
int ssh_client_home(char *out, size_t capacity);

/*
 * Expands a leading "~/" (or a path that is just "~") to the home
 * directory and writes the path into out.  Returns 0, ENOENT or
 * ENAMETOOLONG.
 */
int ssh_client_expand_path(const char *path, char *out, size_t capacity);

/*
 * Reads the whole file at path into out (EMSGSIZE when it is longer than
 * limit bytes).  Returns 0 or the error of the system (ENOENT for a file
 * that is not there).
 */
int ssh_client_read_file(const char *path, struct ssh_buf *out, size_t limit);

/*
 * Prints a message to the standard error stream when level is at most
 * the connection's log level, ending it with LF, or CR LF while the
 * terminal is raw.  client may be NULL (level INFO is then assumed).
 */
void ssh_client_log(const struct ssh_client *client, int level, const char *format, ...);

/*
 * Asks the user on the controlling terminal (/dev/tty): writes prompt,
 * reads one line into out (capacity bytes with the NUL, the LF taken
 * off), with echo off when echo is 0.  Returns 0, ENXIO (no terminal),
 * EMSGSIZE (a longer line, which is wiped), or the error of the system.
 */
int ssh_client_ask(const char *prompt, int echo, char *out, size_t capacity);

/* Returns the time of a monotonic clock in milliseconds. */
uint64_t ssh_client_now_ms(void);

/* Starts a connection's state: empty buffers, no socket, the transport not started. */
void ssh_client_init(struct ssh_client *client);

/* Wipes and frees everything of the connection and closes its socket. */
void ssh_client_free(struct ssh_client *client);

/*
 * Connects to host_name and port, trying each address getaddrinfo gives,
 * each within ConnectTimeout seconds when one is set.  Prints why on a
 * failure.  Returns 0 or the error of the last attempt.
 */
int ssh_client_connect(struct ssh_client *client);

/*
 * Starts the transport as a client, with the host key check of
 * ssh_client_check_host_key.  Returns 0 or ENOMEM.
 */
int ssh_client_start(struct ssh_client *client);

/*
 * Sends one upper-layer message: to the transport, or to the backlog
 * while the transport's output is full or the backlog is not empty.
 * Returns 0, ENOBUFS (the backlog is full too), or the transport's error.
 */
int ssh_client_send(struct ssh_client *client, const uint8_t *payload, size_t size);

/* Sends the message in out (as ssh_client_send), or the error recorded in out. */
int ssh_client_send_buf(struct ssh_client *client, const struct ssh_buf *out);

/* Moves backlog messages into the transport while it takes them.  Returns 0 or the transport's error. */
int ssh_client_flush(struct ssh_client *client);

/*
 * Feeds the bytes read so far to the transport and takes the next
 * upper-layer message into client->payload.  Returns 0 (a message),
 * EAGAIN (none until more bytes come), or the transport's error.
 */
int ssh_client_receive(struct ssh_client *client);

/*
 * Reads from the socket into the receive buffer (only when the buffer is
 * empty).  Sets peer_closed at the end or on a failure.  Returns 0.
 */
int ssh_client_read_socket(struct ssh_client *client);

/* Writes the transport's pending output to the socket as far as it takes it.  Returns 0; sets peer_closed on a failure. */
int ssh_client_write_socket(struct ssh_client *client);

/*
 * Waits until the transport hands up one message (into client->payload),
 * writing the output meanwhile and starting re-exchanges on time.
 *
 * deadline_ms (0 for none) bounds the wait for the server's
 * identification line (ConnectTimeout).  Returns 0, ETIMEDOUT,
 * ECONNRESET (the server closed the connection), or the transport's
 * error.
 */
int ssh_client_wait(struct ssh_client *client, uint64_t deadline_ms);

/*
 * Prints why the connection ended with error (as returned by the
 * functions above), in the words of the user: the server's DISCONNECT,
 * the reason this side gave for a failed key exchange, a closed
 * connection.  Prints nothing for EACCES of the host key check, which
 * printed its own message.
 */
void ssh_client_report(struct ssh_client *client, int error);

/*
 * Ends the connection: queues DISCONNECT with code and description unless
 * the transport is already closed, writes the pending output for at most
 * SSH_CLIENT_CLOSE_WAIT_MS, and closes the socket.
 */
void ssh_client_close(struct ssh_client *client, uint32_t code, const char *description);

/*
 * Finds the description of the DISCONNECT this side queued in its
 * output, while the output is not yet encrypted (before the first
 * NEWKEYS).  Writes it into out (capacity bytes with the NUL) and returns
 * 1, or returns 0 when there is none.
 */
int ssh_client_local_reason(const struct ssh_client *client, char *out, size_t capacity);

/*
 * Decides what to do with a host key from the answers of the user's and
 * the global known_hosts files and the settings (ssh_config(5)
 * StrictHostKeyChecking and BatchMode).
 *
 * A revoked key in either file is refused; a key known in either file is
 * accepted; another key known for the host is refused (whatever the
 * settings say); an unknown host (no line, or lines of other key types
 * only) is added (no, accept-new), refused (yes, or ask in batch mode)
 * or asked about (ask).  Returns 0 or EINVAL (a value out of its range).
 */
int ssh_client_host_decide(enum ssh_known_result user_answer, enum ssh_known_result global_answer, enum ssh_strict_host strict, int batch_mode, enum ssh_client_host_action *action);

/*
 * Checks the server's host key blob (struct ssh_transport_ops
 * check_host_key; context is the struct ssh_client).
 *
 * In the first exchange the key is looked up in the known_hosts files and
 * accepted, added, asked about or refused as ssh_client_host_decide says;
 * in every later exchange it must be the key accepted first.  Returns 0,
 * EACCES (refused, after printing why) or another error.
 */
int ssh_client_check_host_key(void *context, const uint8_t *blob, size_t size);

/*
 * Runs the user authentication (RFC 4252) after the transport has
 * started: the service request, "none", the identity files with
 * "publickey", then "password" on the terminal.  Prints why on a failure.
 * Returns 0 (authenticated), EACCES (every method failed), or the error of
 * the connection.
 */
int ssh_client_authenticate(struct ssh_client *client);

/*
 * Runs the session channel: opens it, asks for a terminal when one is
 * wanted, starts the shell or the command (size bytes at command, none
 * when size is 0), and carries the standard streams until the channel
 * closes.  *exit_status receives the value to exit with.  Returns 0 or the
 * error that ended the connection (already reported).
 */
int ssh_client_run_session(struct ssh_client *client, const struct ssh_client_args *args, const uint8_t *command, size_t size, int *exit_status);

/*
 * Keeps the connection without a session (-N) until the server closes it
 * or a signal ends the program.  *exit_status receives the value to exit
 * with.  Returns 0 or the error that ended the connection (already
 * reported).
 */
int ssh_client_run_without_session(struct ssh_client *client, int *exit_status);

/* Starts the escape state at the start of a line. */
void ssh_client_escape_init(struct ssh_client_escape *escape);

/*
 * Passes size bytes of the user's input through the escape state into
 * out, which has room for size + 1 bytes.
 *
 * *out_size receives the bytes to send.  *disconnect is set to 1 when
 * "~." was read; the bytes after it are not looked at.
 */
void ssh_client_escape_filter(struct ssh_client_escape *escape, const uint8_t *in, size_t size, uint8_t *out, size_t *out_size, int *disconnect);

#endif
