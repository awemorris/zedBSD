/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's L2CAP signalling (ws143-p004, see l2cap.h).
 *
 * Every length a command gives is checked against the C-frame it came in;
 * a command that runs past it ends the frame's reading.
 */

#include "userland/base/bluetoothd/l2cap.h"

#include <errno.h>
#include <string.h>

/* The signalling commands (Core 5.4 Vol 3 Part A §4). */
#define SIGNAL_COMMAND_REJECT		0x01U
#define SIGNAL_CONNECTION_REQUEST	0x02U
#define SIGNAL_CONNECTION_RESPONSE	0x03U
#define SIGNAL_CONFIGURE_REQUEST	0x04U
#define SIGNAL_CONFIGURE_RESPONSE	0x05U
#define SIGNAL_DISCONNECTION_REQUEST	0x06U
#define SIGNAL_DISCONNECTION_RESPONSE	0x07U
#define SIGNAL_ECHO_REQUEST		0x08U
#define SIGNAL_ECHO_RESPONSE		0x09U
#define SIGNAL_INFORMATION_REQUEST	0x0aU
#define SIGNAL_INFORMATION_RESPONSE	0x0bU
#define SIGNAL_PARAMETER_REQUEST	0x12U
#define SIGNAL_PARAMETER_RESPONSE	0x13U

/* A command's header: code, identifier, length. */
#define SIGNAL_HEADER			4U

/* The results bluetoothd answers with. */
#define SIGNAL_PSM_NOT_SUPPORTED	0x0002U
#define SIGNAL_CONFIG_UNACCEPTABLE	0x0001U
#define SIGNAL_CONFIG_UNKNOWN		0x0003U
#define SIGNAL_INFO_NOT_SUPPORTED	0x0001U

/* The configuration option bluetoothd reads, and the hint bit of an option's type. */
#define SIGNAL_OPTION_MTU		0x01U
#define SIGNAL_OPTION_HINT		0x80U

/* The information types: the extended features (fixed channels supported) and the fixed channels (signalling). */
#define SIGNAL_INFO_FEATURES		0x0002U
#define SIGNAL_INFO_FIXED		0x0003U
#define SIGNAL_FEATURE_FIXED		0x00000080U
#define SIGNAL_FIXED_SIGNALLING		0x02U

/* The bounds of an LE connection's parameters (Core Vol 6 Part B §4.5.1, Vol 4 Part E §7.8.12). */
#define SIGNAL_INTERVAL_MIN		6U
#define SIGNAL_INTERVAL_MAX		3200U
#define SIGNAL_LATENCY_MAX		499U
#define SIGNAL_TIMEOUT_MIN		10U
#define SIGNAL_TIMEOUT_MAX		3200U

/* An answer being written: its buffer, its room and how much is used. */
struct signal_out {
	uint8_t *bytes;
	size_t size;
	size_t used;
};

static int signal_put(struct signal_out *out, uint8_t code, uint8_t identifier, const uint8_t *data, size_t length);
static int signal_command(struct btd_l2cap *l2cap, uint16_t handle, int le, uint8_t code, uint8_t identifier, const uint8_t *data, size_t length, struct signal_out *out, struct btd_signal_effect *effect);
static int signal_connection_response(struct btd_l2cap *l2cap, const uint8_t *data, size_t length, struct signal_out *out);
static int signal_configure_request(struct btd_l2cap *l2cap, uint8_t identifier, const uint8_t *data, size_t length, struct signal_out *out);
static int signal_parameters(uint8_t identifier, const uint8_t *data, size_t length, struct signal_out *out, struct btd_signal_effect *effect);
static int signal_information(uint8_t identifier, const uint8_t *data, size_t length, struct signal_out *out);
static int signal_reject(uint8_t identifier, struct signal_out *out);
static struct btd_channel *signal_find(struct btd_l2cap *l2cap, uint16_t local_cid);
static uint16_t signal_le16(const uint8_t *bytes);
static void signal_put16(uint8_t *bytes, uint16_t value);
static uint8_t signal_identifier(struct btd_l2cap *l2cap);
static void signal_information_answer(struct btd_l2cap *l2cap, uint8_t identifier, const uint8_t *data, size_t length, struct btd_signal_effect *effect);

/*
 * Empties the table of channels.
 */
void
btd_l2cap_init(
	struct btd_l2cap *l2cap)
{
	/* No channel; identifiers start at 1 (0 is not one). */
	memset(l2cap, 0, sizeof(*l2cap));
	l2cap->next_identifier = 1U;
}

/*
 * Reads a signalling C-frame's payload (BR/EDR: several commands; LE: one)
 * and writes the answers and the requests that follow from it into
 * answer.  Returns 0, or -1 for a command that runs past the payload (the
 * commands before it are answered) or answers that do not fit.
 */
int
btd_l2cap_signal(
	struct btd_l2cap *l2cap,
	uint16_t handle,
	int le,
	const uint8_t *payload,
	size_t length,
	uint8_t *answer,
	size_t size,
	size_t *answer_length,
	struct btd_signal_effect *effect)
{
	struct signal_out out;
	size_t offset;
	size_t data_length;
	int error;

	/* Nothing written, nothing asked yet. */
	out.bytes = answer;
	out.size = size;
	out.used = 0U;
	memset(effect, 0, sizeof(*effect));
	*answer_length = 0U;

	/* Each command in turn. */
	offset = 0U;
	while (offset < length) {
		/* Its header and its data must be in the payload. */
		if (length - offset < SIGNAL_HEADER) {
			l2cap->malformed++;
			*answer_length = out.used;
			return -1;
		}

		/* The command's length must fit in the frame. */
		data_length = signal_le16(payload + offset + 2U);
		if (data_length > length - offset - SIGNAL_HEADER) {
			l2cap->malformed++;
			*answer_length = out.used;
			return -1;
		}

		/* The command, answered. */
		error = signal_command(l2cap, handle, le, payload[offset], payload[offset + 1U], payload + offset + SIGNAL_HEADER, data_length, &out, effect);
		if (error != 0) {
			*answer_length = out.used;
			return -1;
		}

		/* LE carries one command a frame. */
		offset += SIGNAL_HEADER + data_length;
		if (le)
			break;
	}

	/* Succeeded: the answers. */
	*answer_length = out.used;
	return 0;
}

/*
 * Opens a channel to a PSM on a connection: a free slot, and the
 * Connection Request to send.  Returns 0 with the request and the
 * channel's local CID, ENOSPC when the table is full, or EMSGSIZE.
 */
int
btd_l2cap_connect(
	struct btd_l2cap *l2cap,
	uint16_t handle,
	uint16_t psm,
	uint8_t *request,
	size_t size,
	size_t *request_length,
	uint16_t *local_cid)
{
	struct btd_channel *channel;
	struct signal_out out;
	uint8_t data[4];
	unsigned index;
	int error;

	/* A free slot. */
	channel = NULL;
	for (index = 0U; index < BTD_CHANNELS_MAX; index++) {
		if (l2cap->channels[index].state == BTD_CHANNEL_FREE) {
			channel = &l2cap->channels[index];
			break;
		}
	}

	/* A full table. */
	if (channel == NULL)
		return ENOSPC;

	/* The channel, connecting. */
	memset(channel, 0, sizeof(*channel));
	channel->state = BTD_CHANNEL_CONNECTING;
	channel->handle = handle;
	channel->local_cid = (uint16_t)(BTD_CID_DYNAMIC + index);
	channel->psm = psm;
	channel->identifier = signal_identifier(l2cap);

	/* The request: the PSM and the source CID. */
	signal_put16(data, psm);
	signal_put16(data + 2, channel->local_cid);
	out.bytes = request;
	out.size = size;
	out.used = 0U;
	error = signal_put(&out, SIGNAL_CONNECTION_REQUEST, channel->identifier, data, sizeof(data));
	if (error != 0) {
		channel->state = BTD_CHANNEL_FREE;
		return EMSGSIZE;
	}

	/* Succeeded: the request and the channel. */
	*request_length = out.used;
	*local_cid = channel->local_cid;
	return 0;
}

/*
 * Builds an Information Request for the extended features on BR/EDR's
 * signalling channel (the payload of a frame on CID 1) and remembers its
 * identifier, so that the answer comes back as the effect's information.
 * Returns 0, or ENOBUFS when the request does not fit.
 */
int
btd_l2cap_information(
	struct btd_l2cap *l2cap,
	uint8_t *request,
	size_t size,
	size_t *request_length)
{
	uint8_t type[2];
	struct signal_out out;
	uint8_t identifier;
	int error;

	/* The request's place. */
	out.bytes = request;
	out.size = size;
	out.used = 0U;
	*request_length = 0U;

	/* The extended features, under a new identifier. */
	identifier = signal_identifier(l2cap);
	signal_put16(type, SIGNAL_INFO_FEATURES);
	error = signal_put(&out, SIGNAL_INFORMATION_REQUEST, identifier, type, sizeof(type));
	if (error != 0)
		return error;

	/* The answer is awaited under that identifier. */
	l2cap->information_pending = 1;
	l2cap->information_identifier = identifier;

	/* Succeeded: the request. */
	*request_length = out.used;
	return 0;
}

/*
 * Closes a channel: the Disconnection Request to send.  Returns 0, ENOENT
 * for no such channel, or EMSGSIZE.
 */
int
btd_l2cap_disconnect(
	struct btd_l2cap *l2cap,
	uint16_t local_cid,
	uint8_t *request,
	size_t size,
	size_t *request_length)
{
	struct btd_channel *channel;
	struct signal_out out;
	uint8_t data[4];
	int error;

	/* The channel. */
	channel = signal_find(l2cap, local_cid);
	if (channel == NULL)
		return ENOENT;

	/* The request: the destination (theirs) and the source (ours). */
	signal_put16(data, channel->remote_cid);
	signal_put16(data + 2, channel->local_cid);
	channel->identifier = signal_identifier(l2cap);
	out.bytes = request;
	out.size = size;
	out.used = 0U;
	error = signal_put(&out, SIGNAL_DISCONNECTION_REQUEST, channel->identifier, data, sizeof(data));
	if (error != 0)
		return EMSGSIZE;

	/* Succeeded: closing. */
	channel->state = BTD_CHANNEL_CLOSING;
	*request_length = out.used;
	return 0;
}

/*
 * Finds a channel by its local CID, or NULL.
 */
struct btd_channel *
btd_l2cap_channel(
	struct btd_l2cap *l2cap,
	uint16_t local_cid)
{
	/* The table's search. */
	return signal_find(l2cap, local_cid);
}

/*
 * Frees every channel of a connection that went.
 */
void
btd_l2cap_drop(
	struct btd_l2cap *l2cap,
	uint16_t handle)
{
	unsigned index;

	/* Each channel of the connection. */
	for (index = 0U; index < BTD_CHANNELS_MAX; index++) {
		if (l2cap->channels[index].state != BTD_CHANNEL_FREE && l2cap->channels[index].handle == handle)
			l2cap->channels[index].state = BTD_CHANNEL_FREE;
	}
}

/* Appends one command to an answer; returns -1 when it does not fit. */
static int
signal_put(
	struct signal_out *out,
	uint8_t code,
	uint8_t identifier,
	const uint8_t *data,
	size_t length)
{
	/* Room for the header and the data. */
	if (out->size - out->used < SIGNAL_HEADER + length)
		return -1;

	/* The header and the data. */
	out->bytes[out->used] = code;
	out->bytes[out->used + 1U] = identifier;
	signal_put16(out->bytes + out->used + 2U, (uint16_t)length);
	if (length != 0U)
		memcpy(out->bytes + out->used + SIGNAL_HEADER, data, length);
	out->used += SIGNAL_HEADER + length;

	/* Succeeded: appended. */
	return 0;
}

/* Answers one command; returns -1 when the answer does not fit. */
static int
signal_command(
	struct btd_l2cap *l2cap,
	uint16_t handle,
	int le,
	uint8_t code,
	uint8_t identifier,
	const uint8_t *data,
	size_t length,
	struct signal_out *out,
	struct btd_signal_effect *effect)
{
	struct btd_channel *channel;
	uint8_t response[8];
	uint16_t result;
	unsigned index;
	int error;

	/* The connection is the channel table's business only for the requests bluetoothd makes. */
	(void)handle;

	/* LE's channel has the parameter update and the reject; anything else is rejected. */
	if (le) {
		if (code == SIGNAL_PARAMETER_REQUEST) {
			error = signal_parameters(identifier, data, length, out, effect);
			return error;
		}

		/* Any other command is rejected, except a reject itself. */
		if (code == SIGNAL_COMMAND_REJECT)
			return 0;
		error = signal_reject(identifier, out);
		return error;
	}

	/* BR/EDR's commands. */
	error = 0;
	switch (code) {
	case SIGNAL_COMMAND_REJECT:
		/* A request of ours refused: its channel goes. */
		for (index = 0U; index < BTD_CHANNELS_MAX; index++) {
			channel = &l2cap->channels[index];
			if (channel->state != BTD_CHANNEL_FREE && channel->identifier == identifier)
				channel->state = BTD_CHANNEL_FREE;
		}

		break;
	case SIGNAL_CONNECTION_REQUEST:
		/* A channel the other side asks for is refused in this Phase (PSM not supported). */
		if (length < 4U) {
			error = signal_reject(identifier, out);
			break;
		}

		/* A request for a channel: refused, no PSM is served. */
		l2cap->rejected++;
		signal_put16(response, 0U);
		signal_put16(response + 2, signal_le16(data + 2));
		signal_put16(response + 4, SIGNAL_PSM_NOT_SUPPORTED);
		signal_put16(response + 6, 0U);
		error = signal_put(out, SIGNAL_CONNECTION_RESPONSE, identifier, response, sizeof(response));
		break;
	case SIGNAL_CONNECTION_RESPONSE:
		error = signal_connection_response(l2cap, data, length, out);
		break;
	case SIGNAL_CONFIGURE_REQUEST:
		error = signal_configure_request(l2cap, identifier, data, length, out);
		break;
	case SIGNAL_CONFIGURE_RESPONSE:
		/* Our configuration accepted (or not: the channel closes). */
		if (length < 6U)
			break;
		channel = signal_find(l2cap, signal_le16(data));
		if (channel == NULL || channel->state != BTD_CHANNEL_CONFIGURING)
			break;
		result = signal_le16(data + 4);
		if (result != 0U) {
			channel->state = BTD_CHANNEL_FREE;
			break;
		}

		/* Our configuration accepted. */
		channel->local_done = 1;
		if (channel->remote_done)
			channel->state = BTD_CHANNEL_OPEN;
		break;
	case SIGNAL_DISCONNECTION_REQUEST:
		/* The other side closes a channel: answered with the same two CIDs, and freed. */
		if (length < 4U) {
			error = signal_reject(identifier, out);
			break;
		}

		/* The other side closed the channel: it is free. */
		channel = signal_find(l2cap, signal_le16(data));
		if (channel != NULL)
			channel->state = BTD_CHANNEL_FREE;
		error = signal_put(out, SIGNAL_DISCONNECTION_RESPONSE, identifier, data, 4U);
		break;
	case SIGNAL_DISCONNECTION_RESPONSE:
		/* Our close done. */
		if (length < 4U)
			break;
		channel = signal_find(l2cap, signal_le16(data + 2));
		if (channel != NULL)
			channel->state = BTD_CHANNEL_FREE;
		break;
	case SIGNAL_ECHO_REQUEST:
		/* Echoed (the data, as much as fits). */
		if (length > BTD_SIGNAL_MAX)
			length = BTD_SIGNAL_MAX;
		error = signal_put(out, SIGNAL_ECHO_RESPONSE, identifier, data, length);
		break;
	case SIGNAL_INFORMATION_REQUEST:
		error = signal_information(identifier, data, length, out);
		break;
	case SIGNAL_INFORMATION_RESPONSE:
		/* The answer to bluetoothd's own request, if it is that one. */
		signal_information_answer(l2cap, identifier, data, length, effect);
		break;
	case SIGNAL_ECHO_RESPONSE:
		/* An answer to a request bluetoothd does not send. */
		break;
	default:
		/* Not understood. */
		error = signal_reject(identifier, out);
		break;
	}

	/* Succeeded, or the answer did not fit. */
	return error;
}

/* Takes a Connection Response to a request of ours: on success the channel is configured (our Configure Request goes). */
static int
signal_connection_response(
	struct btd_l2cap *l2cap,
	const uint8_t *data,
	size_t length,
	struct signal_out *out)
{
	struct btd_channel *channel;
	uint8_t request[8];
	uint16_t result;
	int error;

	/* The destination, the source (ours) and the result. */
	if (length < 8U)
		return 0;
	channel = signal_find(l2cap, signal_le16(data + 2));
	if (channel == NULL || channel->state != BTD_CHANNEL_CONNECTING)
		return 0;
	result = signal_le16(data + 4);

	/* Pending: the final answer comes later. */
	if (result == 0x0001U)
		return 0;

	/* Refused: the channel goes. */
	if (result != 0U) {
		channel->state = BTD_CHANNEL_FREE;
		return 0;
	}

	/* Connected: our configuration, the MTU we take. */
	channel->remote_cid = signal_le16(data);
	channel->state = BTD_CHANNEL_CONFIGURING;
	channel->identifier = signal_identifier(l2cap);
	signal_put16(request, channel->remote_cid);
	signal_put16(request + 2, 0U);
	request[4] = SIGNAL_OPTION_MTU;
	request[5] = 2U;
	signal_put16(request + 6, BTD_L2CAP_MTU);
	error = signal_put(out, SIGNAL_CONFIGURE_REQUEST, channel->identifier, request, sizeof(request));

	/* Succeeded, or the request did not fit. */
	return error;
}

/*
 * Answers the other side's Configure Request: the MTU is taken when it is
 * at least BTD_L2CAP_MTU_MIN, an unknown option that is not a hint is
 * refused with its type.
 */
static int
signal_configure_request(
	struct btd_l2cap *l2cap,
	uint8_t identifier,
	const uint8_t *data,
	size_t length,
	struct signal_out *out)
{
	struct btd_channel *channel;
	uint8_t response[16];
	uint16_t result;
	uint16_t flags;
	uint16_t mtu;
	size_t offset;
	size_t answered;
	uint8_t type;
	uint8_t size;
	int error;

	/* The destination (ours) and the flags. */
	if (length < 4U) {
		error = signal_reject(identifier, out);
		return error;
	}

	/* The channel being configured. */
	channel = signal_find(l2cap, signal_le16(data));
	if (channel == NULL || channel->state != BTD_CHANNEL_CONFIGURING) {
		error = signal_reject(identifier, out);
		return error;
	}

	/* The options: [type][length][value]; the MTU is read, a hint is passed over, another is refused. */
	result = 0U;
	mtu = BTD_L2CAP_MTU;
	answered = 6U;
	offset = 4U;
	while (offset + 2U <= length) {
		type = data[offset];
		size = data[offset + 1U];
		if (offset + 2U + size > length)
			break;
		if (type == SIGNAL_OPTION_MTU && size == 2U) {
			mtu = signal_le16(data + offset + 2U);
		} else if ((type & SIGNAL_OPTION_HINT) == 0U && result == 0U) {
			result = SIGNAL_CONFIG_UNKNOWN;
			response[answered] = type;
			answered++;
		}

		/* The next option. */
		offset += 2U + size;
	}

	/* An MTU below the least is refused with the least. */
	if (result == 0U && mtu < BTD_L2CAP_MTU_MIN) {
		result = SIGNAL_CONFIG_UNACCEPTABLE;
		response[answered] = SIGNAL_OPTION_MTU;
		response[answered + 1U] = 2U;
		signal_put16(response + answered + 2U, BTD_L2CAP_MTU_MIN);
		answered += 4U;
	}

	/* The response: the source (theirs), no flags, the result. */
	signal_put16(response, channel->remote_cid);
	signal_put16(response + 2, 0U);
	signal_put16(response + 4, result);
	error = signal_put(out, SIGNAL_CONFIGURE_RESPONSE, identifier, response, answered);
	if (error != 0)
		return error;

	/* Their side is configured when accepted (a request with the continuation flag waits for its rest). */
	flags = signal_le16(data + 2);
	if (result == 0U && (flags & 0x0001U) == 0U) {
		channel->remote_mtu = mtu;
		channel->remote_done = 1;
		if (channel->local_done)
			channel->state = BTD_CHANNEL_OPEN;
	}

	/* Succeeded: answered. */
	return 0;
}

/*
 * Answers LE's Connection Parameter Update Request: parameters in their
 * bounds are accepted and given to the caller to carry out; others are
 * rejected.
 */
static int
signal_parameters(
	uint8_t identifier,
	const uint8_t *data,
	size_t length,
	struct signal_out *out,
	struct btd_signal_effect *effect)
{
	uint8_t response[2];
	uint16_t minimum;
	uint16_t maximum;
	uint16_t latency;
	uint16_t timeout;
	uint32_t needed;
	uint16_t result;
	int accepted;
	int error;

	/* The four parameters. */
	if (length != 8U) {
		error = signal_reject(identifier, out);
		return error;
	}

	/* The parameters asked for. */
	minimum = signal_le16(data);
	maximum = signal_le16(data + 2);
	latency = signal_le16(data + 4);
	timeout = signal_le16(data + 6);

	/* In their bounds, the timeout longer than the latency's span (timeout x 10 ms > (1 + latency) x max x 1.25 ms x 2). */
	accepted = 1;
	if (minimum < SIGNAL_INTERVAL_MIN || maximum > SIGNAL_INTERVAL_MAX || minimum > maximum)
		accepted = 0;
	if (latency > SIGNAL_LATENCY_MAX || timeout < SIGNAL_TIMEOUT_MIN || timeout > SIGNAL_TIMEOUT_MAX)
		accepted = 0;
	needed = (uint32_t)(1U + latency) * (uint32_t)maximum;
	if ((uint32_t)timeout * 4U <= needed)
		accepted = 0;

	/* The answer: 0 accepted, 1 rejected. */
	result = 0x0000U;
	if (!accepted)
		result = 0x0001U;
	signal_put16(response, result);
	error = signal_put(out, SIGNAL_PARAMETER_RESPONSE, identifier, response, sizeof(response));
	if (error != 0)
		return error;

	/* An accepted update is the caller's to carry out. */
	if (accepted) {
		effect->update = 1;
		effect->interval_min = minimum;
		effect->interval_max = maximum;
		effect->latency = latency;
		effect->timeout = timeout;
	}

	/* Succeeded: answered. */
	return 0;
}

/* Answers an Information Request: the extended features and the fixed channels; anything else is not supported. */
static int
signal_information(
	uint8_t identifier,
	const uint8_t *data,
	size_t length,
	struct signal_out *out)
{
	uint8_t response[12];
	uint16_t type;
	size_t answered;
	int error;

	/* The type asked for. */
	if (length < 2U) {
		error = signal_reject(identifier, out);
		return error;
	}

	/* The type asked about. */
	type = signal_le16(data);

	/* The answer for each type. */
	memset(response, 0, sizeof(response));
	signal_put16(response, type);
	answered = 4U;
	if (type == SIGNAL_INFO_FEATURES) {
		response[4] = (uint8_t)(SIGNAL_FEATURE_FIXED & 0xffU);
		answered = 8U;
	} else if (type == SIGNAL_INFO_FIXED) {
		response[4] = SIGNAL_FIXED_SIGNALLING;
		answered = 12U;
	} else {
		signal_put16(response + 2, SIGNAL_INFO_NOT_SUPPORTED);
	}

	/* Written. */
	error = signal_put(out, SIGNAL_INFORMATION_RESPONSE, identifier, response, answered);
	return error;
}

/* Answers a command with Command Reject (not understood). */
static int
signal_reject(
	uint8_t identifier,
	struct signal_out *out)
{
	uint8_t reason[2];
	int error;

	/* Reason 0: command not understood. */
	signal_put16(reason, 0U);
	error = signal_put(out, SIGNAL_COMMAND_REJECT, identifier, reason, sizeof(reason));
	return error;
}

/* Finds a channel by its local CID, or NULL. */
static struct btd_channel *
signal_find(
	struct btd_l2cap *l2cap,
	uint16_t local_cid)
{
	unsigned index;

	/* The CID names the slot. */
	if (local_cid < BTD_CID_DYNAMIC)
		return NULL;
	index = (unsigned)(local_cid - BTD_CID_DYNAMIC);
	if (index >= BTD_CHANNELS_MAX)
		return NULL;
	if (l2cap->channels[index].state == BTD_CHANNEL_FREE)
		return NULL;

	/* Succeeded: the channel. */
	return &l2cap->channels[index];
}

/* Reads a little-endian 16-bit value. */
static uint16_t
signal_le16(
	const uint8_t *bytes)
{
	/* The two bytes. */
	return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

/* Writes a little-endian 16-bit value. */
static void
signal_put16(
	uint8_t *bytes,
	uint16_t value)
{
	/* The two bytes. */
	bytes[0] = (uint8_t)(value & 0xffU);
	bytes[1] = (uint8_t)(value >> 8);
}

/* Gives the next request identifier (1 to 255; 0 is not one). */
static uint8_t
signal_identifier(
	struct btd_l2cap *l2cap)
{
	uint8_t identifier;

	/* The next one, wrapping past 0. */
	identifier = l2cap->next_identifier;
	l2cap->next_identifier++;
	if (l2cap->next_identifier == 0U)
		l2cap->next_identifier = 1U;

	/* Succeeded. */
	return identifier;
}

/*
 * Takes an Information Response: the answer to bluetoothd's request when
 * its identifier is the one awaited (the result, and the features when
 * they were given); any other is passed over.
 */
static void
signal_information_answer(
	struct btd_l2cap *l2cap,
	uint8_t identifier,
	const uint8_t *data,
	size_t length,
	struct btd_signal_effect *effect)
{
	/* Only the answer awaited, with its type and result. */
	if (!l2cap->information_pending || identifier != l2cap->information_identifier)
		return;
	if (length < 4U)
		return;

	/* Answered: no other answer under that identifier is taken. */
	l2cap->information_pending = 0;
	effect->information = 1;
	effect->information_result = signal_le16(data + 2);
	effect->features = 0U;

	/* The features' mask, when it is there. */
	if (effect->information_result == 0U && length >= 8U) {
		effect->features = (uint32_t)data[4] |
				   ((uint32_t)data[5] << 8) |
				   ((uint32_t)data[6] << 16) |
				   ((uint32_t)data[7] << 24);
	}
}
