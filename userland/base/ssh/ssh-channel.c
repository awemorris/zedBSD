/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The connection protocol: channels, windows and their messages.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-channel.h"

/*
 * Clears the table: every slot free, no_more_sessions 0.
 */
void
ssh_channels_init(
	struct ssh_channels *channels)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channels;
}

/*
 * Takes a free slot for a channel this side opens (state OPENING,
 * local_window SSH_CHANNEL_WINDOW) and appends its CHANNEL_OPEN
 * "session" (RFC 4254 6.1) to out.
 */
int
ssh_channel_open_session(
	struct ssh_channels *channels,
	struct ssh_buf *out,
	struct ssh_channel **channel)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channels;
	(void)out;
	(void)channel;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads a CHANNEL_OPEN from the peer (the server side) and answers it.
 */
int
ssh_channel_accept_open(
	struct ssh_channels *channels,
	const uint8_t *payload,
	size_t size,
	struct ssh_buf *out,
	struct ssh_channel **channel)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channels;
	(void)payload;
	(void)size;
	(void)out;
	(void)channel;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads OPEN_CONFIRMATION or OPEN_FAILURE for a channel this side opened
 * (the client side).
 */
int
ssh_channel_open_reply(
	struct ssh_channels *channels,
	const uint8_t *payload,
	size_t size,
	struct ssh_channel **channel,
	uint32_t *reason)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channels;
	(void)payload;
	(void)size;
	(void)channel;
	(void)reason;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Returns the channel with local number id in state OPEN or CLOSING, or
 * NULL (an unknown or free number: the caller treats it as EPROTO).
 */
struct ssh_channel *
ssh_channel_find(
	struct ssh_channels *channels,
	uint32_t id)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channels;
	(void)id;

	/* Reports that the function is not implemented yet. */
	return NULL;
}

/*
 * Reads CHANNEL_DATA or CHANNEL_EXTENDED_DATA: *channel, *data and *size
 * (pointing into the payload) and *extended (0 for data, else the type).
 */
int
ssh_channel_parse_data(
	struct ssh_channels *channels,
	const uint8_t *payload,
	size_t size,
	struct ssh_channel **channel,
	const uint8_t **data,
	size_t *data_size,
	uint32_t *extended)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channels;
	(void)payload;
	(void)size;
	(void)channel;
	(void)data;
	(void)data_size;
	(void)extended;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Appends CHANNEL_DATA (extended 0) or CHANNEL_EXTENDED_DATA (extended
 * the type) with size bytes and takes size from remote_window.
 */
int
ssh_channel_put_data(
	struct ssh_channel *channel,
	uint32_t extended,
	const uint8_t *data,
	size_t size,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;
	(void)extended;
	(void)data;
	(void)size;
	(void)out;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Returns how many bytes may be sent in one data message now: the
 * smaller of remote_window and remote_packet (0 when the channel is not
 * OPEN).
 */
size_t
ssh_channel_send_room(
	const struct ssh_channel *channel)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;

	/* Reports that the function is not implemented yet. */
	return 0;
}

/*
 * Tells the channel that the program has consumed size received bytes
 * (written them on), and, when the local window has fallen below half of
 * SSH_CHANNEL_WINDOW, appends WINDOW_ADJUST bringing it back to
 * SSH_CHANNEL_WINDOW.
 */
void
ssh_channel_consumed(
	struct ssh_channel *channel,
	size_t size,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;
	(void)size;
	(void)out;
}

/*
 * Reads WINDOW_ADJUST and adds to the channel's remote window.
 */
int
ssh_channel_parse_window_adjust(
	struct ssh_channels *channels,
	const uint8_t *payload,
	size_t size,
	struct ssh_channel **channel)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channels;
	(void)payload;
	(void)size;
	(void)channel;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads CHANNEL_EOF or CHANNEL_CLOSE (the number is the first byte),
 * setting eof_received or close_received.
 */
int
ssh_channel_parse_end(
	struct ssh_channels *channels,
	const uint8_t *payload,
	size_t size,
	struct ssh_channel **channel)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channels;
	(void)payload;
	(void)size;
	(void)channel;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Appends CHANNEL_EOF (once; later calls append nothing) and sets
 * eof_sent.
 */
void
ssh_channel_put_eof(
	struct ssh_channel *channel,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;
	(void)out;
}

/*
 * Appends CHANNEL_CLOSE (once) and sets close_sent; the state becomes
 * CLOSING.
 */
void
ssh_channel_put_close(
	struct ssh_channel *channel,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;
	(void)out;
}

/*
 * Frees the channel's slot when its close went both ways and returns 1,
 * or returns 0 and leaves it.
 */
int
ssh_channel_closed(
	struct ssh_channels *channels,
	struct ssh_channel *channel)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channels;
	(void)channel;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads a CHANNEL_REQUEST into request (the channel must be OPEN or
 * CLOSING; another recipient is EPROTO).
 */
int
ssh_channel_parse_request(
	struct ssh_channels *channels,
	const uint8_t *payload,
	size_t size,
	struct ssh_channel_request *request)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channels;
	(void)payload;
	(void)size;
	(void)request;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Appends CHANNEL_SUCCESS (success 1) or CHANNEL_FAILURE (0) for the
 * channel's remote id.
 */
void
ssh_channel_put_reply(
	const struct ssh_channel *channel,
	int success,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;
	(void)success;
	(void)out;
}

/*
 * Reads CHANNEL_SUCCESS or CHANNEL_FAILURE; *channel and *success
 * receive the channel and 1 or 0.
 */
int
ssh_channel_parse_reply(
	struct ssh_channels *channels,
	const uint8_t *payload,
	size_t size,
	struct ssh_channel **channel,
	int *success)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channels;
	(void)payload;
	(void)size;
	(void)channel;
	(void)success;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Appends a "pty-req" (RFC 4254 6.2): term, columns, rows, pixel width
 * and height, and the encoded modes (modes_size bytes, ssh-ttymodes.h)
 * as a string; want_reply TRUE.
 */
void
ssh_channel_put_pty(
	const struct ssh_channel *channel,
	const char *term,
	uint32_t columns,
	uint32_t rows,
	uint32_t width,
	uint32_t height,
	const uint8_t *modes,
	size_t modes_size,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;
	(void)term;
	(void)columns;
	(void)rows;
	(void)width;
	(void)height;
	(void)modes;
	(void)modes_size;
	(void)out;
}

/*
 * Appends an "env" request (RFC 4254 6.4), want_reply FALSE.
 */
void
ssh_channel_put_env(
	const struct ssh_channel *channel,
	const char *name,
	const char *value,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;
	(void)name;
	(void)value;
	(void)out;
}

/*
 * Appends a "shell" request (6.5), want_reply TRUE.
 */
void
ssh_channel_put_shell(
	const struct ssh_channel *channel,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;
	(void)out;
}

/*
 * Appends an "exec" request with size bytes of command (6.5), want_reply
 * TRUE.
 */
void
ssh_channel_put_exec(
	const struct ssh_channel *channel,
	const uint8_t *command,
	size_t size,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;
	(void)command;
	(void)size;
	(void)out;
}

/*
 * Appends a "subsystem" request (6.5), want_reply TRUE.
 */
void
ssh_channel_put_subsystem(
	const struct ssh_channel *channel,
	const char *name,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;
	(void)name;
	(void)out;
}

/*
 * Appends a "window-change" request (6.7), want_reply FALSE.
 */
void
ssh_channel_put_window_change(
	const struct ssh_channel *channel,
	uint32_t columns,
	uint32_t rows,
	uint32_t width,
	uint32_t height,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;
	(void)columns;
	(void)rows;
	(void)width;
	(void)height;
	(void)out;
}

/*
 * Appends an "exit-status" request (6.10), want_reply FALSE.
 */
void
ssh_channel_put_exit_status(
	const struct ssh_channel *channel,
	uint32_t status,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;
	(void)status;
	(void)out;
}

/*
 * Appends an "exit-signal" request (6.10): the signal name without "SIG"
 * (one of the names of 6.10, e.g.
 */
void
ssh_channel_put_exit_signal(
	const struct ssh_channel *channel,
	const char *name,
	int core_dumped,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channel;
	(void)name;
	(void)core_dumped;
	(void)out;
}

/*
 * Reads a GLOBAL_REQUEST: name (capacity SSH_NAME_MAX + 1; a longer name
 * is cut) and *want_reply; the data after them is not read (see the
 * top).
 */
int
ssh_channel_global_request(
	struct ssh_channels *channels,
	const uint8_t *payload,
	size_t size,
	char *name,
	int *want_reply,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)channels;
	(void)payload;
	(void)size;
	(void)name;
	(void)want_reply;
	(void)out;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Appends the client's GLOBAL_REQUEST "no-more-sessions@openssh.com",
 * want_reply FALSE.
 */
void
ssh_channel_put_no_more_sessions(
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)out;
}

/*
 * Appends a GLOBAL_REQUEST with name and want_reply and no data (the
 * keepalive@openssh.com of ServerAliveInterval and ClientAliveInterval,
 * want_reply TRUE: the peer's REQUEST_SUCCESS or REQUEST_FAILURE shows
 * it is alive).
 */
void
ssh_channel_put_global_request(
	const char *name,
	int want_reply,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)name;
	(void)want_reply;
	(void)out;
}
