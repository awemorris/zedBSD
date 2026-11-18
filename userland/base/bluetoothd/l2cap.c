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
#define SIGNAL_CONFIG_UNACCEPTABLE	0x0001U
#define SIGNAL_CONFIG_UNKNOWN		0x0003U
#define SIGNAL_INFO_NOT_SUPPORTED	0x0001U

/*
 * The configuration options bluetoothd reads (Core 5.4 Vol 3 Part A §5):
 * the MTU, the Flush Timeout, the QoS, the retransmission and flow
 * control (its mode byte first; 0 is the basic mode), the FCS; their
 * lengths; and the hint bit of an option's type.
 */
#define SIGNAL_OPTION_MTU		0x01U
#define SIGNAL_OPTION_FLUSH		0x02U
#define SIGNAL_OPTION_QOS		0x03U
#define SIGNAL_OPTION_RFC		0x04U
#define SIGNAL_OPTION_FCS		0x05U
#define SIGNAL_OPTION_HINT		0x80U
#define SIGNAL_FLUSH_LENGTH		2U
#define SIGNAL_QOS_LENGTH		22U
#define SIGNAL_RFC_LENGTH		9U
#define SIGNAL_FCS_LENGTH		1U
#define SIGNAL_RFC_BASIC		0x00U

/* The room of a Configure Response: the header's 6 bytes and the options it sends back (review M11). */
#define SIGNAL_CONFIGURE_ANSWER		48U

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
static int signal_connection_request(struct btd_l2cap *l2cap, uint16_t handle, uint8_t identifier, const uint8_t *data, size_t length, struct signal_out *out);
static int signal_connection_response(struct btd_l2cap *l2cap, const uint8_t *data, size_t length, struct signal_out *out, struct btd_signal_effect *effect);
static int signal_configure_request(struct btd_l2cap *l2cap, uint16_t handle, uint8_t identifier, const uint8_t *data, size_t length, struct signal_out *out, struct btd_signal_effect *effect);
static int signal_configure_send(struct btd_l2cap *l2cap, struct btd_channel *channel, struct signal_out *out);
static struct btd_channel *signal_free_slot(struct btd_l2cap *l2cap, unsigned *index);
static void signal_opened(struct btd_signal_effect *effect, const struct btd_channel *channel);
static void signal_closed(struct btd_signal_effect *effect, struct btd_channel *channel, unsigned reason);
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

	/* Each channel of the connection, with any mark of a move. */
	for (index = 0U; index < BTD_CHANNELS_MAX; index++) {
		if (l2cap->channels[index].state != BTD_CHANNEL_FREE && l2cap->channels[index].handle == handle) {
			l2cap->channels[index].state = BTD_CHANNEL_FREE;
			l2cap->channels[index].left = 0;
		}
	}
}

/*
 * Gives a table the owner's answer to the channels the other side asks
 * for (NULL: every one is refused as PSM not supported).  btd_l2cap_init
 * clears it.
 */
void
btd_l2cap_set_accept(
	struct btd_l2cap *l2cap,
	btd_l2cap_accept_fn accept,
	void *context)
{
	/* The hook and its context. */
	l2cap->accept = accept;
	l2cap->accept_context = context;
}

/*
 * Gives the final answer to every channel of a connection that the other
 * side asked for and that was answered Pending (phase005 section 9.8):
 * success configures it (our Configure Request follows), anything else
 * refuses it and frees its slot.  The commands go in answer (nothing when
 * no channel was pending).  Returns 0, or EMSGSIZE when they do not fit
 * (the channels not answered stay pending).
 */
int
btd_l2cap_answer_pending(
	struct btd_l2cap *l2cap,
	uint16_t handle,
	uint16_t result,
	uint8_t *answer,
	size_t size,
	size_t *answer_length)
{
	struct btd_channel *channel;
	struct signal_out out;
	uint8_t response[8];
	unsigned index;
	int error;

	/* Nothing written yet. */
	out.bytes = answer;
	out.size = size;
	out.used = 0U;
	*answer_length = 0U;

	/* Each pending channel of the connection. */
	for (index = 0U; index < BTD_CHANNELS_MAX; index++) {
		channel = &l2cap->channels[index];
		if (channel->state != BTD_CHANNEL_PENDING || channel->handle != handle)
			continue;

		/* The final response, under their request's identifier: our CID only when accepted. */
		if (result == BTD_L2CAP_SUCCESS) {
			signal_put16(response, channel->local_cid);
		} else {
			signal_put16(response, 0U);
		}

		/* Their CID, the result, no further information. */
		signal_put16(response + 2, channel->remote_cid);
		signal_put16(response + 4, result);
		signal_put16(response + 6, 0U);
		error = signal_put(&out, SIGNAL_CONNECTION_RESPONSE, channel->identifier, response, sizeof(response));
		if (error != 0) {
			*answer_length = out.used;
			return EMSGSIZE;
		}

		/* A refusal frees the slot. */
		if (result != BTD_L2CAP_SUCCESS) {
			l2cap->rejected++;
			channel->state = BTD_CHANNEL_FREE;
			continue;
		}

		/* Accepted: our Configure Request follows. */
		error = signal_configure_send(l2cap, channel, &out);
		if (error != 0) {
			*answer_length = out.used;
			return EMSGSIZE;
		}
	}

	/* Succeeded: the answers. */
	*answer_length = out.used;
	return 0;
}

/*
 * Moves every channel of a connection from one table to another
 * (ws197-p002 section 7.3: the pairing's channels to the phone link's at
 * the handoff): each goes to the same slot of the other table, so its
 * local CID (the slot's number) stays, with its state, its CIDs, its MTU,
 * its identifier and its direction.  The request identifiers go on from
 * the first table's, and its outstanding Information and Echo Requests go
 * with them.  A channel whose slot is taken in the other table stays,
 * marked to be refused (btd_l2cap_refuse_left).  The counts of those moved
 * and left are returned.
 */
void
btd_l2cap_move(
	struct btd_l2cap *from,
	struct btd_l2cap *to,
	uint16_t handle,
	unsigned *moved,
	unsigned *left)
{
	struct btd_channel *source;
	struct btd_channel *target;
	unsigned index;

	/* Each channel of the connection. */
	*moved = 0U;
	*left = 0U;
	for (index = 0U; index < BTD_CHANNELS_MAX; index++) {
		source = &from->channels[index];
		if (source->state == BTD_CHANNEL_FREE || source->handle != handle)
			continue;

		/* A taken slot: the channel stays to be refused. */
		target = &to->channels[index];
		if (target->state != BTD_CHANNEL_FREE) {
			source->left = 1;
			(*left)++;
			continue;
		}

		/* Copied whole into the same slot, then freed here. */
		*target = *source;
		target->left = 0;
		source->state = BTD_CHANNEL_FREE;
		source->left = 0;
		(*moved)++;
	}

	/* The identifiers go on, with the requests still answered to the first table. */
	to->next_identifier = from->next_identifier;
	to->information_pending = from->information_pending;
	to->information_identifier = from->information_identifier;
	to->echo_pending = from->echo_pending;
	to->echo_identifier = from->echo_identifier;
}

/*
 * Builds the refusal of one channel a move left in its table (section
 * 7.3), and frees it: an open or configuring channel gets a Disconnection
 * Request, a pending one the Connection Response "no resources"; one that
 * is connecting or closing is freed without a command (nothing of it can
 * be answered once the table no longer routes the connection).  The caller
 * sends each command in a frame of its own (the smallest signalling MTU
 * holds one).  Returns 0 with *length the command's bytes (0 when no
 * channel is left), or EMSGSIZE when the command does not fit.
 */
int
btd_l2cap_refuse_left(
	struct btd_l2cap *from,
	uint16_t handle,
	uint8_t *answer,
	size_t size,
	size_t *length)
{
	struct btd_channel *channel;
	struct signal_out out;
	uint8_t data[8];
	uint8_t identifier;
	unsigned index;
	int error;

	/* Nothing written yet. */
	out.bytes = answer;
	out.size = size;
	out.used = 0U;
	*length = 0U;

	/* The first channel of the connection left by a move. */
	for (index = 0U; index < BTD_CHANNELS_MAX; index++) {
		channel = &from->channels[index];
		if (channel->state == BTD_CHANNEL_FREE || channel->handle != handle || !channel->left)
			continue;

		/* An open or configuring channel: Disconnection Request, their CID then ours. */
		error = 0;
		if (channel->state == BTD_CHANNEL_OPEN || channel->state == BTD_CHANNEL_CONFIGURING) {
			signal_put16(data, channel->remote_cid);
			signal_put16(data + 2, channel->local_cid);
			identifier = signal_identifier(from);
			error = signal_put(&out, SIGNAL_DISCONNECTION_REQUEST, identifier, data, 4U);
		} else if (channel->state == BTD_CHANNEL_PENDING) {
			/* A pending one: Connection Response under their identifier, no CID, no resources. */
			signal_put16(data, 0U);
			signal_put16(data + 2, channel->remote_cid);
			signal_put16(data + 4, BTD_L2CAP_NO_RESOURCES);
			signal_put16(data + 6, 0U);
			error = signal_put(&out, SIGNAL_CONNECTION_RESPONSE, channel->identifier, data, 8U);
		}

		/* A command that does not fit keeps the channel for the next call. */
		if (error != 0)
			return EMSGSIZE;

		/* Succeeded: refused (or dropped), and freed. */
		from->rejected++;
		channel->state = BTD_CHANNEL_FREE;
		channel->left = 0;
		*length = out.used;
		return 0;
	}

	/* Succeeded: nothing is left. */
	return 0;
}

/*
 * Builds an Echo Request (without data) on BR/EDR's signalling channel and
 * remembers its identifier, so that its answer comes back as the effect's
 * echo (a resumed link is alive, phase005 section 4.9).  Returns 0, or
 * ENOBUFS when the request does not fit.
 */
int
btd_l2cap_echo(
	struct btd_l2cap *l2cap,
	uint8_t *request,
	size_t size,
	size_t *request_length)
{
	struct signal_out out;
	uint8_t identifier;
	int error;

	/* The request's place. */
	out.bytes = request;
	out.size = size;
	out.used = 0U;
	*request_length = 0U;

	/* The request, under a new identifier. */
	identifier = signal_identifier(l2cap);
	error = signal_put(&out, SIGNAL_ECHO_REQUEST, identifier, NULL, 0U);
	if (error != 0)
		return ENOBUFS;

	/* Its answer is awaited under that identifier. */
	l2cap->echo_pending = 1;
	l2cap->echo_identifier = identifier;

	/* Succeeded: the request. */
	*request_length = out.used;
	return 0;
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
	uint16_t result;
	unsigned index;
	int error;

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
		/* A request of ours refused: its channel goes (a pending channel of theirs holds their identifier, not ours). */
		for (index = 0U; index < BTD_CHANNELS_MAX; index++) {
			channel = &l2cap->channels[index];
			if (channel->state == BTD_CHANNEL_FREE || channel->state == BTD_CHANNEL_PENDING)
				continue;
			if (channel->handle != handle || channel->identifier != identifier)
				continue;
			signal_closed(effect, channel, BTD_L2CAP_CLOSED_REFUSED);
		}

		break;
	case SIGNAL_CONNECTION_REQUEST:
		error = signal_connection_request(l2cap, handle, identifier, data, length, out);
		break;
	case SIGNAL_CONNECTION_RESPONSE:
		error = signal_connection_response(l2cap, data, length, out, effect);
		break;
	case SIGNAL_CONFIGURE_REQUEST:
		error = signal_configure_request(l2cap, handle, identifier, data, length, out, effect);
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
			signal_closed(effect, channel, BTD_L2CAP_CLOSED_REFUSED);
			break;
		}

		/* Our configuration accepted; with theirs done too, the channel is open. */
		channel->local_done = 1;
		if (channel->remote_done) {
			channel->state = BTD_CHANNEL_OPEN;
			signal_opened(effect, channel);
		}

		break;
	case SIGNAL_DISCONNECTION_REQUEST:
		/* The other side closes a channel: answered with the same two CIDs, and freed. */
		if (length < 4U) {
			error = signal_reject(identifier, out);
			break;
		}

		/* The other side closed the channel of this connection: it is free. */
		channel = signal_find(l2cap, signal_le16(data));
		if (channel != NULL && channel->handle == handle)
			signal_closed(effect, channel, BTD_L2CAP_CLOSED_REMOTE);
		error = signal_put(out, SIGNAL_DISCONNECTION_RESPONSE, identifier, data, 4U);
		break;
	case SIGNAL_DISCONNECTION_RESPONSE:
		/* Our close done. */
		if (length < 4U)
			break;
		channel = signal_find(l2cap, signal_le16(data + 2));
		if (channel != NULL && channel->handle == handle)
			signal_closed(effect, channel, BTD_L2CAP_CLOSED_LOCAL);
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
		/* The answer to our Echo Request (the link is alive), if it is that one. */
		if (l2cap->echo_pending && identifier == l2cap->echo_identifier) {
			l2cap->echo_pending = 0;
			effect->echo = 1;
		}

		break;
	default:
		/* Not understood. */
		error = signal_reject(identifier, out);
		break;
	}

	/* Succeeded, or the answer did not fit. */
	return error;
}

/*
 * Answers the other side's Connection Request: the owner's accept hook
 * says success, pending or a refusal (without a hook: PSM not supported).
 * A channel accepted or pending takes a slot (none free: no resources);
 * an accepted one is configured at once (our Configure Request follows
 * the response).  Returns -1 when the answer does not fit.
 */
static int
signal_connection_request(
	struct btd_l2cap *l2cap,
	uint16_t handle,
	uint8_t identifier,
	const uint8_t *data,
	size_t length,
	struct signal_out *out)
{
	struct btd_channel *channel;
	uint8_t response[8];
	uint16_t psm;
	uint16_t source;
	uint16_t result;
	uint16_t status;
	unsigned index;
	int answered;
	int error;

	/* A request too short for its PSM and source CID is not understood. */
	if (length < 4U) {
		error = signal_reject(identifier, out);
		return error;
	}

	/* The PSM and their source CID. */
	psm = signal_le16(data);
	source = signal_le16(data + 2);

	/* The owner's answer, or PSM not supported without one. */
	result = BTD_L2CAP_PSM_NOT_SUPPORTED;
	status = 0U;
	if (l2cap->accept != NULL) {
		answered = l2cap->accept(l2cap->accept_context, handle, psm, &result, &status);
		if (answered != 0) {
			result = BTD_L2CAP_PSM_NOT_SUPPORTED;
			status = 0U;
		}
	}

	/* Their source must be a dynamic CID (Core 5.4 Vol 3 Part A §2.1). */
	if (result <= BTD_L2CAP_PENDING && source < BTD_CID_DYNAMIC)
		result = BTD_L2CAP_INVALID_SOURCE;

	/* One of their CIDs names one channel on a connection. */
	if (result <= BTD_L2CAP_PENDING) {
		for (index = 0U; index < BTD_CHANNELS_MAX; index++) {
			channel = &l2cap->channels[index];
			if (channel->state == BTD_CHANNEL_FREE || channel->handle != handle)
				continue;
			if (channel->remote_cid == source)
				result = BTD_L2CAP_SOURCE_TAKEN;
		}
	}

	/* A channel accepted or pending needs a slot. */
	channel = NULL;
	if (result <= BTD_L2CAP_PENDING) {
		channel = signal_free_slot(l2cap, &index);
		if (channel == NULL)
			result = BTD_L2CAP_NO_RESOURCES;
	}

	/* A refusal: no channel, and only their CID in the answer. */
	if (channel == NULL) {
		l2cap->rejected++;
		signal_put16(response, 0U);
		signal_put16(response + 2, source);
		signal_put16(response + 4, result);
		signal_put16(response + 6, 0U);
		error = signal_put(out, SIGNAL_CONNECTION_RESPONSE, identifier, response, sizeof(response));
		return error;
	}

	/* The channel: theirs, configuring or waiting for its final answer (which goes under their identifier). */
	memset(channel, 0, sizeof(*channel));
	channel->handle = handle;
	channel->local_cid = (uint16_t)(BTD_CID_DYNAMIC + index);
	channel->remote_cid = source;
	channel->psm = psm;
	channel->inbound = 1;
	channel->identifier = identifier;
	channel->state = BTD_CHANNEL_CONFIGURING;
	if (result == BTD_L2CAP_PENDING)
		channel->state = BTD_CHANNEL_PENDING;

	/* The response: our CID, theirs, the result and the status. */
	signal_put16(response, channel->local_cid);
	signal_put16(response + 2, source);
	signal_put16(response + 4, result);
	signal_put16(response + 6, status);
	error = signal_put(out, SIGNAL_CONNECTION_RESPONSE, identifier, response, sizeof(response));
	if (error != 0) {
		channel->state = BTD_CHANNEL_FREE;
		return error;
	}

	/* A pending channel waits for its final answer (btd_l2cap_answer_pending). */
	if (result == BTD_L2CAP_PENDING)
		return 0;

	/* Succeeded: an accepted channel is configured, our request first. */
	error = signal_configure_send(l2cap, channel, out);
	return error;
}

/* Takes a Connection Response to a request of ours: on success the channel is configured (our Configure Request goes). */
static int
signal_connection_response(
	struct btd_l2cap *l2cap,
	const uint8_t *data,
	size_t length,
	struct signal_out *out,
	struct btd_signal_effect *effect)
{
	struct btd_channel *channel;
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
	if (result == BTD_L2CAP_PENDING)
		return 0;

	/* Refused: the channel goes. */
	if (result != BTD_L2CAP_SUCCESS) {
		signal_closed(effect, channel, BTD_L2CAP_CLOSED_REFUSED);
		return 0;
	}

	/* Connected: our configuration, the MTU we take. */
	channel->remote_cid = signal_le16(data);
	error = signal_configure_send(l2cap, channel, out);

	/* Succeeded, or the request did not fit. */
	return error;
}

/*
 * Answers the other side's Configure Request: the MTU is taken when it is
 * at least BTD_L2CAP_MTU_MIN; the Flush Timeout and the QoS are kept and
 * taken (bluetoothd carries out neither, phase005 Q10), the basic mode of
 * retransmission and the FCS are taken, another mode is unacceptable (the
 * basic mode is offered); an unknown option that is not a hint is refused
 * with its type.
 */
static int
signal_configure_request(
	struct btd_l2cap *l2cap,
	uint16_t handle,
	uint8_t identifier,
	const uint8_t *data,
	size_t length,
	struct signal_out *out,
	struct btd_signal_effect *effect)
{
	struct btd_channel *channel;
	uint8_t response[SIGNAL_CONFIGURE_ANSWER];
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

	/* The channel of this connection being configured. */
	channel = signal_find(l2cap, signal_le16(data));
	if (channel == NULL ||
	    channel->handle != handle ||
	    channel->state != BTD_CHANNEL_CONFIGURING) {
		error = signal_reject(identifier, out);
		return error;
	}

	/* The options, each [type][length][value], read in turn. */
	result = 0U;
	mtu = BTD_L2CAP_MTU;
	answered = 6U;
	offset = 4U;
	while (offset + 2U <= length) {
		type = data[offset];
		size = data[offset + 1U];

		/* An option that runs past the request ends the reading. */
		if (offset + 2U + size > length)
			break;

		/* Each option bluetoothd knows, at its own length; anything else that is not a hint is refused with its type. */
		if (type == SIGNAL_OPTION_MTU && size == 2U) {
			mtu = signal_le16(data + offset + 2U);
		} else if (type == SIGNAL_OPTION_FLUSH && size == SIGNAL_FLUSH_LENGTH) {
			channel->have_flush_timeout = 1;
			channel->flush_timeout = signal_le16(data + offset + 2U);
		} else if (type == SIGNAL_OPTION_QOS && size == SIGNAL_QOS_LENGTH) {
			channel->have_qos = 1;
		} else if (type == SIGNAL_OPTION_RFC && size == SIGNAL_RFC_LENGTH) {
			/* Only the basic mode: another is answered with the basic mode's option. */
			if (data[offset + 2U] != SIGNAL_RFC_BASIC && result == 0U) {
				result = SIGNAL_CONFIG_UNACCEPTABLE;
				memset(response + answered, 0, 2U + SIGNAL_RFC_LENGTH);
				response[answered] = SIGNAL_OPTION_RFC;
				response[answered + 1U] = SIGNAL_RFC_LENGTH;
				response[answered + 2U] = SIGNAL_RFC_BASIC;
				answered += 2U + SIGNAL_RFC_LENGTH;
			}
		} else if (type == SIGNAL_OPTION_FCS && size == SIGNAL_FCS_LENGTH) {
			/* The FCS means nothing in the basic mode: taken. */
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
	if (result != 0U || (flags & 0x0001U) != 0U)
		return 0;
	channel->remote_mtu = mtu;
	channel->remote_done = 1;

	/* With ours done too, the channel is open. */
	if (channel->local_done) {
		channel->state = BTD_CHANNEL_OPEN;
		signal_opened(effect, channel);
	}

	/* Succeeded: answered. */
	return 0;
}

/* Sends our Configure Request for a channel that is now connected (the MTU we take); the channel is configuring. */
static int
signal_configure_send(
	struct btd_l2cap *l2cap,
	struct btd_channel *channel,
	struct signal_out *out)
{
	uint8_t request[8];
	int error;

	/* Configuring, under a new identifier. */
	channel->state = BTD_CHANNEL_CONFIGURING;
	channel->identifier = signal_identifier(l2cap);

	/* Their CID, no flags, the MTU option. */
	signal_put16(request, channel->remote_cid);
	signal_put16(request + 2, 0U);
	request[4] = SIGNAL_OPTION_MTU;
	request[5] = 2U;
	signal_put16(request + 6, BTD_L2CAP_MTU);
	error = signal_put(out, SIGNAL_CONFIGURE_REQUEST, channel->identifier, request, sizeof(request));

	/* Succeeded, or the request did not fit. */
	return error;
}

/* Finds a free slot of the table and its index, or NULL when the table is full. */
static struct btd_channel *
signal_free_slot(
	struct btd_l2cap *l2cap,
	unsigned *index)
{
	unsigned slot;

	/* The first free slot. */
	for (slot = 0U; slot < BTD_CHANNELS_MAX; slot++) {
		if (l2cap->channels[slot].state == BTD_CHANNEL_FREE) {
			*index = slot;
			return &l2cap->channels[slot];
		}
	}

	/* The table is full. */
	return NULL;
}

/* Tells the frame's caller that a channel opened (as many as the effect holds). */
static void
signal_opened(
	struct btd_signal_effect *effect,
	const struct btd_channel *channel)
{
	/* Kept while there is room. */
	if (effect->opened_count >= BTD_SIGNAL_CHANGES_MAX)
		return;

	/* Succeeded: its local CID. */
	effect->opened[effect->opened_count] = channel->local_cid;
	effect->opened_count++;
}

/* Frees a channel that closed and tells the frame's caller why (as many as the effect holds). */
static void
signal_closed(
	struct btd_signal_effect *effect,
	struct btd_channel *channel,
	unsigned reason)
{
	/* The slot is free. */
	channel->state = BTD_CHANNEL_FREE;

	/* Kept while there is room. */
	if (effect->closed_count >= BTD_SIGNAL_CHANGES_MAX)
		return;

	/* Succeeded: its local CID and why. */
	effect->closed[effect->closed_count] = channel->local_cid;
	effect->closed_reason[effect->closed_count] = reason;
	effect->closed_count++;
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
