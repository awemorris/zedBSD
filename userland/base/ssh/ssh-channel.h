/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The connection protocol (RFC 4254): the table of channels of one
 * connection, their flow-control windows, and the messages (global
 * requests, channel open, data, end of data, close, channel requests).
 * What a channel is connected to (a shell, a command, a terminal, the
 * user's standard streams) belongs to the programs.
 *
 * Channel types: "session" (RFC 4254 6) is the only type opened or
 * accepted.  An open of another type ("direct-tcpip", "x11",
 * "auth-agent@openssh.com", ...) is answered with
 * SSH_OPEN_ADMINISTRATIVELY_PROHIBITED.  Global requests are all refused
 * (REQUEST_FAILURE when a reply is wanted), except
 * "no-more-sessions@openssh.com" (OpenSSH PROTOCOL 2.2), which the server
 * honours.
 *
 * Channel requests read (RFC 4254 6.2 to 6.10, OpenSSH PROTOCOL 2.1):
 * pty-req, env, shell, exec, subsystem, window-change, signal,
 * exit-status, exit-signal, eow@openssh.com; any other is read as
 * SSH_CREQ_OTHER (failed when a reply is wanted).
 *
 * Requests that are refused are read only as far as their name and
 * want_reply, and the rest of the payload is not looked at: an unknown or
 * refused global request or channel request may carry any data (OpenSSH's
 * server sends hostkeys-00@openssh.com with key blobs after the login; a
 * client run with -X or -A sends x11-req or auth-agent-req@openssh.com).
 * A request name longer than its field is read as SSH_CREQ_OTHER with
 * the name cut, not as an error, and a known request whose string is
 * longer than its limit below is read as SSH_CREQ_OTHER too, so the
 * program fails that request and keeps the connection.  EINVAL is kept for
 * a payload too short for the recipient, the name and want_reply, or for
 * the fields of a known request that run past the end.
 *
 * Closing (RFC 4254 5.3): after this side sends CLOSE, the peer may still
 * send data, EOF and requests until its own CLOSE crosses ours.  They are
 * read normally on a CLOSING channel (data still takes from the window),
 * and the program drops them and answers a wanted reply with FAILURE.
 *
 * Windows (RFC 4254 5.2): local_window is what the peer may still send
 * here, remote_window what this side may still send.  Data beyond the
 * local window or longer than the announced maximum packet is a protocol
 * error (EPROTO).  A window adjustment that would take the window past
 * 2^32 - 1 is EPROTO (5.2 says the window must not be increased above
 * that).
 */

#ifndef SSH_SSH_CHANNEL_H
#define SSH_SSH_CHANNEL_H

#include "ssh-buf.h"

/* The message numbers (RFC 4250 4.1.2). */
#define SSH_MSG_GLOBAL_REQUEST			80
#define SSH_MSG_REQUEST_SUCCESS			81
#define SSH_MSG_REQUEST_FAILURE			82
#define SSH_MSG_CHANNEL_OPEN			90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION	91
#define SSH_MSG_CHANNEL_OPEN_FAILURE		92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST		93
#define SSH_MSG_CHANNEL_DATA			94
#define SSH_MSG_CHANNEL_EXTENDED_DATA		95
#define SSH_MSG_CHANNEL_EOF			96
#define SSH_MSG_CHANNEL_CLOSE			97
#define SSH_MSG_CHANNEL_REQUEST			98
#define SSH_MSG_CHANNEL_SUCCESS			99
#define SSH_MSG_CHANNEL_FAILURE			100

/* The channel open failure reasons (RFC 4254 5.1). */
#define SSH_OPEN_ADMINISTRATIVELY_PROHIBITED	1
#define SSH_OPEN_CONNECT_FAILED			2
#define SSH_OPEN_UNKNOWN_CHANNEL_TYPE		3
#define SSH_OPEN_RESOURCE_SHORTAGE		4

/* The extended data type of the standard error (RFC 4254 5.2). */
#define SSH_EXTENDED_DATA_STDERR		1

/* The most channels of one connection (OpenSSH's MaxSessions default is 10). */
#define SSH_CHANNEL_MAX			10U

/* The window announced and kept up (2 MiB), and the largest data packet announced. */
#define SSH_CHANNEL_WINDOW		(2U * 1024U * 1024U)
#define SSH_CHANNEL_PACKET		SSH_PAYLOAD_SEND_MAX

/* The longest strings kept from a channel request. */
#define SSH_CHANNEL_TERM_MAX		64U
#define SSH_CHANNEL_ENV_NAME_MAX	64U
#define SSH_CHANNEL_ENV_VALUE_MAX	1024U
#define SSH_CHANNEL_COMMAND_MAX		65536U
#define SSH_CHANNEL_SIGNAL_MAX		32U

/* Where a channel is. */
enum ssh_channel_state {
	/* The slot is unused. */
	SSH_CHANNEL_FREE = 0,

	/* This side sent CHANNEL_OPEN and waits for the answer. */
	SSH_CHANNEL_OPENING = 1,

	/* Open both ways. */
	SSH_CHANNEL_OPEN = 2,

	/* CLOSE was sent or received but not both; the slot is freed when both are. */
	SSH_CHANNEL_CLOSING = 3
};

/*
 * One channel.
 *
 * local_id is this side's number (the slot index), remote_id the peer's.
 * remote_packet is the largest data packet the peer accepts.  The eof_
 * and close_ flags say which of those messages went which way; a channel
 * whose close went both ways is freed by ssh_channel_closed.  user is the
 * program's own state for the channel (not owned here).
 */
struct ssh_channel {
	enum ssh_channel_state state;
	uint32_t local_id;
	uint32_t remote_id;
	uint32_t local_window;
	uint32_t remote_window;
	uint32_t remote_packet;
	int eof_sent;
	int eof_received;
	int close_sent;
	int close_received;
	void *user;
};

/*
 * The channels of one connection.
 *
 * no_more_sessions is 1 after the client's no-more-sessions@openssh.com
 * (a further session open is refused).
 */
struct ssh_channels {
	struct ssh_channel channel[SSH_CHANNEL_MAX];
	int no_more_sessions;
};

/* The channel requests read. */
enum ssh_channel_request_type {
	SSH_CREQ_PTY = 1,
	SSH_CREQ_ENV = 2,
	SSH_CREQ_SHELL = 3,
	SSH_CREQ_EXEC = 4,
	SSH_CREQ_SUBSYSTEM = 5,
	SSH_CREQ_WINDOW_CHANGE = 6,
	SSH_CREQ_SIGNAL = 7,
	SSH_CREQ_EXIT_STATUS = 8,
	SSH_CREQ_EXIT_SIGNAL = 9,
	SSH_CREQ_EOW = 10,
	SSH_CREQ_OTHER = 11
};

/*
 * One CHANNEL_REQUEST as read.
 *
 * recipient is the local channel number; want_reply the boolean.  The
 * fields filled depend on type: pty (term, the four sizes, modes and
 * modes_size pointing into the payload), env (name, value), exec (command
 * and command_size pointing into the payload: a command is bytes, not
 * necessarily text), subsystem (name), window-change (the four sizes),
 * signal (name, without "SIG"), exit-status (status), exit-signal (name,
 * core_dumped, message pointing into the payload).  name holds the
 * request name for SSH_CREQ_OTHER.
 */
struct ssh_channel_request {
	uint32_t recipient;
	enum ssh_channel_request_type type;
	int want_reply;
	char name[SSH_CHANNEL_ENV_NAME_MAX + 1];
	char term[SSH_CHANNEL_TERM_MAX + 1];
	char value[SSH_CHANNEL_ENV_VALUE_MAX + 1];
	uint32_t columns;
	uint32_t rows;
	uint32_t width;
	uint32_t height;
	const uint8_t *modes;
	size_t modes_size;
	const uint8_t *command;
	size_t command_size;
	uint32_t status;
	int core_dumped;
	const uint8_t *message;
	size_t message_size;
};

/* Clears the table: every slot free, no_more_sessions 0. */
void ssh_channels_init(struct ssh_channels *channels);

/*
 * Takes a free slot for a channel this side opens (state OPENING,
 * local_window SSH_CHANNEL_WINDOW) and appends its CHANNEL_OPEN "session"
 * (RFC 4254 6.1) to out.  *channel receives the slot.  Returns 0, EAGAIN
 * (no free slot) or the error recorded in out.
 */
int ssh_channel_open_session(struct ssh_channels *channels, struct ssh_buf *out, struct ssh_channel **channel);

/*
 * Reads a CHANNEL_OPEN from the peer (the server side) and answers it.
 *
 * A "session" open, when a slot is free and no_more_sessions is 0, takes
 * a slot (state OPEN, the peer's id, window and packet size kept; a
 * packet size below 1 is EPROTO), appends OPEN_CONFIRMATION with this
 * side's window and packet size to out, and sets *channel.  Any other
 * type, no free slot, or no_more_sessions appends OPEN_FAILURE with the
 * reason (UNKNOWN_CHANNEL_TYPE is not used: a known but refused type and
 * an unknown one both get ADMINISTRATIVELY_PROHIBITED, so the reply says
 * nothing about what exists) and sets *channel to NULL.
 *
 * Returns 0 (answered either way), EINVAL (malformed), EPROTO, or the
 * error recorded in out.
 */
int ssh_channel_accept_open(struct ssh_channels *channels, const uint8_t *payload, size_t size, struct ssh_buf *out, struct ssh_channel **channel);

/*
 * Reads OPEN_CONFIRMATION or OPEN_FAILURE for a channel this side opened
 * (the client side).  A confirmation moves it to OPEN with the peer's id,
 * window and packet size; a failure frees it and *reason receives the
 * code.  *channel receives the channel.  A recipient that is not OPENING
 * is EPROTO.  Returns 0, EINVAL, EPROTO, or EACCES (the open failed).
 */
int ssh_channel_open_reply(struct ssh_channels *channels, const uint8_t *payload, size_t size, struct ssh_channel **channel, uint32_t *reason);

/*
 * Returns the channel with local number id in state OPEN or CLOSING, or
 * NULL (an unknown or free number: the caller treats it as EPROTO).
 */
struct ssh_channel *ssh_channel_find(struct ssh_channels *channels, uint32_t id);

/*
 * Reads CHANNEL_DATA or CHANNEL_EXTENDED_DATA: *channel, *data and *size
 * (pointing into the payload) and *extended (0 for data, else the type).
 * The size is taken from the channel's local window (also when CLOSING,
 * where the program drops the data).  An unknown recipient, data after the
 * peer's EOF or CLOSE, data beyond the window or longer than
 * SSH_CHANNEL_PACKET is EPROTO.  Returns 0, EINVAL or EPROTO.
 */
int ssh_channel_parse_data(struct ssh_channels *channels, const uint8_t *payload, size_t size, struct ssh_channel **channel, const uint8_t **data, size_t *data_size, uint32_t *extended);

/*
 * Appends CHANNEL_DATA (extended 0) or CHANNEL_EXTENDED_DATA (extended
 * the type) with size bytes and takes size from remote_window.  size must
 * be at most ssh_channel_send_room (EINVAL otherwise); after EOF sent,
 * EPIPE.  Returns 0, EINVAL, EPIPE or the error recorded in out.
 */
int ssh_channel_put_data(struct ssh_channel *channel, uint32_t extended, const uint8_t *data, size_t size, struct ssh_buf *out);

/* Returns how many bytes may be sent in one data message now: the smaller of remote_window and remote_packet (0 when the channel is not OPEN). */
size_t ssh_channel_send_room(const struct ssh_channel *channel);

/*
 * Tells the channel that the program has consumed size received bytes
 * (written them on), and, when the local window has fallen below half of
 * SSH_CHANNEL_WINDOW, appends WINDOW_ADJUST bringing it back to
 * SSH_CHANNEL_WINDOW.  (A program that cannot pass data on yet does not
 * call this, so the peer stops when the window is used up.)
 */
void ssh_channel_consumed(struct ssh_channel *channel, size_t size, struct ssh_buf *out);

/*
 * Reads WINDOW_ADJUST and adds to the channel's remote window.  Unknown
 * recipient or an increase past 2^32 - 1 is EPROTO.  Returns 0, EINVAL or
 * EPROTO; *channel receives the channel.
 */
int ssh_channel_parse_window_adjust(struct ssh_channels *channels, const uint8_t *payload, size_t size, struct ssh_channel **channel);

/*
 * Reads CHANNEL_EOF or CHANNEL_CLOSE (the number is the first byte),
 * setting eof_received or close_received.  A second EOF or CLOSE is
 * EPROTO.  Returns 0, EINVAL or EPROTO; *channel receives the channel.
 */
int ssh_channel_parse_end(struct ssh_channels *channels, const uint8_t *payload, size_t size, struct ssh_channel **channel);

/* Appends CHANNEL_EOF (once; later calls append nothing) and sets eof_sent. */
void ssh_channel_put_eof(struct ssh_channel *channel, struct ssh_buf *out);

/* Appends CHANNEL_CLOSE (once) and sets close_sent; the state becomes CLOSING. */
void ssh_channel_put_close(struct ssh_channel *channel, struct ssh_buf *out);

/*
 * Frees the channel's slot when its close went both ways and returns 1,
 * or returns 0 and leaves it.  user is not touched (the program frees its
 * own state first).
 */
int ssh_channel_closed(struct ssh_channels *channels, struct ssh_channel *channel);

/*
 * Reads a CHANNEL_REQUEST into request (the channel must be OPEN or
 * CLOSING; another recipient is EPROTO).  Malformed framing is EINVAL; a
 * refused, unknown or over-long request is read as described at the top
 * (SSH_CREQ_OTHER).  Returns 0, EINVAL or EPROTO.
 */
int ssh_channel_parse_request(struct ssh_channels *channels, const uint8_t *payload, size_t size, struct ssh_channel_request *request);

/* Appends CHANNEL_SUCCESS (success 1) or CHANNEL_FAILURE (0) for the channel's remote id. */
void ssh_channel_put_reply(const struct ssh_channel *channel, int success, struct ssh_buf *out);

/*
 * Reads CHANNEL_SUCCESS or CHANNEL_FAILURE; *channel and *success
 * receive the channel and 1 or 0.  Returns 0, EINVAL or EPROTO.
 */
int ssh_channel_parse_reply(struct ssh_channels *channels, const uint8_t *payload, size_t size, struct ssh_channel **channel, int *success);

/*
 * Appends a "pty-req" (RFC 4254 6.2): term, columns, rows, pixel width
 * and height, and the encoded modes (modes_size bytes, ssh-ttymodes.h)
 * as a string; want_reply TRUE.
 */
void ssh_channel_put_pty(const struct ssh_channel *channel, const char *term, uint32_t columns, uint32_t rows, uint32_t width, uint32_t height, const uint8_t *modes, size_t modes_size, struct ssh_buf *out);

/* Appends an "env" request (RFC 4254 6.4), want_reply FALSE. */
void ssh_channel_put_env(const struct ssh_channel *channel, const char *name, const char *value, struct ssh_buf *out);

/* Appends a "shell" request (6.5), want_reply TRUE. */
void ssh_channel_put_shell(const struct ssh_channel *channel, struct ssh_buf *out);

/* Appends an "exec" request with size bytes of command (6.5), want_reply TRUE. */
void ssh_channel_put_exec(const struct ssh_channel *channel, const uint8_t *command, size_t size, struct ssh_buf *out);

/* Appends a "subsystem" request (6.5), want_reply TRUE. */
void ssh_channel_put_subsystem(const struct ssh_channel *channel, const char *name, struct ssh_buf *out);

/* Appends a "window-change" request (6.7), want_reply FALSE. */
void ssh_channel_put_window_change(const struct ssh_channel *channel, uint32_t columns, uint32_t rows, uint32_t width, uint32_t height, struct ssh_buf *out);

/* Appends an "exit-status" request (6.10), want_reply FALSE. */
void ssh_channel_put_exit_status(const struct ssh_channel *channel, uint32_t status, struct ssh_buf *out);

/*
 * Appends an "exit-signal" request (6.10): the signal name without "SIG"
 * (one of the names of 6.10, e.g. "TERM"), core dumped, an empty message
 * and an empty language tag; want_reply FALSE.
 */
void ssh_channel_put_exit_signal(const struct ssh_channel *channel, const char *name, int core_dumped, struct ssh_buf *out);

/*
 * Reads a GLOBAL_REQUEST: name (capacity SSH_NAME_MAX + 1; a longer name
 * is cut) and *want_reply; the data after them is not read (see the top).
 * Sets channels->no_more_sessions for no-more-sessions@openssh.com.
 * When a reply is wanted, appends REQUEST_FAILURE (every global request is
 * refused) to out.  Returns 0, EINVAL (a payload too short for the name
 * and want_reply) or the error recorded in out.
 */
int ssh_channel_global_request(struct ssh_channels *channels, const uint8_t *payload, size_t size, char *name, int *want_reply, struct ssh_buf *out);

/* Appends the client's GLOBAL_REQUEST "no-more-sessions@openssh.com", want_reply FALSE. */
void ssh_channel_put_no_more_sessions(struct ssh_buf *out);

/*
 * Appends a GLOBAL_REQUEST with name and want_reply and no data (the
 * keepalive@openssh.com of ServerAliveInterval and ClientAliveInterval,
 * want_reply TRUE: the peer's REQUEST_SUCCESS or REQUEST_FAILURE shows it
 * is alive).
 */
void ssh_channel_put_global_request(const char *name, int want_reply, struct ssh_buf *out);

#endif
