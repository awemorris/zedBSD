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
 * channel (RFC 4254 6) with or without a pseudo-terminal, and the X11
 * connections the server forwards on it (RFC 4254 6.3).
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
 * The user's keys are ssh-ed25519 private key files (openssh-key-v1)
 * given as their bytes, in the clear or encrypted as ssh-keygen writes
 * them with a passphrase; a file that cannot be used is passed over.  An
 * encrypted key is offered by its public half, which the file keeps in
 * the clear, and its passphrase is asked for (SSH_ENGINE_PASSPHRASE) only
 * when the server would take the key.  The engine keeps no passphrase.
 *
 * The received data of the session is handed out as it comes
 * (SSH_ENGINE_DATA); the channel's window is given back only while the
 * caller holds less than half a window of it not yet reported used with
 * ssh_engine_consumed, so the server stops when the caller falls behind
 * and the caller never holds more than one and a half windows
 * (SSH_CHANNEL_WINDOW).
 *
 * X11 forwarding (options x11): the session asks for it with x11-req
 * after its terminal and before the shell or the command, giving the
 * caller's fake MIT-MAGIC-COOKIE-1, which the server hands its X clients
 * (through xauth) and the caller's X server checks.  The answer comes as
 * SSH_ENGINE_X11_REPLY; a refusal ends nothing.  Once the server accepted
 * it, each "x11" channel the server opens for an X client is shown as
 * SSH_ENGINE_X11_OPEN and waits for ssh_engine_x11_answer; before that
 * answer, on a connection that did not ask, and after the session channel
 * closed, the open is refused (administratively prohibited).  An X11
 * channel carries its bytes both ways with the channel calls below
 * (ssh_engine_channel_write, ssh_engine_channel_consumed,
 * ssh_engine_channel_eof, ssh_engine_channel_close); its window is
 * SSH_ENGINE_X11_WINDOW and, as for the session, given back only for what
 * the caller reported used, so a caller that passes the bytes on to an X
 * server reports them as the server takes them and the X client stops
 * when the server falls behind.  When the session channel closes while
 * X11 channels are open, the connection stays until the last of them
 * closed (as OpenSSH's client waits for forwarded connections to end).
 *
 * Port forwarding (RFC 4254 7): the engine owns no socket.  The caller
 * listens, accepts and connects, and the engine carries each forwarded
 * connection on a channel of its own.  For a local port (ssh -L, -D) the
 * caller opens a "direct-tcpip" channel with ssh_engine_open_direct and
 * hears whether it opened with SSH_ENGINE_OPEN_REPLY.  For a remote port
 * (ssh -R) the caller asks the server to listen with
 * ssh_engine_forward_listen and hears the answer with
 * SSH_ENGINE_FORWARD_REPLY; each "forwarded-tcpip" channel the server then
 * opens for a connection to that port is shown as SSH_ENGINE_FORWARD_OPEN
 * (an open for an address and port not asked for is refused) and waits
 * for ssh_engine_open_answer.  A forwarded channel carries its bytes with
 * the same channel calls as an X11 channel, with a window of
 * SSH_ENGINE_FORWARD_WINDOW given back only for what the caller reported
 * used.  A connection keeps at most 32 X11 channels and
 * SSH_ENGINE_FORWARD_CHANNELS_MAX forwarded ones (32 when it asks for X11),
 * and its session keeps channel number 0 even when it has none (options
 * session 0), so no forwarded channel takes the session's number.  The
 * channels opened before the session closed keep the connection after
 * it, as X11 channels do; a connection without a session lasts until the
 * caller or the server ends it.
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

/* How many times the passphrase of one key is asked for before the key is passed over (as many as the passwords). */
#define SSH_ENGINE_PASSPHRASE_TRIES	3U

/* The longest name-list of methods kept from USERAUTH_FAILURE. */
#define SSH_ENGINE_METHODS_MAX		1024U

/* The longest text of an event's reason. */
#define SSH_ENGINE_REASON_MAX		(SSH_TRANSPORT_REASON_MAX + SSH_ENGINE_METHODS_MAX)

/* The number of the session channel in the channel calls and events (the first channel opened). */
#define SSH_ENGINE_SESSION_CHANNEL	0U

/* The window of an X11 channel (128 KiB): what the caller holds of one is at most one and a half of it. */
#define SSH_ENGINE_X11_WINDOW		(128U * 1024U)

/* The length of the fake MIT-MAGIC-COOKIE-1 in hexadecimal digits (16 bytes). */
#define SSH_ENGINE_X11_COOKIE_DIGITS	32U

/* The longest originator address of an X11 or forwarded channel kept for its event (a longer one is cut). */
#define SSH_ENGINE_ADDRESS_MAX		64U

/* The window of a forwarded TCP channel (512 KiB): what the caller holds of one is at most one and a half of it. */
#define SSH_ENGINE_FORWARD_WINDOW	(512U * 1024U)

/* The most remote forwardings (tcpip-forward) one connection asks for. */
#define SSH_ENGINE_REMOTE_FORWARDS_MAX	16U

/* The most forwarded TCP channels open at once on a connection that does not ask for X11 (32 on one that does). */
#define SSH_ENGINE_FORWARD_CHANNELS_MAX	64U

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
	 * Data of a channel (channel: SSH_ENGINE_SESSION_CHANNEL, an X11 or a
	 * forwarded channel; bytes, size; stream 0 for the output, 1 for the session's
	 * error output).  The bytes stay valid until the next ssh_engine_next
	 * or ssh_engine_close; the caller reports them used with
	 * ssh_engine_consumed (the session) or ssh_engine_channel_consumed.
	 */
	SSH_ENGINE_DATA = 6,

	/*
	 * The session channel closed: exit_status is the command's status (-1
	 * when the server sent none), exit_signal the signal that ended it
	 * (empty when none), reason why the session failed (empty when it did
	 * not).  The connection then ends with SSH_ENGINE_CLOSED, at once, or
	 * when the last X11 or forwarded channel closed while any is open.
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
	 * side sent).  tries is how many passwords the server refused.  Only
	 * the pending output remains.
	 */
	SSH_ENGINE_CLOSED = 8,

	/*
	 * The server would take the key of an encrypted identity file
	 * (identity, its place in the options' identities); tries is how many
	 * passphrases did not open the file before.  The caller answers with
	 * ssh_engine_passphrase.
	 */
	SSH_ENGINE_PASSPHRASE = 9,

	/*
	 * The server answered x11-req: accepted 1, or 0 with reason the line
	 * OpenSSH's client shows ("X11 forwarding request failed on channel
	 * 0"); the session goes on either way.
	 */
	SSH_ENGINE_X11_REPLY = 10,

	/*
	 * The server opens an X11 channel (channel) for an X client at
	 * address and port (the originator, RFC 4254 6.3.2: text made safe,
	 * cut to SSH_ENGINE_ADDRESS_MAX).  The caller answers with
	 * ssh_engine_x11_answer; until then the channel carries nothing.
	 */
	SSH_ENGINE_X11_OPEN = 11,

	/* The server sends no more data on an X11 or forwarded channel (channel). */
	SSH_ENGINE_CHANNEL_EOF = 12,

	/*
	 * An X11 or forwarded channel (channel) closed both ways, or a
	 * direct-tcpip channel the caller closed before its answer ended; its
	 * number may be given to a later channel.  Bytes of it not yet
	 * reported used no longer need reporting.
	 */
	SSH_ENGINE_CHANNEL_CLOSED = 13,

	/*
	 * The server answered ssh_engine_forward_listen for forward: accepted
	 * 1 with port the port it listens on (the one asked for, or the one
	 * it chose for 0), or accepted 0 with port the one asked for.
	 */
	SSH_ENGINE_FORWARD_REPLY = 14,

	/*
	 * The server opens a forwarded-tcpip channel (channel) for a
	 * connection to the port of forward: listen_address and listen_port
	 * are the address asked for and the port listened on, address and
	 * port the originator (text made safe, cut to
	 * SSH_ENGINE_ADDRESS_MAX).  The caller answers with
	 * ssh_engine_open_answer; until then the channel carries nothing.
	 */
	SSH_ENGINE_FORWARD_OPEN = 15,

	/*
	 * The server answered ssh_engine_open_direct for channel: accepted 1
	 * (the channel is open), or accepted 0 with code the reason (RFC 4254
	 * 5.1) and reason its words and the server's description, as
	 * OpenSSH's client shows them after "open failed: " (text made safe);
	 * the number is then free.
	 */
	SSH_ENGINE_OPEN_REPLY = 16
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
	uint32_t channel;
	const uint8_t *bytes;
	size_t size;
	unsigned stream;
	unsigned tries;
	unsigned identity;
	int pty;
	int exit_status;
	char exit_signal[SSH_CHANNEL_SIGNAL_MAX + 1];
	int error;
	uint32_t code;
	char reason[SSH_ENGINE_REASON_MAX];
	int accepted;
	char address[SSH_ENGINE_ADDRESS_MAX + 1];
	unsigned port;
	unsigned forward;
	char listen_address[SSH_CONFIG_HOST_MAX + 1];
	unsigned listen_port;
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
 * clock in milliseconds.  x11 1 asks for X11 forwarding on the session
 * (session must be 1) with x11_cookie, the fake MIT-MAGIC-COOKIE-1 as
 * SSH_ENGINE_X11_COOKIE_DIGITS hexadecimal digits; x11_single 1 asks the
 * server to forward one X client only (RFC 4254 6.3.1 "single
 * connection"; OpenSSH's client asks for any number, 0) and refuses a
 * second X11 channel.
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
	int x11;
	const char *x11_cookie;
	int x11_single;
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
 * Answers SSH_ENGINE_PASSPHRASE for identity with passphrase
 * (NUL-terminated, at most SSH_KEY_PASSPHRASE_MAX bytes; the engine does
 * not keep it).  The file is decrypted at once, which takes a fraction of
 * a second for OpenSSH's default rounds: a passphrase that opens it sends
 * the signed request; a wrong one asks again (another
 * SSH_ENGINE_PASSPHRASE) until SSH_ENGINE_PASSPHRASE_TRIES were wrong,
 * and then, as for NULL or an empty passphrase (the user gives up), and
 * for a file that is broken under its encryption, the key is passed over
 * and the next method follows.  Returns 0 (the passphrase opened the file,
 * or the user gave up), EACCES (the passphrase did not open the file: the
 * answer is taken as above, and a caller that kept the passphrase should
 * forget it), or EINVAL when no passphrase is asked for identity or it is
 * too long.
 */
int ssh_engine_passphrase(struct ssh_engine *engine, unsigned identity, const char *passphrase);

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
 * Answers SSH_ENGINE_X11_OPEN for channel, as ssh_engine_open_answer
 * does (the window of an X11 channel is SSH_ENGINE_X11_WINDOW).
 */
int ssh_engine_x11_answer(struct ssh_engine *engine, uint32_t channel, int accept);

/*
 * Answers SSH_ENGINE_X11_OPEN or SSH_ENGINE_FORWARD_OPEN for channel:
 * accept 1 opens it, 0 refuses it (connect failed) and frees its number.
 * Returns 0, EINVAL when channel waits for no answer, or EPIPE after the
 * connection ended.
 */
int ssh_engine_open_answer(struct ssh_engine *engine, uint32_t channel, int accept);

/*
 * Asks the server to listen on address and port for connections to
 * forward here (tcpip-forward, RFC 4254 7.1; address as OpenSSH's ssh
 * sends it: "localhost" for no bind address, "" for "" or "*"; port 0
 * for the server's choice).  Allowed once the user is authenticated.
 * *forward receives the number the reply (SSH_ENGINE_FORWARD_REPLY) and
 * the channels (SSH_ENGINE_FORWARD_OPEN) name it by; channels for a port
 * asked for are taken from the moment of asking (as OpenSSH's client
 * does), for port 0 once the server told the port it chose.  Returns 0,
 * EINVAL (before the login, an address longer than SSH_CONFIG_HOST_MAX
 * or with an unprintable byte, a port above 65535), EAGAIN
 * (SSH_ENGINE_REMOTE_FORWARDS_MAX asked already) or EPIPE.
 */
int ssh_engine_forward_listen(struct ssh_engine *engine, const char *address, unsigned port, unsigned *forward);

/*
 * Opens a direct-tcpip channel (RFC 4254 7.2) to host and port for a
 * connection that came from origin and origin_port.  Allowed once the
 * user is authenticated and until the session channel closed (on a
 * connection without a session, until it ends).  *channel receives its
 * number; SSH_ENGINE_OPEN_REPLY tells whether it opened, and nothing is
 * sent on it before.  Returns 0, EINVAL (not allowed now; host empty or
 * longer than SSH_CONFIG_HOST_MAX, origin longer, a port out of range or
 * an unprintable byte), EAGAIN (SSH_ENGINE_FORWARD_CHANNELS_MAX open, or
 * no free channel) or EPIPE.
 */
int ssh_engine_open_direct(struct ssh_engine *engine, const char *host, unsigned port, const char *origin, unsigned origin_port, uint32_t *channel);

/*
 * Sends bytes on a channel: SSH_ENGINE_SESSION_CHANNEL as
 * ssh_engine_write, or an open X11 or forwarded channel; *written receives how many of
 * size bytes went (0 while the server's window or the output is full, and
 * after this side's EOF or close or the server's close).  Returns 0,
 * EINVAL (no such channel), or EPIPE after the connection ended.
 */
int ssh_engine_channel_write(struct ssh_engine *engine, uint32_t channel, const uint8_t *bytes, size_t size, size_t *written);

/*
 * Reports size bytes of SSH_ENGINE_DATA of channel used, as
 * ssh_engine_consumed does for the session: the window is given back only
 * for what was reported.  A channel no longer open is ignored.
 */
void ssh_engine_channel_consumed(struct ssh_engine *engine, uint32_t channel, size_t size);

/*
 * Says that this side sends no more data on channel
 * (SSH_ENGINE_SESSION_CHANNEL as ssh_engine_eof).  Returns 0, EINVAL (no
 * such channel) or EPIPE.
 */
int ssh_engine_channel_eof(struct ssh_engine *engine, uint32_t channel);

/*
 * Closes an X11 or forwarded channel (not the session's): the server's
 * close coming back ends it with SSH_ENGINE_CHANNEL_CLOSED, and data
 * arriving before that is dropped.  A direct-tcpip channel still waiting
 * for its answer is closed as soon as the server confirms it (or ends at
 * its refusal), with no SSH_ENGINE_OPEN_REPLY but the
 * SSH_ENGINE_CHANNEL_CLOSED at the end.  A channel waiting for the
 * caller's answer is refused with ssh_engine_open_answer instead.
 * Returns 0, EINVAL (not such a channel) or EPIPE.
 */
int ssh_engine_channel_close(struct ssh_engine *engine, uint32_t channel);

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
 * Reports whether the server's identification line (RFC 4253 4.2) was
 * read: 1 once it was, 0 before.
 */
int ssh_engine_identified(const struct ssh_engine *engine);

/* Reports how many later exchanges showed the first exchange's host key again. */
unsigned long ssh_engine_rekeys(const struct ssh_engine *engine);

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
