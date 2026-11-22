/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The client side of one SSH connection as a state machine that does no
 * I/O: the transport (RFC 4253), the user authentication (RFC 4252: "none",
 * then "publickey" with each identity, then "password"), and one session
 * channel (RFC 4254 6) with or without a pseudo-terminal.
 *
 * The caller owns the socket, the clock, the files and the user.  It
 * feeds the bytes it reads with ssh_engine_input (none at all, size 0,
 * when the connection reached its end), writes what ssh_engine_output
 * returns and reports it with ssh_engine_written, calls ssh_engine_tick on
 * time, and takes what happened with ssh_engine_next until it answers
 * EAGAIN.  An event may ask the caller something (the host key, a
 * password); the engine then waits, keeping the connection, until the
 * answering call.  Nothing blocks; one engine is one connection and
 * engines share no state, so a program may run many, each from one thread
 * at a time.
 *
 * The server's host key is shown (SSH_ENGINE_HOST_KEY) only after its
 * signature over the exchange hash verified and the first exchange ended,
 * and nothing of the user (the service request, a key, a password, the
 * session) is sent until the caller accepts it.  A later exchange must
 * show the same key, or the connection ends.
 *
 * The user's keys are unencrypted ssh-ed25519 private key files
 * (openssh-key-v1) given as their bytes; a file that cannot be used (an
 * encrypted one among them) is passed over.
 *
 * The received data of the session is handed out as it comes
 * (SSH_ENGINE_DATA); the channel's window is given back only for what the
 * caller reports used with ssh_engine_consumed, so the server stops when
 * the caller falls behind by one window (SSH_CHANNEL_WINDOW) and the
 * caller never holds more than that.
 */

#ifndef SSH_SSH_CLIENT_ENGINE_H
#define SSH_SSH_CLIENT_ENGINE_H

#include "ssh-auth.h"
#include "ssh-channel.h"
#include "ssh-config.h"
#include "ssh-hostfile.h"
#include "ssh-transport.h"

/* The most identity files one engine tries. */
#define SSH_ENGINE_IDENTITIES_MAX	SSH_CONFIG_KEYS_MAX

/* How many times a password is asked for (OpenSSH's NumberOfPasswordPrompts default). */
#define SSH_ENGINE_PASSWORD_TRIES	3U

/* The longest name-list of methods kept from USERAUTH_FAILURE. */
#define SSH_ENGINE_METHODS_MAX		1024U

/* The longest text of an event's reason. */
#define SSH_ENGINE_REASON_MAX		(SSH_TRANSPORT_REASON_MAX + SSH_ENGINE_METHODS_MAX)

/* What an event says. */
enum ssh_engine_event_kind {
	/*
	 * The server's host key blob (bytes, size) after the first exchange
	 * ended.  The caller answers with ssh_engine_host_answer; until then
	 * nothing more is sent but the transport's own messages.
	 */
	SSH_ENGINE_HOST_KEY = 1,

	/* A banner the server shows before the login (bytes, size: text made safe for a terminal). */
	SSH_ENGINE_BANNER = 2,

	/*
	 * The server would take a password; tries is how many were refused
	 * before.  The caller answers with ssh_engine_password.
	 */
	SSH_ENGINE_PASSWORD = 3,

	/* The server accepted the user. */
	SSH_ENGINE_AUTHENTICATED = 4,

	/* The shell or the command started; pty is 1 when the server gave the terminal asked for. */
	SSH_ENGINE_STARTED = 5,

	/*
	 * Data of the session (bytes, size; stream 0 for the output, 1 for the
	 * error output).  The bytes stay valid until the next ssh_engine_next
	 * or ssh_engine_close; the caller reports them used with
	 * ssh_engine_consumed.
	 */
	SSH_ENGINE_DATA = 6,

	/*
	 * The session channel closed: exit_status is the command's status (-1
	 * when the server sent none), exit_signal the signal that ended it
	 * (empty when none), reason why the session failed (empty when it did
	 * not).  The connection then ends with SSH_ENGINE_CLOSED.
	 */
	SSH_ENGINE_EXIT = 7,

	/*
	 * The connection ended; the last event.  error is 0 when this side
	 * ended it as asked (after EXIT, or ssh_engine_disconnect),
	 * ECONNABORTED when the server sent DISCONNECT (code and reason are
	 * the server's), ECONNRESET when the connection reached its end
	 * without one, EACCES when the host key was refused or no
	 * authentication method was left, ETIMEDOUT when the server answered
	 * no keepalive, ECONNREFUSED when the server refused the session, or
	 * the transport's error (code and reason are then the DISCONNECT this
	 * side sent).  Only the pending output remains.
	 */
	SSH_ENGINE_CLOSED = 8
};

/* What to do with a host key after looking it up (ssh_engine_host_decide). */
enum ssh_engine_host_action {
	/* The key is known for the host: accept it. */
	SSH_ENGINE_HOST_ACCEPT = 0,

	/* The host is not known: accept it and add the key to the user's known_hosts. */
	SSH_ENGINE_HOST_ADD = 1,

	/* The host is not known: ask the user. */
	SSH_ENGINE_HOST_ASK = 2,

	/* The host is known with another key: refuse. */
	SSH_ENGINE_HOST_REFUSE_CHANGED = 3,

	/* The key is revoked: refuse. */
	SSH_ENGINE_HOST_REFUSE_REVOKED = 4,

	/* The host is not known and the settings forbid adding it: refuse. */
	SSH_ENGINE_HOST_REFUSE_UNKNOWN = 5
};

/* The opaque state of one connection; made by ssh_engine_open, freed by ssh_engine_close. */
struct ssh_engine;

/*
 * One event, filled by ssh_engine_next.  Which fields mean something
 * depends on kind (above); the others are zero.
 */
struct ssh_engine_event {
	enum ssh_engine_event_kind kind;
	const uint8_t *bytes;
	size_t size;
	unsigned stream;
	unsigned tries;
	int pty;
	int exit_status;
	char exit_signal[SSH_CHANNEL_SIGNAL_MAX + 1];
	int error;
	uint32_t code;
	char reason[SSH_ENGINE_REASON_MAX];
};

/*
 * One identity: name names it for the caller (a path; may be NULL), file
 * and size are the bytes of the private key file.  The engine copies both.
 */
struct ssh_engine_identity {
	const char *name;
	const uint8_t *file;
	size_t size;
};

/*
 * What one connection does.
 *
 * user is the remote user (1 to SSH_USER_MAX bytes).  identities (count
 * identity_count, at most SSH_ENGINE_IDENTITIES_MAX) are tried in order
 * when pubkey is 1; passwords are asked for when password is 1.  session
 * 1 opens a session channel after the login (0 keeps the connection
 * without one).  pty 1 asks for a pseudo-terminal of type term (NULL for
 * "dumb", at most SSH_CHANNEL_TERM_MAX bytes), columns and rows, width and
 * height in pixels, with the encoded terminal modes (RFC 4254 8; NULL for
 * ssh_engine_put_default_modes's).  command (command_size bytes) is run
 * with exec; NULL or size 0 starts the shell.  alive_interval is
 * ssh_config(5)'s ServerAliveInterval in seconds (0 for none) and
 * alive_count its ServerAliveCountMax.  now_ms is the time of a monotonic
 * clock in milliseconds.
 */
struct ssh_engine_options {
	const char *user;
	const struct ssh_engine_identity *identities;
	unsigned identity_count;
	int pubkey;
	int password;
	int session;
	int pty;
	const char *term;
	unsigned columns;
	unsigned rows;
	unsigned width;
	unsigned height;
	const uint8_t *modes;
	size_t modes_size;
	const uint8_t *command;
	size_t command_size;
	unsigned alive_interval;
	unsigned alive_count;
	uint64_t now_ms;
};

/*
 * Makes an engine for one connection and queues this side's
 * identification line in its output.  The options are copied.  Returns
 * 0, EINVAL (a bad option) or ENOMEM.
 */
int ssh_engine_open(const struct ssh_engine_options *options, struct ssh_engine **engine);

/* Wipes every secret of the engine and frees it; NULL is ignored. */
void ssh_engine_close(struct ssh_engine *engine);

/*
 * Takes size received bytes; *taken receives how many were taken (fewer
 * when the engine's input is full: the caller keeps the rest, takes the
 * events with ssh_engine_next and gives them again).  size 0 says that the
 * connection reached its end.  After the end every byte is taken and
 * dropped.  Returns 0, or ENOMEM (the connection then ends).
 */
int ssh_engine_input(struct ssh_engine *engine, const uint8_t *bytes, size_t size, size_t *taken);

/* Returns the bytes waiting to be written (*size of them; NULL when none). */
const uint8_t *ssh_engine_output(const struct ssh_engine *engine, size_t *size);

/* Removes size written bytes from the output. */
void ssh_engine_written(struct ssh_engine *engine, size_t size);

/*
 * Gives the time: starts a re-exchange when one is due and sends a
 * keepalive when the server has been silent for alive_interval (ending
 * the connection when alive_count went unanswered).  *wake_ms receives the
 * latest time to call again.  Returns 0.
 */
int ssh_engine_tick(struct ssh_engine *engine, uint64_t now_ms, uint64_t *wake_ms);

/*
 * Takes the next event into event.  Returns 0 (an event), or EAGAIN
 * (none until more input, an answer or a tick; also after
 * SSH_ENGINE_CLOSED).
 */
int ssh_engine_next(struct ssh_engine *engine, struct ssh_engine_event *event);

/*
 * Answers SSH_ENGINE_HOST_KEY: accept 1 goes on to the login, 0 ends the
 * connection (HOST_KEY_NOT_VERIFIABLE).  Returns 0, or EINVAL when no
 * host key waits for an answer.
 */
int ssh_engine_host_answer(struct ssh_engine *engine, int accept);

/*
 * Answers SSH_ENGINE_PASSWORD with password (NUL-terminated, at most
 * SSH_PASSWORD_MAX bytes; the engine does not keep it), or with NULL when
 * the user gives up, which ends the passwords.  Returns 0, or EINVAL when
 * no password is asked for or it is too long.
 */
int ssh_engine_password(struct ssh_engine *engine, const char *password);

/*
 * Sends the user's input to the session: *written receives how many of
 * size bytes went (0 before the session started, while the server's
 * window or the output is full, or after ssh_engine_eof).  Returns 0, or
 * EPIPE after the connection ended.
 */
int ssh_engine_write(struct ssh_engine *engine, const uint8_t *bytes, size_t size, size_t *written);

/* Reports size bytes of SSH_ENGINE_DATA used, so that the window may be given back. */
void ssh_engine_consumed(struct ssh_engine *engine, size_t size);

/*
 * Gives the terminal's new size, sent at once when the session has its
 * terminal, kept for the request otherwise.  Returns 0 or EPIPE.
 */
int ssh_engine_resize(struct ssh_engine *engine, unsigned columns, unsigned rows, unsigned width, unsigned height);

/* Says that the user's input ended: the session gets EOF once it runs.  Returns 0 or EPIPE. */
int ssh_engine_eof(struct ssh_engine *engine);

/*
 * Ends the connection at the user's word: queues DISCONNECT
 * (BY_APPLICATION, description) and an SSH_ENGINE_CLOSED with error 0.
 */
void ssh_engine_disconnect(struct ssh_engine *engine, const char *description);

/*
 * Appends the terminal modes of a terminal without its own settings
 * (RFC 4254 8, RFC 8160): the usual control characters (DEL to erase),
 * line editing, echo and signals on, CR to NL on input, NL to CR NL on
 * output, eight bits and UTF-8, 38400 baud, then TTY_OP_END.  Returns 0
 * or the error recorded in out.
 */
int ssh_engine_put_default_modes(struct ssh_buf *out);

/*
 * Decides what to do with a host key from the answers of the user's and
 * the global known_hosts files and the settings (ssh_config(5)
 * StrictHostKeyChecking and BatchMode): a revoked key in either file is
 * refused; a key known in either file is accepted; another key known for
 * the host is refused whatever the settings say; an unknown host is added
 * (no, accept-new), refused (yes, or ask in batch mode) or asked about
 * (ask).  Returns 0 or EINVAL (a value out of its range).
 */
int ssh_engine_host_decide(enum ssh_known_result user_answer, enum ssh_known_result global_answer, enum ssh_strict_host strict, int batch_mode, enum ssh_engine_host_action *action);

#endif
