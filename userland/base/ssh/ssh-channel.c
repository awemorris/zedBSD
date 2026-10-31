/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The connection protocol: channels, windows and their messages.
 *
 * The functions that read a message take the whole payload, message number
 * first, check its framing with an ssh_reader, and only then look at the
 * state of the channel it names, so a malformed message changes nothing.
 */

#include <string.h>

#include "ssh-channel.h"

/* The channel type that is opened and accepted (RFC 4254 6.1). */
#define CHANNEL_TYPE_SESSION		"session"

/* The global request that closes the door to further sessions (OpenSSH PROTOCOL 2.2). */
#define GLOBAL_NO_MORE_SESSIONS		"no-more-sessions@openssh.com"

/* The local window level below which consumed bytes are given back to the peer: half of the full window. */
#define CHANNEL_WINDOW_HALF		(SSH_CHANNEL_WINDOW / 2U)

/* The most a window may hold: 2^32 - 1 (RFC 4254 5.2). */
#define CHANNEL_WINDOW_LIMIT		0xffffffffU

/* The text sent with a channel open failure for each reason; the language tag is empty. */
#define OPEN_FAILURE_PROHIBITED_TEXT	"administratively prohibited"
#define OPEN_FAILURE_SHORTAGE_TEXT	"resource shortage"

/* One row of the table of request names: the name on the wire and the type it stands for. */
struct request_name {
	const char *text;
	enum ssh_channel_request_type type;
};

/*
 * The request names this module reads, matched against the whole name the
 * peer sent.
 *
 * Values referenced:
 *   RFC 4254 6.2 "pty-req", 6.4 "env", 6.5 "shell", "exec" and
 *   "subsystem", 6.7 "window-change", 6.9 "signal", 6.10 "exit-status"
 *   and "exit-signal";
 *   OpenSSH PROTOCOL 2.1 "eow@openssh.com".
 * The last row, with no text, ends the table.
 */
static const struct request_name request_names[] = {
	{ "pty-req", SSH_CREQ_PTY },
	{ "env", SSH_CREQ_ENV },
	{ "shell", SSH_CREQ_SHELL },
	{ "exec", SSH_CREQ_EXEC },
	{ "subsystem", SSH_CREQ_SUBSYSTEM },
	{ "window-change", SSH_CREQ_WINDOW_CHANGE },
	{ "signal", SSH_CREQ_SIGNAL },
	{ "exit-status", SSH_CREQ_EXIT_STATUS },
	{ "exit-signal", SSH_CREQ_EXIT_SIGNAL },
	{ "eow@openssh.com", SSH_CREQ_EOW },
	{ NULL, SSH_CREQ_OTHER }
};

static struct ssh_channel *take_free_slot(struct ssh_channels *channels);
static void reset_slot(struct ssh_channel *channel);
static int slot_in_table(const struct ssh_channels *channels, const struct ssh_channel *channel);
static int read_message_number(struct ssh_reader *reader, unsigned first, unsigned second, unsigned *message);
static void put_open_failure(struct ssh_buf *out, uint32_t recipient, uint32_t reason, const char *text);
static void record_error(struct ssh_buf *out, int error);
static int name_is(const uint8_t *name, size_t size, const char *text);
static enum ssh_channel_request_type find_request_type(const uint8_t *name, size_t size);
static void copy_name_cut(char *out, size_t capacity, const uint8_t *bytes, size_t size);
static int copy_text(char *out, size_t limit, const uint8_t *bytes, size_t size);
static void copy_to_name(struct ssh_channel_request *request, const char *text);
static int put_request_start(const struct ssh_channel *channel, const char *name, int want_reply, struct ssh_buf *out);
static int parse_request_pty(struct ssh_reader *reader, struct ssh_channel_request *request);
static int parse_request_env(struct ssh_reader *reader, struct ssh_channel_request *request);
static int parse_request_exec(struct ssh_reader *reader, struct ssh_channel_request *request);
static int parse_request_subsystem(struct ssh_reader *reader, struct ssh_channel_request *request);
static int parse_request_window_change(struct ssh_reader *reader, struct ssh_channel_request *request);
static int parse_request_signal(struct ssh_reader *reader, struct ssh_channel_request *request);
static int parse_request_exit_status(struct ssh_reader *reader, struct ssh_channel_request *request);
static int parse_request_exit_signal(struct ssh_reader *reader, struct ssh_channel_request *request);

/*
 * Clears the table: every slot free, no_more_sessions 0.
 */
void
ssh_channels_init(
	struct ssh_channels *channels)
{
	unsigned index;

	/* Frees every slot and gives it its number. */
	for (index = 0;
	     index < SSH_CHANNEL_MAX;
	     index++) {
		channels->channel[index].local_id = index;
		reset_slot(&channels->channel[index]);
		channels->channel[index].user = NULL;
	}

	/* Allows sessions until the peer says it will open no more. */
	channels->no_more_sessions = 0;
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
	struct ssh_channel *slot;
	int error;

	*channel = NULL;

	/* Does nothing when the output has already failed. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Reports that every slot is in use. */
	slot = take_free_slot(channels);
	if (slot == NULL)
		return EAGAIN;

	/* Starts the channel: waiting for the peer's answer, with the whole window offered. */
	slot->state = SSH_CHANNEL_OPENING;
	slot->local_window = SSH_CHANNEL_WINDOW;
	slot->user = NULL;

	/* Writes the open request: type, our number, the window and the largest data packet we accept. */
	ssh_buf_put_u8(out, SSH_MSG_CHANNEL_OPEN);
	ssh_buf_put_cstring(out, CHANNEL_TYPE_SESSION);
	ssh_buf_put_u32(out, slot->local_id);
	ssh_buf_put_u32(out, SSH_CHANNEL_WINDOW);
	ssh_buf_put_u32(out, SSH_CHANNEL_PACKET);

	/* Gives the slot back when the request could not be queued. */
	error = ssh_buf_error(out);
	if (error != 0) {
		reset_slot(slot);
		return error;
	}

	/* Succeeded: the open request is queued and the channel waits for the answer. */
	*channel = slot;
	return 0;
}

/*
 * Reads a CHANNEL_OPEN from the peer (the server side) and answers it.
 *
 * Whatever the type, the sender's number, window and packet size are read
 * first, because the failure answer needs the number; the data that
 * follows them belongs to the type and is not read.
 */
int
ssh_channel_accept_open(
	struct ssh_channels *channels,
	const uint8_t *payload,
	size_t size,
	struct ssh_buf *out,
	struct ssh_channel **channel)
{
	struct ssh_reader reader;
	struct ssh_channel *slot;
	const uint8_t *type;
	size_t type_size;
	unsigned message;
	uint32_t sender;
	uint32_t window;
	uint32_t packet;
	int is_session;
	int error;

	*channel = NULL;

	/* Reads the framing: message number, type, sender, window, packet size. */
	ssh_reader_init(&reader, payload, size);
	error = read_message_number(&reader, SSH_MSG_CHANNEL_OPEN, 0, &message);
	if (error != 0)
		return error;
	ssh_reader_get_string(&reader, &type, &type_size);
	sender = ssh_reader_get_u32(&reader);
	window = ssh_reader_get_u32(&reader);
	packet = ssh_reader_get_u32(&reader);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;

	/* Does nothing more when the output has already failed. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* A session is the only type taken; every other type gets the same refusal, so the answer reveals nothing. */
	is_session = name_is(type, type_size, CHANNEL_TYPE_SESSION);
	if (is_session == 0) {
		put_open_failure(out, sender, SSH_OPEN_ADMINISTRATIVELY_PROHIBITED, OPEN_FAILURE_PROHIBITED_TEXT);
		error = ssh_buf_error(out);
		if (error != 0)
			return error;
		return 0;
	}

	/* A peer that accepts no data at all cannot be sent anything: a protocol fault. */
	if (packet < 1)
		return EPROTO;

	/* Refuses a session after the peer said it would open no more. */
	if (channels->no_more_sessions != 0) {
		put_open_failure(out, sender, SSH_OPEN_ADMINISTRATIVELY_PROHIBITED, OPEN_FAILURE_PROHIBITED_TEXT);
		error = ssh_buf_error(out);
		if (error != 0)
			return error;
		return 0;
	}

	/* Refuses a session when every slot is in use. */
	slot = take_free_slot(channels);
	if (slot == NULL) {
		put_open_failure(out, sender, SSH_OPEN_RESOURCE_SHORTAGE, OPEN_FAILURE_SHORTAGE_TEXT);
		error = ssh_buf_error(out);
		if (error != 0)
			return error;
		return 0;
	}

	/*
	 * Opens the channel.  The largest data packet this side sends is the
	 * smaller of the peer's limit and the largest packet these programs send.
	 */
	slot->state = SSH_CHANNEL_OPEN;
	slot->remote_id = sender;
	slot->local_window = SSH_CHANNEL_WINDOW;
	slot->remote_window = window;
	slot->remote_packet = packet;
	if (slot->remote_packet > SSH_CHANNEL_PACKET)
		slot->remote_packet = SSH_CHANNEL_PACKET;
	slot->user = NULL;

	/* Writes the confirmation: the peer's number, ours, our window and our packet size. */
	ssh_buf_put_u8(out, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
	ssh_buf_put_u32(out, sender);
	ssh_buf_put_u32(out, slot->local_id);
	ssh_buf_put_u32(out, SSH_CHANNEL_WINDOW);
	ssh_buf_put_u32(out, SSH_CHANNEL_PACKET);

	/* Gives the slot back when the confirmation could not be queued. */
	error = ssh_buf_error(out);
	if (error != 0) {
		reset_slot(slot);
		return error;
	}

	/* Succeeded: the channel is open and the confirmation is queued. */
	*channel = slot;
	return 0;
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
	struct ssh_reader reader;
	struct ssh_channel *slot;
	const uint8_t *text;
	size_t text_size;
	unsigned message;
	uint32_t recipient;
	uint32_t sender;
	uint32_t window;
	uint32_t packet;
	uint32_t code;
	int error;

	*channel = NULL;
	*reason = 0;
	sender = 0;
	window = 0;
	packet = 0;
	code = 0;

	/* Reads the number and the recipient. */
	ssh_reader_init(&reader, payload, size);
	error = read_message_number(&reader, SSH_MSG_CHANNEL_OPEN_CONFIRMATION, SSH_MSG_CHANNEL_OPEN_FAILURE, &message);
	if (error != 0)
		return error;
	recipient = ssh_reader_get_u32(&reader);

	/* Reads the rest of the answer: our fields for a confirmation, the code and the two strings for a failure. */
	if (message == SSH_MSG_CHANNEL_OPEN_CONFIRMATION) {
		sender = ssh_reader_get_u32(&reader);
		window = ssh_reader_get_u32(&reader);
		packet = ssh_reader_get_u32(&reader);
	} else {
		code = ssh_reader_get_u32(&reader);
		ssh_reader_get_string(&reader, &text, &text_size);
		ssh_reader_get_string(&reader, &text, &text_size);
	}
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;

	/* The answer must be for a channel this side has opened and is waiting for. */
	if (recipient >= SSH_CHANNEL_MAX)
		return EPROTO;
	slot = &channels->channel[recipient];
	if (slot->state != SSH_CHANNEL_OPENING)
		return EPROTO;

	/* A failure frees the slot and tells the caller why. */
	if (message == SSH_MSG_CHANNEL_OPEN_FAILURE) {
		reset_slot(slot);
		*channel = slot;
		*reason = code;
		return EACCES;
	}

	/* A peer that accepts no data at all cannot be sent anything: a protocol fault. */
	if (packet < 1)
		return EPROTO;

	/* Opens the channel with the peer's number, window and packet size (the packet size capped as for an accepted open). */
	slot->state = SSH_CHANNEL_OPEN;
	slot->remote_id = sender;
	slot->remote_window = window;
	slot->remote_packet = packet;
	if (slot->remote_packet > SSH_CHANNEL_PACKET)
		slot->remote_packet = SSH_CHANNEL_PACKET;

	/* Succeeded: the channel is open. */
	*channel = slot;
	return 0;
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
	struct ssh_channel *slot;

	/* An id past the table names no channel. */
	if (id >= SSH_CHANNEL_MAX)
		return NULL;

	/* Only a channel that is open, or closing and still readable, can be found. */
	slot = &channels->channel[id];
	if (slot->state == SSH_CHANNEL_OPEN)
		return slot;
	if (slot->state == SSH_CHANNEL_CLOSING)
		return slot;

	/* Reports that the slot is free or still waiting for the peer's answer. */
	return NULL;
}

/*
 * Reads CHANNEL_DATA or CHANNEL_EXTENDED_DATA: *channel, *data and *size
 * (pointing into the payload) and *extended (0 for data, else the type).
 *
 * The framing is read before the state is looked at, and the window is
 * charged only when every check has passed.
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
	struct ssh_reader reader;
	struct ssh_channel *found;
	const uint8_t *bytes;
	size_t bytes_size;
	unsigned message;
	uint32_t recipient;
	uint32_t type;
	int error;

	*channel = NULL;
	*data = NULL;
	*data_size = 0;
	*extended = 0;
	type = 0;

	/* Reads the framing: number, recipient, the data type for extended data, the string. */
	ssh_reader_init(&reader, payload, size);
	error = read_message_number(&reader, SSH_MSG_CHANNEL_DATA, SSH_MSG_CHANNEL_EXTENDED_DATA, &message);
	if (error != 0)
		return error;
	recipient = ssh_reader_get_u32(&reader);
	if (message == SSH_MSG_CHANNEL_EXTENDED_DATA)
		type = ssh_reader_get_u32(&reader);
	ssh_reader_get_string(&reader, &bytes, &bytes_size);
	error = ssh_reader_finish(&reader);
	if (error != 0)
		return EINVAL;

	/* Data for a channel that is not open is a protocol fault. */
	found = ssh_channel_find(channels, recipient);
	if (found == NULL)
		return EPROTO;

	/* The peer may not send after its own end of data or its close. */
	if (found->eof_received != 0)
		return EPROTO;
	if (found->close_received != 0)
		return EPROTO;

	/* Type 0 is how a caller tells ordinary data apart, and RFC 4254 5.2 numbers the extended types from 1. */
	if (message == SSH_MSG_CHANNEL_EXTENDED_DATA && type == 0)
		return EPROTO;

	/* The peer may not send more than the packet size announced, nor more than the window left. */
	if (bytes_size > SSH_CHANNEL_PACKET)
		return EPROTO;
	if (bytes_size > found->local_window)
		return EPROTO;

	/* Charges the data to the window (RFC 4254 5.2: extended data uses the same window). */
	found->local_window -= (uint32_t)bytes_size;

	/* Succeeded: the data points into the payload. */
	*channel = found;
	*data = bytes;
	*data_size = bytes_size;
	*extended = type;
	return 0;
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
	size_t room;
	int error;

	/* Refuses bytes that are missing. */
	if (data == NULL && size != 0)
		return EINVAL;

	/* Nothing may follow the end of data or the close this side has sent. */
	if (channel->eof_sent != 0)
		return EPIPE;
	if (channel->close_sent != 0)
		return EPIPE;

	/* Data goes only on a channel that is open. */
	if (channel->state != SSH_CHANNEL_OPEN)
		return EINVAL;

	/* Refuses more than the window and the peer's packet size allow. */
	room = ssh_channel_send_room(channel);
	if (size > room)
		return EINVAL;

	/* Does nothing when the output has already failed. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Writes the header, then the bytes as a string. */
	if (extended == 0) {
		ssh_buf_put_u8(out, SSH_MSG_CHANNEL_DATA);
		ssh_buf_put_u32(out, channel->remote_id);
	} else {
		ssh_buf_put_u8(out, SSH_MSG_CHANNEL_EXTENDED_DATA);
		ssh_buf_put_u32(out, channel->remote_id);
		ssh_buf_put_u32(out, extended);
	}
	ssh_buf_put_string(out, data, size);

	/* Leaves the window alone when the message could not be queued. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Takes the bytes from the window the peer gave (RFC 4254 5.2). */
	channel->remote_window -= (uint32_t)size;

	/* Succeeded: the data message is queued. */
	return 0;
}

/*
 * Returns how many bytes may be sent in one data message now: the smaller
 * of remote_window and remote_packet (0 when the channel is not OPEN).
 */
size_t
ssh_channel_send_room(
	const struct ssh_channel *channel)
{
	/* A channel that is not open takes no data. */
	if (channel->state != SSH_CHANNEL_OPEN)
		return 0;

	/* The window and the packet size both limit one message. */
	if (channel->remote_window < channel->remote_packet)
		return channel->remote_window;

	/* The packet size is the smaller one. */
	return channel->remote_packet;
}

/*
 * Tells the channel that the program has consumed size received bytes
 * (written them on), and, when the local window has fallen below half of
 * SSH_CHANNEL_WINDOW, appends WINDOW_ADJUST bringing it back to
 * SSH_CHANNEL_WINDOW.
 *
 * Telling it that nothing was consumed (size 0) changes nothing, and a
 * channel that is not open needs no more room.
 */
void
ssh_channel_consumed(
	struct ssh_channel *channel,
	size_t size,
	struct ssh_buf *out)
{
	uint32_t amount;
	int error;

	/* Nothing consumed means nothing to give back. */
	if (size == 0)
		return;

	/* A channel that is closing receives no more data. */
	if (channel->state != SSH_CHANNEL_OPEN)
		return;

	/* The window is still more than half full: the peer has room. */
	if (channel->local_window >= CHANNEL_WINDOW_HALF)
		return;

	/* Does nothing when the output has already failed. */
	error = ssh_buf_error(out);
	if (error != 0)
		return;

	/* Works out how much brings the window back to its full size. */
	amount = SSH_CHANNEL_WINDOW - channel->local_window;

	/* Writes the adjustment. */
	ssh_buf_put_u8(out, SSH_MSG_CHANNEL_WINDOW_ADJUST);
	ssh_buf_put_u32(out, channel->remote_id);
	ssh_buf_put_u32(out, amount);

	/* Keeps the window as it was when the message could not be queued. */
	error = ssh_buf_error(out);
	if (error != 0)
		return;

	/* The peer now has the full window again. */
	channel->local_window = SSH_CHANNEL_WINDOW;
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
	struct ssh_reader reader;
	struct ssh_channel *found;
	unsigned message;
	uint32_t recipient;
	uint32_t increase;
	uint64_t total;
	int error;

	*channel = NULL;

	/* Reads the framing. */
	ssh_reader_init(&reader, payload, size);
	error = read_message_number(&reader, SSH_MSG_CHANNEL_WINDOW_ADJUST, 0, &message);
	if (error != 0)
		return error;
	recipient = ssh_reader_get_u32(&reader);
	increase = ssh_reader_get_u32(&reader);
	error = ssh_reader_finish(&reader);
	if (error != 0)
		return EINVAL;

	/* An adjustment for a channel that is not open is a protocol fault. */
	found = ssh_channel_find(channels, recipient);
	if (found == NULL)
		return EPROTO;

	/* The window must not pass 2^32 - 1 (RFC 4254 5.2); the sum is made in a wider type. */
	total = (uint64_t)found->remote_window + (uint64_t)increase;
	if (total > CHANNEL_WINDOW_LIMIT)
		return EPROTO;

	/* Adds the room the peer made. */
	found->remote_window = (uint32_t)total;

	/* Succeeded: the window grew. */
	*channel = found;
	return 0;
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
	struct ssh_reader reader;
	struct ssh_channel *found;
	unsigned message;
	uint32_t recipient;
	int error;

	*channel = NULL;

	/* Reads the framing. */
	ssh_reader_init(&reader, payload, size);
	error = read_message_number(&reader, SSH_MSG_CHANNEL_EOF, SSH_MSG_CHANNEL_CLOSE, &message);
	if (error != 0)
		return error;
	recipient = ssh_reader_get_u32(&reader);
	error = ssh_reader_finish(&reader);
	if (error != 0)
		return EINVAL;

	/* A message for a channel that is not open is a protocol fault. */
	found = ssh_channel_find(channels, recipient);
	if (found == NULL)
		return EPROTO;

	/* Nothing may follow the peer's close, and the end of data may come once. */
	if (found->close_received != 0)
		return EPROTO;
	if (message == SSH_MSG_CHANNEL_EOF && found->eof_received != 0)
		return EPROTO;

	/* Records what the peer said. */
	if (message == SSH_MSG_CHANNEL_EOF) {
		found->eof_received = 1;
	} else {
		/* A close received means the channel is on its way out; the slot is freed when ours has gone too. */
		found->close_received = 1;
		found->state = SSH_CHANNEL_CLOSING;
	}

	/* Succeeded: the message is recorded. */
	*channel = found;
	return 0;
}

/*
 * Appends CHANNEL_EOF (once; later calls append nothing) and sets eof_sent.
 */
void
ssh_channel_put_eof(
	struct ssh_channel *channel,
	struct ssh_buf *out)
{
	int error;

	/* Only a channel the peer knows can be told, and only once; nothing follows a close. */
	if (channel->state != SSH_CHANNEL_OPEN && channel->state != SSH_CHANNEL_CLOSING)
		return;
	if (channel->eof_sent != 0)
		return;
	if (channel->close_sent != 0)
		return;

	/* Does nothing when the output has already failed. */
	error = ssh_buf_error(out);
	if (error != 0)
		return;

	/* Writes the message. */
	ssh_buf_put_u8(out, SSH_MSG_CHANNEL_EOF);
	ssh_buf_put_u32(out, channel->remote_id);

	/* Records that the end of data was sent, unless the message was lost. */
	error = ssh_buf_error(out);
	if (error != 0)
		return;
	channel->eof_sent = 1;
}

/*
 * Appends CHANNEL_CLOSE (once) and sets close_sent; the state becomes CLOSING.
 */
void
ssh_channel_put_close(
	struct ssh_channel *channel,
	struct ssh_buf *out)
{
	int error;

	/* Only a channel the peer knows can be closed, and only once. */
	if (channel->state != SSH_CHANNEL_OPEN && channel->state != SSH_CHANNEL_CLOSING)
		return;
	if (channel->close_sent != 0)
		return;

	/* Does nothing when the output has already failed. */
	error = ssh_buf_error(out);
	if (error != 0)
		return;

	/* Writes the message. */
	ssh_buf_put_u8(out, SSH_MSG_CHANNEL_CLOSE);
	ssh_buf_put_u32(out, channel->remote_id);

	/* Records that the close was sent, unless the message was lost. */
	error = ssh_buf_error(out);
	if (error != 0)
		return;
	channel->close_sent = 1;
	channel->state = SSH_CHANNEL_CLOSING;
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
	int inside;

	/* Only a slot of this table can be freed. */
	inside = slot_in_table(channels, channel);
	if (inside == 0)
		return 0;

	/* The slot stays until the close has gone both ways (RFC 4254 5.3). */
	if (channel->state != SSH_CHANNEL_CLOSING)
		return 0;
	if (channel->close_sent == 0)
		return 0;
	if (channel->close_received == 0)
		return 0;

	/* Frees the slot; the program's own state (user) is not touched. */
	reset_slot(channel);

	/* Succeeded: the number can be used again. */
	return 1;
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
	struct ssh_reader reader;
	struct ssh_channel *found;
	const uint8_t *name;
	size_t name_size;
	unsigned message;
	uint32_t recipient;
	enum ssh_channel_request_type kind;
	int want_reply;
	int error;

	/* Reads the framing every request has: number, recipient, request name, want_reply. */
	ssh_reader_init(&reader, payload, size);
	error = read_message_number(&reader, SSH_MSG_CHANNEL_REQUEST, 0, &message);
	if (error != 0)
		return error;
	recipient = ssh_reader_get_u32(&reader);
	ssh_reader_get_string(&reader, &name, &name_size);
	want_reply = ssh_reader_get_bool(&reader);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;

	/* A request for a channel that is not open is a protocol fault. */
	found = ssh_channel_find(channels, recipient);
	if (found == NULL)
		return EPROTO;

	/* Starts as a request that is read no further: its name, nothing else. */
	request->recipient = recipient;
	request->type = SSH_CREQ_OTHER;
	request->want_reply = want_reply;
	copy_name_cut(request->name, sizeof(request->name), name, name_size);
	request->term[0] = '\0';
	request->value[0] = '\0';
	request->columns = 0;
	request->rows = 0;
	request->width = 0;
	request->height = 0;
	request->modes = NULL;
	request->modes_size = 0;
	request->command = NULL;
	request->command_size = 0;
	request->status = 0;
	request->core_dumped = 0;
	request->message = NULL;
	request->message_size = 0;

	/* Reads the fields of the request type the name stands for; any other name stays SSH_CREQ_OTHER. */
	error = 0;
	kind = find_request_type(name, name_size);
	switch (kind) {
	case SSH_CREQ_PTY:
		error = parse_request_pty(&reader, request);
		break;
	case SSH_CREQ_ENV:
		error = parse_request_env(&reader, request);
		break;
	case SSH_CREQ_SHELL:
	case SSH_CREQ_EOW:
		/* These two have no fields of their own. */
		request->type = kind;
		request->name[0] = '\0';
		break;
	case SSH_CREQ_EXEC:
		error = parse_request_exec(&reader, request);
		break;
	case SSH_CREQ_SUBSYSTEM:
		error = parse_request_subsystem(&reader, request);
		break;
	case SSH_CREQ_WINDOW_CHANGE:
		error = parse_request_window_change(&reader, request);
		break;
	case SSH_CREQ_SIGNAL:
		error = parse_request_signal(&reader, request);
		break;
	case SSH_CREQ_EXIT_STATUS:
		error = parse_request_exit_status(&reader, request);
		break;
	case SSH_CREQ_EXIT_SIGNAL:
		error = parse_request_exit_signal(&reader, request);
		break;
	case SSH_CREQ_OTHER:
		break;
	}

	/* Reports a known request whose fields run past the end. */
	if (error != 0)
		return error;

	/* Succeeded: the request is read. */
	return 0;
}

/*
 * Appends CHANNEL_SUCCESS (success 1) or CHANNEL_FAILURE (0) for the channel's remote id.
 */
void
ssh_channel_put_reply(
	const struct ssh_channel *channel,
	int success,
	struct ssh_buf *out)
{
	/* Only a channel the peer knows can be answered on. */
	if (channel->state != SSH_CHANNEL_OPEN && channel->state != SSH_CHANNEL_CLOSING) {
		record_error(out, EINVAL);
		return;
	}

	/* Writes the answer. */
	if (success != 0)
		ssh_buf_put_u8(out, SSH_MSG_CHANNEL_SUCCESS);
	else
		ssh_buf_put_u8(out, SSH_MSG_CHANNEL_FAILURE);
	ssh_buf_put_u32(out, channel->remote_id);
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
	struct ssh_reader reader;
	struct ssh_channel *found;
	unsigned message;
	uint32_t recipient;
	int error;

	*channel = NULL;
	*success = 0;

	/* Reads the framing. */
	ssh_reader_init(&reader, payload, size);
	error = read_message_number(&reader, SSH_MSG_CHANNEL_SUCCESS, SSH_MSG_CHANNEL_FAILURE, &message);
	if (error != 0)
		return error;
	recipient = ssh_reader_get_u32(&reader);
	error = ssh_reader_finish(&reader);
	if (error != 0)
		return EINVAL;

	/* An answer for a channel that is not open is a protocol fault. */
	found = ssh_channel_find(channels, recipient);
	if (found == NULL)
		return EPROTO;

	/* Succeeded: reports which answer it was. */
	*channel = found;
	if (message == SSH_MSG_CHANNEL_SUCCESS)
		*success = 1;
	return 0;
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
	int error;

	/* Writes the start of the request. */
	error = put_request_start(channel, "pty-req", 1, out);
	if (error != 0)
		return;

	/* Writes the terminal type, the four sizes and the encoded modes. */
	ssh_buf_put_cstring(out, term);
	ssh_buf_put_u32(out, columns);
	ssh_buf_put_u32(out, rows);
	ssh_buf_put_u32(out, width);
	ssh_buf_put_u32(out, height);
	ssh_buf_put_string(out, modes, modes_size);
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
	int error;

	/* Writes the start of the request. */
	error = put_request_start(channel, "env", 0, out);
	if (error != 0)
		return;

	/* Writes the variable name and its value. */
	ssh_buf_put_cstring(out, name);
	ssh_buf_put_cstring(out, value);
}

/*
 * Appends a "shell" request (6.5), want_reply TRUE.
 */
void
ssh_channel_put_shell(
	const struct ssh_channel *channel,
	struct ssh_buf *out)
{
	int error;

	/* Writes the request, which has no fields of its own. */
	error = put_request_start(channel, "shell", 1, out);
	if (error != 0)
		return;
}

/*
 * Appends an "exec" request with size bytes of command (6.5), want_reply TRUE.
 */
void
ssh_channel_put_exec(
	const struct ssh_channel *channel,
	const uint8_t *command,
	size_t size,
	struct ssh_buf *out)
{
	int error;

	/* Writes the start of the request. */
	error = put_request_start(channel, "exec", 1, out);
	if (error != 0)
		return;

	/* Writes the command, which is bytes and not necessarily text. */
	ssh_buf_put_string(out, command, size);
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
	int error;

	/* Writes the start of the request. */
	error = put_request_start(channel, "subsystem", 1, out);
	if (error != 0)
		return;

	/* Writes the subsystem name. */
	ssh_buf_put_cstring(out, name);
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
	int error;

	/* Writes the start of the request. */
	error = put_request_start(channel, "window-change", 0, out);
	if (error != 0)
		return;

	/* Writes the four sizes: columns, rows, pixel width, pixel height. */
	ssh_buf_put_u32(out, columns);
	ssh_buf_put_u32(out, rows);
	ssh_buf_put_u32(out, width);
	ssh_buf_put_u32(out, height);
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
	int error;

	/* Writes the start of the request. */
	error = put_request_start(channel, "exit-status", 0, out);
	if (error != 0)
		return;

	/* Writes the exit status. */
	ssh_buf_put_u32(out, status);
}

/*
 * Appends an "exit-signal" request (6.10): the signal name without "SIG"
 * (one of the names of 6.10, e.g. "TERM"), core dumped, an empty message
 * and an empty language tag; want_reply FALSE.
 */
void
ssh_channel_put_exit_signal(
	const struct ssh_channel *channel,
	const char *name,
	int core_dumped,
	struct ssh_buf *out)
{
	int error;

	/* Writes the start of the request. */
	error = put_request_start(channel, "exit-signal", 0, out);
	if (error != 0)
		return;

	/* Writes the signal name, the core flag, and the empty message and language tag. */
	ssh_buf_put_cstring(out, name);
	ssh_buf_put_bool(out, core_dumped);
	ssh_buf_put_string(out, NULL, 0);
	ssh_buf_put_string(out, NULL, 0);
}

/*
 * Reads a GLOBAL_REQUEST: name (capacity SSH_NAME_MAX + 1; a longer name
 * is cut) and *want_reply; the data after them is not read.
 *
 * The one request that is honoured, no-more-sessions@openssh.com, is
 * answered with REQUEST_SUCCESS when a reply is wanted; every other is
 * refused with REQUEST_FAILURE.  The name is compared as the whole string
 * the peer sent.
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
	struct ssh_reader reader;
	const uint8_t *request_name;
	size_t request_name_size;
	unsigned message;
	int wanted;
	int honoured;
	int error;

	name[0] = '\0';
	*want_reply = 0;

	/* Reads the framing: number, name, want_reply. */
	ssh_reader_init(&reader, payload, size);
	error = read_message_number(&reader, SSH_MSG_GLOBAL_REQUEST, 0, &message);
	if (error != 0)
		return error;
	ssh_reader_get_string(&reader, &request_name, &request_name_size);
	wanted = ssh_reader_get_bool(&reader);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;

	/* Gives the caller the name (cut to fit) and whether a reply is wanted. */
	copy_name_cut(name, SSH_NAME_MAX + 1U, request_name, request_name_size);
	*want_reply = wanted;

	/* Honours the request that closes the door to further sessions. */
	honoured = name_is(request_name, request_name_size, GLOBAL_NO_MORE_SESSIONS);
	if (honoured != 0)
		channels->no_more_sessions = 1;

	/* Answers only when a reply is wanted. */
	if (wanted != 0) {
		if (honoured != 0)
			ssh_buf_put_u8(out, SSH_MSG_REQUEST_SUCCESS);
		else
			ssh_buf_put_u8(out, SSH_MSG_REQUEST_FAILURE);
	}
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Succeeded: the request is read and answered. */
	return 0;
}

/*
 * Appends the client's GLOBAL_REQUEST "no-more-sessions@openssh.com", want_reply FALSE.
 */
void
ssh_channel_put_no_more_sessions(
	struct ssh_buf *out)
{
	/* Writes the request, which has no data. */
	ssh_buf_put_u8(out, SSH_MSG_GLOBAL_REQUEST);
	ssh_buf_put_cstring(out, GLOBAL_NO_MORE_SESSIONS);
	ssh_buf_put_bool(out, 0);
}

/*
 * Appends a GLOBAL_REQUEST with name and want_reply and no data.
 */
void
ssh_channel_put_global_request(
	const char *name,
	int want_reply,
	struct ssh_buf *out)
{
	/* Writes the request, which has no data. */
	ssh_buf_put_u8(out, SSH_MSG_GLOBAL_REQUEST);
	ssh_buf_put_cstring(out, name);
	ssh_buf_put_bool(out, want_reply);
}

/*
 * Returns the first free slot, or NULL when every slot is in use.
 */
static struct ssh_channel *
take_free_slot(
	struct ssh_channels *channels)
{
	unsigned index;

	/* Looks for a slot nobody uses. */
	for (index = 0;
	     index < SSH_CHANNEL_MAX;
	     index++) {
		if (channels->channel[index].state == SSH_CHANNEL_FREE) {
			reset_slot(&channels->channel[index]);
			return &channels->channel[index];
		}
	}

	/* Reports that the table is full. */
	return NULL;
}

/*
 * Puts a slot back to the free state, keeping its number and the
 * program's own pointer.
 */
static void
reset_slot(
	struct ssh_channel *channel)
{
	/* Forgets the peer and the windows; the flags say no message went either way. */
	channel->state = SSH_CHANNEL_FREE;
	channel->remote_id = 0;
	channel->local_window = 0;
	channel->remote_window = 0;
	channel->remote_packet = 0;
	channel->eof_sent = 0;
	channel->eof_received = 0;
	channel->close_sent = 0;
	channel->close_received = 0;
}

/*
 * Reports whether channel is one of the slots of the table.
 */
static int
slot_in_table(
	const struct ssh_channels *channels,
	const struct ssh_channel *channel)
{
	unsigned index;

	/* Compares the address with each slot. */
	for (index = 0;
	     index < SSH_CHANNEL_MAX;
	     index++) {
		if (&channels->channel[index] == channel)
			return 1;
	}

	/* Reports that the address is not a slot. */
	return 0;
}

/*
 * Reads the message number and checks that it is first or second (second
 * 0 for none); *message receives it.  A different number is EINVAL.
 */
static int
read_message_number(
	struct ssh_reader *reader,
	unsigned first,
	unsigned second,
	unsigned *message)
{
	unsigned number;
	int reader_error;

	/* Reads the first byte of the payload. */
	number = ssh_reader_get_u8(reader);
	*message = number;
	reader_error = ssh_reader_error(reader);
	if (reader_error != 0)
		return EINVAL;

	/* Checks that it is a message the caller expects. */
	if (number == first)
		return 0;
	if (second != 0 && number == second)
		return 0;

	/* Reports a payload of another message. */
	return EINVAL;
}

/*
 * Appends CHANNEL_OPEN_FAILURE: the recipient, the reason, the text and an empty language tag.
 */
static void
put_open_failure(
	struct ssh_buf *out,
	uint32_t recipient,
	uint32_t reason,
	const char *text)
{
	/* Writes the refusal (RFC 4254 5.1). */
	ssh_buf_put_u8(out, SSH_MSG_CHANNEL_OPEN_FAILURE);
	ssh_buf_put_u32(out, recipient);
	ssh_buf_put_u32(out, reason);
	ssh_buf_put_cstring(out, text);
	ssh_buf_put_string(out, NULL, 0);
}

/*
 * Records a caller's mistake in the output, unless it already holds an error.
 */
static void
record_error(
	struct ssh_buf *out,
	int error)
{
	/* The first error wins. */
	if (out->error == 0)
		out->error = error;
}

/*
 * Reports whether the size bytes at name are exactly the text.
 */
static int
name_is(
	const uint8_t *name,
	size_t size,
	const char *text)
{
	size_t length;
	int different;

	/* A name of another length is another name. */
	length = strlen(text);
	if (size != length)
		return 0;

	/* Compares the bytes; an empty name never gets here with a non-empty text. */
	if (length == 0)
		return 1;
	different = memcmp(name, text, length);
	if (different != 0)
		return 0;

	/* Reports that the name is the text. */
	return 1;
}

/*
 * Finds the request type a name stands for; SSH_CREQ_OTHER when it is none.
 */
static enum ssh_channel_request_type
find_request_type(
	const uint8_t *name,
	size_t size)
{
	size_t index;
	int same;

	/* Compares the name with each row of the table. */
	for (index = 0;
	     request_names[index].text != NULL;
	     index++) {
		same = name_is(name, size, request_names[index].text);
		if (same != 0)
			return request_names[index].type;
	}

	/* Reports a name the module does not read. */
	return SSH_CREQ_OTHER;
}

/*
 * Copies a name into out as a C string of at most capacity - 1 characters,
 * cutting it when it is longer.  A byte that is not printable US-ASCII
 * becomes '?', so the result is one line of text that can be logged.
 */
static void
copy_name_cut(
	char *out,
	size_t capacity,
	const uint8_t *bytes,
	size_t size)
{
	size_t count;
	size_t index;

	/* Takes as many bytes as fit. */
	count = size;
	if (count > capacity - 1)
		count = capacity - 1;

	/* Copies them, replacing anything that is not printable. */
	for (index = 0;
	     index < count;
	     index++) {
		if (bytes[index] >= 0x21 && bytes[index] <= 0x7e)
			out[index] = (char)bytes[index];
		else
			out[index] = '?';
	}
	out[count] = '\0';
}

/*
 * Copies a string that must be text into out as a C string.  Returns 0, or
 * 1 when the string is longer than limit or holds a NUL byte, in which
 * case out is left empty.
 */
static int
copy_text(
	char *out,
	size_t limit,
	const uint8_t *bytes,
	size_t size)
{
	size_t index;

	out[0] = '\0';

	/* A string longer than the field is refused. */
	if (size > limit)
		return 1;

	/* A NUL byte would end the C string early, so the text would not be the text the peer sent. */
	for (index = 0;
	     index < size;
	     index++) {
		if (bytes[index] == 0)
			return 1;
	}

	/* Copies the text. */
	if (size > 0)
		memcpy(out, bytes, size);
	out[size] = '\0';

	/* Reports that the text fits. */
	return 0;
}

/*
 * Stores a name that has been checked to fit (at most
 * SSH_CHANNEL_ENV_NAME_MAX characters) in the request's name field.
 */
static void
copy_to_name(
	struct ssh_channel_request *request,
	const char *text)
{
	size_t length;

	/* Copies the characters and the NUL that ends them. */
	length = strlen(text);
	memcpy(request->name, text, length + 1);
}

/*
 * Appends the start of a CHANNEL_REQUEST: the number, the peer's channel
 * number, the request name and want_reply.  Returns 0, or EINVAL (recorded
 * in out) when the channel is not one the peer knows.
 */
static int
put_request_start(
	const struct ssh_channel *channel,
	const char *name,
	int want_reply,
	struct ssh_buf *out)
{
	/* A request can be sent only on a channel the peer has confirmed. */
	if (channel->state != SSH_CHANNEL_OPEN && channel->state != SSH_CHANNEL_CLOSING) {
		record_error(out, EINVAL);
		return EINVAL;
	}

	/* Writes the fields every request has (RFC 4254 5.4). */
	ssh_buf_put_u8(out, SSH_MSG_CHANNEL_REQUEST);
	ssh_buf_put_u32(out, channel->remote_id);
	ssh_buf_put_cstring(out, name);
	ssh_buf_put_bool(out, want_reply);

	/* Succeeded: the start is queued. */
	return 0;
}

/*
 * Reads the fields of a "pty-req" (RFC 4254 6.2).  A terminal type that
 * is too long or holds a NUL leaves the request as SSH_CREQ_OTHER.
 */
static int
parse_request_pty(
	struct ssh_reader *reader,
	struct ssh_channel_request *request)
{
	const uint8_t *term;
	size_t term_size;
	const uint8_t *modes;
	size_t modes_size;
	uint32_t columns;
	uint32_t rows;
	uint32_t width;
	uint32_t height;
	int refused;
	int reader_error;

	/* Reads the fields in the order of the RFC. */
	ssh_reader_get_string(reader, &term, &term_size);
	columns = ssh_reader_get_u32(reader);
	rows = ssh_reader_get_u32(reader);
	width = ssh_reader_get_u32(reader);
	height = ssh_reader_get_u32(reader);
	ssh_reader_get_string(reader, &modes, &modes_size);
	reader_error = ssh_reader_error(reader);
	if (reader_error != 0)
		return EINVAL;

	/* A terminal type that does not fit is refused as a request, not as the connection. */
	refused = copy_text(request->term, SSH_CHANNEL_TERM_MAX, term, term_size);
	if (refused != 0)
		return 0;

	/* Keeps the sizes and the modes (which point into the payload). */
	request->type = SSH_CREQ_PTY;
	request->name[0] = '\0';
	request->columns = columns;
	request->rows = rows;
	request->width = width;
	request->height = height;
	request->modes = modes;
	request->modes_size = modes_size;

	/* Succeeded: the request is a pty-req. */
	return 0;
}

/*
 * Reads the fields of an "env" request (RFC 4254 6.4).
 */
static int
parse_request_env(
	struct ssh_reader *reader,
	struct ssh_channel_request *request)
{
	const uint8_t *variable;
	size_t variable_size;
	const uint8_t *value;
	size_t value_size;
	char name_text[SSH_CHANNEL_ENV_NAME_MAX + 1];
	int refused;
	int reader_error;

	/* Reads the variable name and its value. */
	ssh_reader_get_string(reader, &variable, &variable_size);
	ssh_reader_get_string(reader, &value, &value_size);
	reader_error = ssh_reader_error(reader);
	if (reader_error != 0)
		return EINVAL;

	/* A name or a value that does not fit, or holds a NUL, makes the request one that is refused. */
	refused = copy_text(name_text, SSH_CHANNEL_ENV_NAME_MAX, variable, variable_size);
	if (refused != 0)
		return 0;
	refused = copy_text(request->value, SSH_CHANNEL_ENV_VALUE_MAX, value, value_size);
	if (refused != 0)
		return 0;

	/* Keeps the pair. */
	request->type = SSH_CREQ_ENV;
	copy_to_name(request, name_text);

	/* Succeeded: the request is an env. */
	return 0;
}

/*
 * Reads the field of an "exec" request (RFC 4254 6.5): the command, which
 * is bytes and stays in the payload.
 */
static int
parse_request_exec(
	struct ssh_reader *reader,
	struct ssh_channel_request *request)
{
	const uint8_t *command;
	size_t command_size;
	int reader_error;

	/* Reads the command. */
	ssh_reader_get_string(reader, &command, &command_size);
	reader_error = ssh_reader_error(reader);
	if (reader_error != 0)
		return EINVAL;

	/* A command longer than the limit makes the request one that is refused. */
	if (command_size > SSH_CHANNEL_COMMAND_MAX)
		return 0;

	/* Keeps the command. */
	request->type = SSH_CREQ_EXEC;
	request->name[0] = '\0';
	request->command = command;
	request->command_size = command_size;

	/* Succeeded: the request is an exec. */
	return 0;
}

/*
 * Reads the field of a "subsystem" request (RFC 4254 6.5).
 */
static int
parse_request_subsystem(
	struct ssh_reader *reader,
	struct ssh_channel_request *request)
{
	const uint8_t *subsystem;
	size_t subsystem_size;
	char name_text[SSH_CHANNEL_ENV_NAME_MAX + 1];
	int refused;
	int reader_error;

	/* Reads the subsystem name. */
	ssh_reader_get_string(reader, &subsystem, &subsystem_size);
	reader_error = ssh_reader_error(reader);
	if (reader_error != 0)
		return EINVAL;

	/* A name that does not fit, or holds a NUL, makes the request one that is refused. */
	refused = copy_text(name_text, SSH_CHANNEL_ENV_NAME_MAX, subsystem, subsystem_size);
	if (refused != 0)
		return 0;

	/* Keeps the name. */
	request->type = SSH_CREQ_SUBSYSTEM;
	copy_to_name(request, name_text);

	/* Succeeded: the request is a subsystem. */
	return 0;
}

/*
 * Reads the fields of a "window-change" request (RFC 4254 6.7).
 */
static int
parse_request_window_change(
	struct ssh_reader *reader,
	struct ssh_channel_request *request)
{
	uint32_t columns;
	uint32_t rows;
	uint32_t width;
	uint32_t height;
	int reader_error;

	/* Reads the four sizes: columns, rows, pixel width, pixel height. */
	columns = ssh_reader_get_u32(reader);
	rows = ssh_reader_get_u32(reader);
	width = ssh_reader_get_u32(reader);
	height = ssh_reader_get_u32(reader);
	reader_error = ssh_reader_error(reader);
	if (reader_error != 0)
		return EINVAL;

	/* Keeps them. */
	request->type = SSH_CREQ_WINDOW_CHANGE;
	request->name[0] = '\0';
	request->columns = columns;
	request->rows = rows;
	request->width = width;
	request->height = height;

	/* Succeeded: the request is a window-change. */
	return 0;
}

/*
 * Reads the field of a "signal" request (RFC 4254 6.9).
 */
static int
parse_request_signal(
	struct ssh_reader *reader,
	struct ssh_channel_request *request)
{
	const uint8_t *signal;
	size_t signal_size;
	char name_text[SSH_CHANNEL_SIGNAL_MAX + 1];
	int refused;
	int reader_error;

	/* Reads the signal name. */
	ssh_reader_get_string(reader, &signal, &signal_size);
	reader_error = ssh_reader_error(reader);
	if (reader_error != 0)
		return EINVAL;

	/* A name that does not fit, or holds a NUL, makes the request one that is refused. */
	refused = copy_text(name_text, SSH_CHANNEL_SIGNAL_MAX, signal, signal_size);
	if (refused != 0)
		return 0;

	/* Keeps the name. */
	request->type = SSH_CREQ_SIGNAL;
	copy_to_name(request, name_text);

	/* Succeeded: the request is a signal. */
	return 0;
}

/*
 * Reads the field of an "exit-status" request (RFC 4254 6.10).
 */
static int
parse_request_exit_status(
	struct ssh_reader *reader,
	struct ssh_channel_request *request)
{
	uint32_t status;
	int reader_error;

	/* Reads the exit status. */
	status = ssh_reader_get_u32(reader);
	reader_error = ssh_reader_error(reader);
	if (reader_error != 0)
		return EINVAL;

	/* Keeps it. */
	request->type = SSH_CREQ_EXIT_STATUS;
	request->name[0] = '\0';
	request->status = status;

	/* Succeeded: the request is an exit-status. */
	return 0;
}

/*
 * Reads the fields of an "exit-signal" request (RFC 4254 6.10): the signal
 * name, the core flag, the message (which stays in the payload) and the
 * language tag (read and dropped).
 */
static int
parse_request_exit_signal(
	struct ssh_reader *reader,
	struct ssh_channel_request *request)
{
	const uint8_t *signal;
	size_t signal_size;
	const uint8_t *message;
	size_t message_size;
	const uint8_t *language;
	size_t language_size;
	char name_text[SSH_CHANNEL_SIGNAL_MAX + 1];
	int core_dumped;
	int refused;
	int reader_error;

	/* Reads the fields in the order of the RFC. */
	ssh_reader_get_string(reader, &signal, &signal_size);
	core_dumped = ssh_reader_get_bool(reader);
	ssh_reader_get_string(reader, &message, &message_size);
	ssh_reader_get_string(reader, &language, &language_size);
	reader_error = ssh_reader_error(reader);
	if (reader_error != 0)
		return EINVAL;

	/* A name that does not fit, or holds a NUL, makes the request one that is refused. */
	refused = copy_text(name_text, SSH_CHANNEL_SIGNAL_MAX, signal, signal_size);
	if (refused != 0)
		return 0;

	/* Keeps the name, the flag and the message. */
	request->type = SSH_CREQ_EXIT_SIGNAL;
	copy_to_name(request, name_text);
	request->core_dumped = core_dumped;
	request->message = message;
	request->message_size = message_size;

	/* Succeeded: the request is an exit-signal. */
	return 0;
}
