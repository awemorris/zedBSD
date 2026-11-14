/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libFuzzer target: one connection's channel table (the server side) fed
 * a run of messages, each a uint16 length and the payload, as the session
 * process dispatches them; the answers go to a buffer emptied after each.
 */

#include <string.h>

#include "ssh-channel.h"
#include "ssh-ttymodes.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_channel_request request;
	struct ssh_buf out;
	struct termios terminal;
	const uint8_t *payload;
	const uint8_t *bytes;
	size_t length;
	size_t bytes_size;
	size_t room;
	uint32_t extended;
	char name[SSH_NAME_MAX + 1];
	int want_reply;
	int success;
	unsigned index;
	int error;

	/* Starts with an empty channel table and an empty answer buffer. */
	ssh_channels_init(&channels);
	ssh_buf_init(&out, SSH_PACKET_LIMIT, 0);

	/* Feeds the table one length-prefixed message at a time until the input is used up. */
	while (size >= 2) {
		length = ((size_t)data[0] << 8) | data[1];
		data += 2;
		size -= 2;
		if (length > size)
			length = size;
		payload = data;
		data += length;
		size -= length;
		ssh_buf_reset(&out);
		if (length == 0)
			continue;
		switch (payload[0]) {
		case SSH_MSG_GLOBAL_REQUEST:
			(void)ssh_channel_global_request(&channels, payload, length, name, &want_reply, &out);
			break;
		case SSH_MSG_CHANNEL_OPEN:
			(void)ssh_channel_accept_open(&channels, payload, length, &out, &channel);
			break;
		case SSH_MSG_CHANNEL_WINDOW_ADJUST:
			(void)ssh_channel_parse_window_adjust(&channels, payload, length, &channel);
			break;
		case SSH_MSG_CHANNEL_DATA:
		case SSH_MSG_CHANNEL_EXTENDED_DATA:
			/* Parses the message as data for a channel. */
			error = ssh_channel_parse_data(&channels, payload, length, &channel, &bytes, &bytes_size, &extended);

			/* Accounts the data and answers with an echo when the peer's window has room. */
			if (error == 0) {
				ssh_channel_consumed(channel, bytes_size, &out);
				room = ssh_channel_send_room(channel);
				if (room >= 4)
					(void)ssh_channel_put_data(channel, 0, (const uint8_t *)"echo", 4, &out);
			}

			break;
		case SSH_MSG_CHANNEL_EOF:
		case SSH_MSG_CHANNEL_CLOSE:
			/* Parses the message as an end of the channel's data or as its close. */
			error = ssh_channel_parse_end(&channels, payload, length, &channel);

			/* Answers with this side's end and close, then lets the table forget the channel. */
			if (error == 0) {
				ssh_channel_put_eof(channel, &out);
				ssh_channel_put_close(channel, &out);
				(void)ssh_channel_closed(&channels, channel);
			}

			break;
		case SSH_MSG_CHANNEL_REQUEST:
			/* Parses the message as a request on a channel. */
			error = ssh_channel_parse_request(&channels, payload, length, &request);

			/* Looks the channel up, decodes a terminal request's modes and answers when a reply is wanted. */
			if (error == 0) {
				channel = ssh_channel_find(&channels, request.recipient);
				if (request.type == SSH_CREQ_PTY) {
					memset(&terminal, 0, sizeof(terminal));
					(void)ssh_ttymodes_apply(request.modes, request.modes_size, &terminal);
				}

				/* Answers a request that asked for an answer. */
				if (channel != NULL && request.want_reply)
					ssh_channel_put_reply(channel, 0, &out);
			}

			break;
		case SSH_MSG_CHANNEL_SUCCESS:
		case SSH_MSG_CHANNEL_FAILURE:
			(void)ssh_channel_parse_reply(&channels, payload, length, &channel, &success);
			break;
		default:
			break;
		}
	}

	/* Clears the user pointers so that nothing is left dangling. */
	for (index = 0; index < SSH_CHANNEL_MAX; index++)
		channels.channel[index].user = NULL;

	/* Releases the answer buffer. */
	ssh_buf_free(&out);

	/* Succeeded: the whole input was dispatched without a crash. */
	return 0;
}
