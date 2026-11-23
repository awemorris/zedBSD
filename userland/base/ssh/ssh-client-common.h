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
 * The program runs one connection in one process.  The protocol is the
 * client engine's (ssh-client-engine.h), which does no I/O; the functions
 * here own the socket, the terminal, the files and the standard streams,
 * wait with poll, feed the engine the bytes read and act on its events.
 * Messages to the user go to the standard error stream; while the
 * terminal is in raw mode a line ends with CR LF.
 */

#ifndef SSH_SSH_CLIENT_COMMON_H
#define SSH_SSH_CLIENT_COMMON_H

#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <termios.h>

#include "ssh-client-engine.h"

/* The exit value of a connection that failed or ended without an exit status (ssh(1) EXIT STATUS). */
#define SSH_CLIENT_EXIT_FAILURE		255

/* The longest path the program builds. */
#define SSH_CLIENT_PATH_MAX		4096U

/* The most settings the command line may give (-o, -l, -p, -i, -L, -R and the tty and log flags). */
#define SSH_CLIENT_SETTINGS_MAX		64U

/* The longest setting line built from a command-line flag. */
#define SSH_CLIENT_SETTING_MAX		(SSH_CONFIG_PATH_MAX + 32U)

/* The longest command sent to the server (the remaining arguments joined by spaces). */
#define SSH_CLIENT_COMMAND_MAX		SSH_CHANNEL_COMMAND_MAX

/* The bytes read from the socket at once. */
#define SSH_CLIENT_RECEIVE_SIZE		16384U

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
 * The engine's output above which the standard input is not read, so
 * that the user's input does not pile up behind what the socket has not
 * taken yet.
 */
#define SSH_CLIENT_OUTPUT_HIGH		(1024U * 1024U)

/*
 * The most bytes of channel data kept for one output stream: the engine
 * gives the window back only for the bytes written, so at most one window
 * waits, beside at most half a window already written but not yet
 * removed.
 */
#define SSH_CLIENT_STREAM_MAX		(2U * SSH_CHANNEL_WINDOW)

/* The longest time spent writing the last bytes (the DISCONNECT) after the end, in milliseconds. */
#define SSH_CLIENT_CLOSE_WAIT_MS	2000U

/* The most addresses one local forwarding listens on (both loopbacks, both wildcards, or a name's first four). */
#define SSH_CLIENT_LISTEN_ADDRESSES	4U

/* The most sockets that listen for the local forwardings. */
#define SSH_CLIENT_LISTENERS_MAX	(SSH_CONFIG_FORWARDS_MAX * SSH_CLIENT_LISTEN_ADDRESSES)

/* The most forwarded connections at once: as many as the engine keeps forwarded channels. */
#define SSH_CLIENT_LINKS_MAX		SSH_ENGINE_FORWARD_CHANNELS_MAX

/* The most poll slots the forwardings take beside the program's own. */
#define SSH_CLIENT_FORWARD_SLOTS	(SSH_CLIENT_LISTENERS_MAX + SSH_CLIENT_LINKS_MAX)

/* The bytes read from a forwarded connection at once. */
#define SSH_CLIENT_LINK_CHUNK		16384U

/*
 * The most bytes of a forwarded channel's data kept for its socket: the
 * engine gives the window back only for the bytes written, so at most one
 * and a half windows wait.
 */
#define SSH_CLIENT_LINK_QUEUE_MAX	(SSH_ENGINE_FORWARD_WINDOW + SSH_ENGINE_FORWARD_WINDOW / 2U)

/* The longest time a remote forwarding's connection to its destination takes, in milliseconds. */
#define SSH_CLIENT_CONNECT_WAIT_MS	30000U

/* How long a listening socket rests after an accept failed for the lack of descriptors, in milliseconds. */
#define SSH_CLIENT_ACCEPT_PAUSE_MS	1000U

/* The backlog of a listening socket (OpenSSH's SSH_LISTEN_BACKLOG). */
#define SSH_CLIENT_LISTEN_BACKLOG	128

/*
 * The error the forwardings report when ExitOnForwardFailure ends the
 * connection: the reason is already printed, and the exit value is 255.
 */
#define SSH_CLIENT_FORWARD_FATAL	ECANCELED

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
	SSH_CLIENT_SETTING_IDENTITY = 3,

	/* -L: a local forwarding, "[bind_address:]port:host:hostport". */
	SSH_CLIENT_SETTING_LOCAL_FORWARD = 4,

	/* -R: a remote forwarding, "[bind_address:]port:host:hostport". */
	SSH_CLIENT_SETTING_REMOTE_FORWARD = 5
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
 * quiet -q, gateway_ports -g, and verbose counts -v.
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
	int gateway_ports;
	unsigned verbose;
};

/*
 * One connection of the program.
 *
 * config is the settings for the destination, host the name the user
 * gave, host_name the name connected to and looked up in known_hosts,
 * address the numeric address the socket reached (empty before),
 * user the remote user and port the port.  socket is the connected
 * socket (-1 before and after).  engine is the connection's protocol
 * (NULL before it is opened and after it is freed).  receive keeps bytes
 * read from the socket that the engine has not taken yet (receive_offset
 * to receive_size).  peer_closed is 1 once the socket reached its end or
 * failed, and end_told 1 once the engine was told so.  rekeys is how many
 * re-exchanges were reported at the debug level.  raw is 1 while the
 * terminal is in raw mode, so that messages end with CR LF.
 * identities_named is 1 when a setting named the identity files, so that
 * a missing one is reported.  identity_paths are the paths of the identity
 * files given to the engine, in its order (a passphrase is asked for by
 * the file's path).  The owner calls ssh_client_free.
 */
struct ssh_client {
	struct ssh_config config;
	char host[SSH_HOST_MAX + 1];
	char host_name[SSH_HOST_MAX + 1];
	char address[SSH_HOST_MAX + 1];
	char user[SSH_USER_MAX + 1];
	uint16_t port;
	int log_level;
	int socket;
	struct ssh_engine *engine;
	uint8_t receive[SSH_CLIENT_RECEIVE_SIZE];
	size_t receive_size;
	size_t receive_offset;
	int peer_closed;
	int end_told;
	unsigned long rekeys;
	int raw;
	int identities_named;
	char identity_paths[SSH_ENGINE_IDENTITIES_MAX][SSH_CLIENT_PATH_MAX];
};

/*
 * The identity files of a connection as the engine takes them: files
 * holds each file's bytes (secret) and identities names them (the
 * setting's path) and points at the bytes; count of each are used.
 */
struct ssh_client_identities {
	struct ssh_buf files[SSH_ENGINE_IDENTITIES_MAX];
	struct ssh_engine_identity identities[SSH_ENGINE_IDENTITIES_MAX];
	unsigned count;
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
 * What one forwarded connection is doing.
 *
 * A local forwarding's connection (accepted on a listening socket) waits
 * in OPENING for the server's answer to its direct-tcpip channel; a
 * remote forwarding's (a forwarded-tcpip channel the server opened)
 * connects to its destination in CONNECTING while the server waits for
 * the answer.  Both carry bytes in OPEN, and stay in CLOSING once this
 * side closed the channel, until the engine says it ended.
 */
enum ssh_client_link_state {
	SSH_CLIENT_LINK_FREE = 0,
	SSH_CLIENT_LINK_OPENING = 1,
	SSH_CLIENT_LINK_CONNECTING = 2,
	SSH_CLIENT_LINK_OPEN = 3,
	SSH_CLIENT_LINK_CLOSING = 4
};

/*
 * One socket that listens for a local forwarding (ssh -L).
 *
 * forward is the forwarding's place in the settings' list.  paused_until
 * is when an accept that failed for the lack of descriptors is tried
 * again (0 when not paused), so that a connection left in the backlog
 * does not keep poll from waiting.  slot is the socket's place in the
 * poll array of the last turn (-1 when it was not watched).
 */
struct ssh_client_listener {
	int socket;
	unsigned forward;
	uint64_t paused_until;
	int slot;
};

/*
 * One forwarded connection: a local socket and the channel that carries
 * it.
 *
 * socket is the local end (-1 once it is closed), channel the engine's
 * number of the channel, forward the forwarding's place in the settings'
 * list.  chunk holds the bytes read from the socket that the engine has
 * not taken yet (chunk_size of them, from chunk_offset); the socket is
 * read only when it is empty, so a channel without window stops the
 * sender.  socket_ended is 1 once the socket reached its end, eof_sent
 * once the server was told.  queue holds the server's bytes not yet
 * written to the socket (from queue_offset on).  channel_ended is 1 once
 * the server sends no more, shut once the socket's sending side was shut
 * after the last byte.  channel_gone is 1 once the engine said the
 * channel ended (the queue may still be written, without the window
 * given back).  addresses and next_address are the destination's
 * addresses and the next one to try while CONNECTING, until deadline.
 * slot is the socket's place in the poll array of the last turn (-1 when
 * it was not watched).
 */
struct ssh_client_link {
	enum ssh_client_link_state state;
	int socket;
	uint32_t channel;
	unsigned forward;
	uint8_t chunk[SSH_CLIENT_LINK_CHUNK];
	size_t chunk_size;
	size_t chunk_offset;
	int socket_ended;
	int eof_sent;
	struct ssh_buf queue;
	size_t queue_offset;
	int channel_ended;
	int shut;
	int channel_gone;
	struct addrinfo *addresses;
	struct addrinfo *next_address;
	uint64_t deadline;
	int slot;
};

/*
 * One remote forwarding asked of the server (ssh -R).
 *
 * forward is its place in the settings' list and number the engine's
 * number for it, by which SSH_ENGINE_FORWARD_REPLY and
 * SSH_ENGINE_FORWARD_OPEN name it.
 */
struct ssh_client_remote {
	unsigned forward;
	unsigned number;
};

/*
 * The port forwardings of one connection (ssh -L and -R): the sockets
 * that listen, the remote forwardings asked for, and the forwarded
 * connections.
 *
 * client is the connection, whose settings hold the forwardings.
 * listening is 1 while the local forwardings take connections (from the
 * login until the session ends).  listeners (listener_count used),
 * remotes (remote_count used) and links (a free one has state FREE) are
 * what is set up and running.  The owner starts it with
 * ssh_client_forwards_init and ends it with ssh_client_forwards_free.
 */
struct ssh_client_forwards {
	struct ssh_client *client;
	int listening;
	struct ssh_client_listener listeners[SSH_CLIENT_LISTENERS_MAX];
	unsigned listener_count;
	struct ssh_client_remote remotes[SSH_CONFIG_FORWARDS_MAX];
	unsigned remote_count;
	struct ssh_client_link links[SSH_CLIENT_LINKS_MAX];
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
 * then the tty and log flags and -g, then -l, -p, -i, -o, -L and -R in the
 * order given (the first value of a keyword wins, as in a configuration
 * file, and the forwardings add up).  A -L or -R spec that cannot be read
 * is "Bad local forwarding specification '...'" (or remote), as in
 * OpenSSH.  Returns 0 or EINVAL with error (capacity SSH_CONFIG_ERROR_MAX)
 * set.
 */
int ssh_client_apply_args(const struct ssh_client_args *args, struct ssh_config *config, char *error);

/*
 * Refuses settings the program does not act on although ssh_config reads
 * them: ForwardX11 yes, ForwardX11Trusted yes and a ForwardX11Timeout
 * other than 0 (ssh forwards no X11).  Returns 0 or EINVAL with error
 * (capacity SSH_CONFIG_ERROR_MAX) set.
 */
int ssh_client_check_settings(const struct ssh_config *config, char *error);

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

/* Starts a connection's state: no socket, no engine, nothing received. */
void ssh_client_init(struct ssh_client *client);

/* Wipes and frees everything of the connection, closes its socket and frees its engine. */
void ssh_client_free(struct ssh_client *client);

/*
 * Connects to host_name and port, trying each address getaddrinfo gives,
 * each within ConnectTimeout seconds when one is set.  Prints why on a
 * failure.  Returns 0 or the error of the last attempt.
 */
int ssh_client_connect(struct ssh_client *client);

/*
 * Reads from the socket into the receive buffer (only when the buffer is
 * empty).  Sets peer_closed at the end or on a failure.  Returns 0.
 */
int ssh_client_read_socket(struct ssh_client *client);

/* Writes the engine's pending output to the socket as far as it takes it.  Returns 0; sets peer_closed on a failure. */
int ssh_client_write_socket(struct ssh_client *client);

/*
 * Gives the engine the bytes read so far, and the end of the connection
 * once every byte before it is given.  *fed is set to 1 when the engine
 * took anything.  Returns 0 or the engine's error.
 */
int ssh_client_feed(struct ssh_client *client, int *fed);

/*
 * Prints why the connection ended with error, in the words of the user:
 * the server's DISCONNECT (ECONNABORTED: code and reason are the
 * server's), a server that did not identify itself in time (ETIMEDOUT), a
 * closed connection (ECONNRESET, EPIPE), a host key that changed in a
 * re-exchange (EACCES), the reason this side gave (reason, may be empty;
 * a failed negotiation is worded as the kind of algorithm without a
 * match), or else the error's name.
 */
void ssh_client_report(const struct ssh_client *client, int error, uint32_t code, const char *reason);

/*
 * Ends the connection: writes the engine's pending output (the
 * DISCONNECT) for at most SSH_CLIENT_CLOSE_WAIT_MS, and closes the
 * socket.
 */
void ssh_client_close(struct ssh_client *client);

/*
 * Checks the server's host key blob against the known_hosts files and the
 * settings (ssh_engine_host_decide): accepts it, adds it, asks the user,
 * or refuses it.  Returns 0 (accepted), EACCES (refused, after printing
 * why) or another error.
 */
int ssh_client_check_host_key(struct ssh_client *client, const uint8_t *blob, size_t size);

/*
 * Loads the identity files of the settings that can be used (readable by
 * the user alone, ssh-ed25519 private keys in the clear or encrypted with
 * a passphrase) into identities, naming each by its path in
 * client->identity_paths and reporting the others.  Returns 0 or ENOMEM.
 */
int ssh_client_load_identities(struct ssh_client *client, struct ssh_client_identities *identities);

/* Wipes and frees the identity files' bytes. */
void ssh_client_free_identities(struct ssh_client_identities *identities);

/*
 * Answers the engine's SSH_ENGINE_PASSWORD (tries refused before): asks
 * the password on the terminal and gives it to the engine (*sent 1), or
 * gives up when nobody can answer (*sent 0).  Returns 0 or the engine's
 * error.
 */
int ssh_client_answer_password(struct ssh_client *client, unsigned tries, int *sent);

/*
 * Answers the engine's SSH_ENGINE_PASSPHRASE for identity: asks the
 * passphrase of that file on the terminal (a wrong one is asked again
 * without a word, as ssh(1) does) and gives it to the engine, or gives up
 * the key when nobody can answer.  The passphrase is wiped after use.
 * Returns 0 or the engine's error.
 */
int ssh_client_answer_passphrase(struct ssh_client *client, unsigned identity);

/*
 * Runs the connection after the socket is connected: opens the engine,
 * logs in, and runs the session (size bytes at command, the shell when
 * size is 0) or, with -N, keeps the connection without one, carrying the
 * standard streams until the session or the connection ends.  Prints why
 * on a failure.  *exit_status receives the value to exit with: the
 * command's exit status, or 255.  Returns 0, or the error that kept the
 * connection from running (already reported).
 */
int ssh_client_run(struct ssh_client *client, const struct ssh_client_args *args, const uint8_t *command, size_t size, int *exit_status);

/*
 * Expands the %-tokens of the settings' RemoteCommand (ssh_config(5)
 * TOKENS: %%, %d, %h, %i, %j, %k, %L, %l, %n, %p, %r, %u) into out, for
 * the connection whose names, user and port are settled.  %C (a SHA-1
 * hash this program does not compute) and an unknown token are EINVAL
 * with error (capacity SSH_CONFIG_ERROR_MAX) set.  Returns 0, EINVAL,
 * EMSGSIZE (longer than SSH_CLIENT_COMMAND_MAX) or the error recorded in
 * out.
 */
int ssh_client_remote_command(const struct ssh_client *client, struct ssh_buf *out, char *error);

/* Starts the forwardings of client: nothing listens and nothing is asked yet. */
void ssh_client_forwards_init(struct ssh_client_forwards *forwards, struct ssh_client *client);

/*
 * Sets up the forwardings once the user is authenticated: listens for
 * each local forwarding and asks the server for each remote one, saying
 * why one could not be set up as ssh(1) does.  Returns 0, or
 * SSH_CLIENT_FORWARD_FATAL when ExitOnForwardFailure ends the connection
 * (the reason printed).
 */
int ssh_client_forwards_start(struct ssh_client_forwards *forwards);

/* Closes the listening sockets: the session ended, and no new forwarded connection is taken. */
void ssh_client_forwards_stop_listening(struct ssh_client_forwards *forwards);

/*
 * Takes an event of the engine that belongs to the forwardings: data,
 * EOF and the end of a channel other than the session's, and the answers
 * and opens of the forwardings.  *taken is set to 1 when it was one.
 * Returns 0, SSH_CLIENT_FORWARD_FATAL (ExitOnForwardFailure, the reason
 * printed) or ENOMEM.
 */
int ssh_client_forwards_event(struct ssh_client_forwards *forwards, const struct ssh_engine_event *event, int *taken);

/*
 * Moves the forwarded connections' bytes into the engine as far as the
 * channels' windows take them, and sends the EOFs and closes that are
 * due.
 */
void ssh_client_forwards_pump(struct ssh_client_forwards *forwards);

/*
 * Puts the sockets the forwardings wait on into slots (at most capacity
 * of them) and returns how many.  *wake_ms is lowered to the time a
 * paused listener or a connection's deadline wants the loop back.
 */
unsigned ssh_client_forwards_poll(struct ssh_client_forwards *forwards, struct pollfd *slots, unsigned capacity, uint64_t now_ms, uint64_t *wake_ms);

/* Acts on what poll reported for the slots ssh_client_forwards_poll filled, and on the deadlines past now_ms. */
void ssh_client_forwards_serve(struct ssh_client_forwards *forwards, const struct pollfd *slots, uint64_t now_ms);

/* Reports whether a forwarded connection still has the server's bytes to write to its socket. */
int ssh_client_forwards_draining(const struct ssh_client_forwards *forwards);

/* Closes every socket of the forwardings and frees what they hold. */
void ssh_client_forwards_free(struct ssh_client_forwards *forwards);

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
