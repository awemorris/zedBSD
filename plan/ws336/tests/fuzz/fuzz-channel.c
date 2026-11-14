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
	uint32_t extended;
	char name[SSH_NAME_MAX + 1];
	int want_reply;
	int success;
	unsigned index;

	ssh_channels_init(&channels);
	ssh_buf_init(&out, SSH_PACKET_LIMIT, 0);
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
			if (ssh_channel_parse_data(&channels, payload, length, &channel, &bytes, &bytes_size, &extended) == 0) {
				ssh_channel_consumed(channel, bytes_size, &out);
				if (ssh_channel_send_room(channel) >= 4)
					(void)ssh_channel_put_data(channel, 0, (const uint8_t *)"echo", 4, &out);
			}

			break;
		case SSH_MSG_CHANNEL_EOF:
		case SSH_MSG_CHANNEL_CLOSE:
			if (ssh_channel_parse_end(&channels, payload, length, &channel) == 0) {
				ssh_channel_put_eof(channel, &out);
				ssh_channel_put_close(channel, &out);
				(void)ssh_channel_closed(&channels, channel);
			}

			break;
		case SSH_MSG_CHANNEL_REQUEST:
			if (ssh_channel_parse_request(&channels, payload, length, &request) == 0) {
				channel = ssh_channel_find(&channels, request.recipient);
				if (request.type == SSH_CREQ_PTY) {
					memset(&terminal, 0, sizeof(terminal));
					(void)ssh_ttymodes_apply(request.modes, request.modes_size, &terminal);
				}

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

	for (index = 0; index < SSH_CHANNEL_MAX; index++)
		channels.channel[index].user = NULL;
	ssh_buf_free(&out);
	return 0;
}
