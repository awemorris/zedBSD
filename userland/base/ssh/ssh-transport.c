/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transport layer of one connection.
 *
 * Written from RFC 4253 sections 4.2 (identification lines), 7 and 7.1
 * (KEXINIT and what may be sent around it), 7.3 (NEWKEYS), 9 (re-keying)
 * and 11 (the generic messages), RFC 4250 sections 4.1.2 and 4.2.2 (the
 * message numbers and the disconnect reasons), draft-ietf-sshm-strict-
 * kex-02 sections 3.2 and 3.3, and draft-ietf-sshm-mlkem-hybrid-kex-10
 * section 2.1.  The steps of an exchange are kept as bits of kex_step, one
 * for each message sent or received, so that a message is accepted only at
 * its own step and only once.
 */

#include <string.h>

#include "ssh-key.h"
#include "ssh-transport.h"

/*
 * The bits of kex_step: one per message of an exchange, set when it is
 * sent or received.  kex_step is 0 when no exchange is in progress.
 *
 * Values referenced:
 *   RFC 4253 7 and 7.3: each side sends one KEXINIT and, at the end, one
 *   NEWKEYS.
 *   draft-ietf-sshm-mlkem-hybrid-kex-10 2.1: the client sends
 *   SSH_MSG_KEX_HYBRID_INIT, the server answers SSH_MSG_KEX_HYBRID_REPLY.
 * HYBRID_SENT is the client's INIT or the server's REPLY; HYBRID_RECEIVED
 * is the server's INIT or the client's REPLY.
 */
#define TRANSPORT_KEXINIT_SENT		0x01
#define TRANSPORT_KEXINIT_RECEIVED	0x02
#define TRANSPORT_HYBRID_SENT		0x04
#define TRANSPORT_HYBRID_RECEIVED	0x08
#define TRANSPORT_NEWKEYS_SENT		0x10
#define TRANSPORT_NEWKEYS_RECEIVED	0x20

/*
 * The room for an identification line given to ssh_packet_take_line: 253
 * characters and the NUL.
 *
 * Values referenced:
 *   RFC 4253 4.2: the line is at most 255 characters including CR LF.
 */
#define TRANSPORT_LINE_CAPACITY		(SSH_VERSION_LINE_MAX - 1U)

/*
 * The identification line prefixes accepted from a peer.
 *
 * Values referenced:
 *   RFC 4253 4.2: "SSH-protoversion-softwareversion", protoversion "2.0".
 *   RFC 4253 5.1: a server that also speaks the old protocol identifies
 *   itself as "1.99", which a version 2 client treats as "2.0".
 * Any other line that starts with "SSH-" names a version not supported.
 */
#define TRANSPORT_PREFIX_SSH		"SSH-"
#define TRANSPORT_PREFIX_V2		"SSH-2.0-"
#define TRANSPORT_PREFIX_V199		"SSH-1.99-"

/* The largest upper-layer payload ssh_packet_send takes (its size limit). */
#define TRANSPORT_PAYLOAD_MAX		(SSH_PACKET_LIMIT - 255U - 1U)

/*
 * The output kept free for the transport's own messages: upper-layer
 * messages are refused (ENOBUFS) before the output passes
 * SSH_PACKET_OUTPUT_MAX less this, so a KEXINIT, a reply, a NEWKEYS or an
 * UNIMPLEMENTED finds room.
 */
#define TRANSPORT_OUTPUT_RESERVE	(64U * 1024U)

/*
 * The most bytes framing adds to a payload: the packet_length, the
 * padding_length, at most two blocks of padding and the tag.
 *
 * Values referenced:
 *   RFC 4253 6: uint32 packet_length, byte padding_length, at least four
 *   bytes of padding up to the block size.
 *   draft-ietf-sshm-chacha20-poly1305-04 6: a 16-byte tag.
 */
#define TRANSPORT_FRAME_EXTRA		(4U + 1U + 2U * SSH_CHACHAPOLY_BLOCK + SSH_CHACHAPOLY_TAG)

/* The largest host key blob or signature blob taken from the host key operations. */
#define TRANSPORT_BLOB_MAX		1024U

/* The largest SSH_MSG_KEX_HYBRID_REPLY this side builds: the message number and three strings. */
#define TRANSPORT_REPLY_MAX		(1U + 12U + 2U * TRANSPORT_BLOB_MAX + SSH_HYBRID_REPLY_SIZE)

/* The largest SSH_MSG_KEX_HYBRID_INIT this side builds: the message number and one string. */
#define TRANSPORT_INIT_MAX		(1U + 4U + SSH_HYBRID_INIT_SIZE)

/* The largest SSH_MSG_DISCONNECT this side builds: number, code, description and an empty language tag. */
#define TRANSPORT_DISCONNECT_MAX	(1U + 4U + 4U + SSH_TRANSPORT_REASON_MAX + 4U)

/* The size of an SSH_MSG_UNIMPLEMENTED payload: the number and the uint32 sequence number (RFC 4253 11.4). */
#define TRANSPORT_UNIMPLEMENTED_SIZE	5U

/* The size of the length field in front of each held payload. */
#define TRANSPORT_HELD_LENGTH		4U

/*
 * The bounds of the ranges of RFC 4250 4.1.2 the transport tells apart.
 *
 * Values referenced:
 *   RFC 4250 4.1.2: 1 to 19 transport generic, 20 to 29 algorithm
 *   negotiation, 30 to 49 key exchange method specific, 50 to 79 user
 *   authentication, 80 to 127 connection protocol, 128 to 191 reserved,
 *   192 to 255 local extensions.
 */
#define TRANSPORT_MSG_KEX_LAST		49U
#define TRANSPORT_MSG_UPPER_FIRST	50U
#define TRANSPORT_MSG_RESERVED_FIRST	128U
#define TRANSPORT_MSG_RESERVED_LAST	191U

/*
 * The layout of the bytes ssh_transport_export makes (this
 * implementation's own; both ends are the same program).
 *
 *   byte    format, TRANSPORT_EXPORT_FORMAT
 *   byte    role
 *   string  version_local
 *   string  version_peer
 *   string  session_id (SSH_KEX_HASH_SIZE bytes)
 *   byte    strict (1)
 *   twice, sending direction then receiving direction:
 *     string  key material (SSH_CHACHAPOLY_KEY bytes: K_1 then K_2)
 *     uint32  seq
 *     byte    wrapped (0 or 1)
 *     uint64  bytes
 *     uint64  packets
 *   uint64  kex_time
 *   string  received bytes not yet processed
 */
#define TRANSPORT_EXPORT_FORMAT		1U

static int transport_version(struct ssh_transport *transport);
static int transport_receive(struct ssh_transport *transport, struct ssh_buf *payload, int *delivered);
static int transport_dispatch(struct ssh_transport *transport, struct ssh_buf *payload, int *delivered);
static void transport_receive_disconnect(struct ssh_transport *transport, const struct ssh_buf *payload);
static int transport_receive_kexinit(struct ssh_transport *transport, const struct ssh_buf *payload);
static int transport_receive_hybrid_init(struct ssh_transport *transport, const struct ssh_buf *payload);
static int transport_receive_hybrid_reply(struct ssh_transport *transport, const struct ssh_buf *payload);
static int transport_receive_newkeys(struct ssh_transport *transport, const struct ssh_buf *payload);
static int transport_send_kexinit(struct ssh_transport *transport);
static int transport_send_hybrid_init(struct ssh_transport *transport);
static int transport_send_newkeys(struct ssh_transport *transport);
static int transport_send_unimplemented(struct ssh_transport *transport);
static int transport_send_packet(struct ssh_transport *transport, const uint8_t *payload, size_t size);
static int transport_flush_held(struct ssh_transport *transport);
static void transport_finish_exchange(struct ssh_transport *transport);
static int transport_fail(struct ssh_transport *transport, int failure, uint32_t code, const char *description);
static void transport_setup(struct ssh_transport *transport, enum ssh_role role, const struct ssh_transport_ops *ops, uint64_t now);
static void transport_forget_secrets(struct ssh_transport *transport);
static int transport_holding(const struct ssh_transport *transport);
static int transport_upper_room(const struct ssh_transport *transport, size_t size);
static int transport_is_upper(unsigned message);
static int transport_has_prefix(const char *line, size_t length, const char *prefix);
static int transport_check_line(const uint8_t *line, size_t size);
static void transport_put_direction(const struct ssh_packet_direction *direction, struct ssh_buf *out);
static int transport_get_direction(struct ssh_reader *reader, const uint8_t **key, uint32_t *seq, int *wrapped, uint64_t *bytes, uint64_t *packets);
static void transport_set_direction(struct ssh_packet_direction *direction, uint32_t seq, int wrapped, uint64_t bytes, uint64_t packets);

/*
 * Starts a transport for role with ops (copied) and queues this side's
 * identification line (SSH_VERSION_STRING and CR LF) in the output.
 *
 * now is the current time in seconds of a monotonic clock.
 */
int
ssh_transport_init(
	struct ssh_transport *transport,
	enum ssh_role role,
	const struct ssh_transport_ops *ops,
	uint64_t now)
{
	static const uint8_t line_end[2] = { '\r', '\n' };
	size_t length;
	int error;

	/* Starts an empty transport waiting for the peer's identification line. */
	transport_setup(transport, role, ops, now);

	/* Keeps this side's identification line for the exchange hash. */
	length = sizeof(SSH_VERSION_STRING) - 1U;
	memcpy(transport->version_local, SSH_VERSION_STRING, length + 1U);

	/* Queues the line, unframed, before any packet (RFC 4253 4.2). */
	error = ssh_packet_send_raw(&transport->packet, (const uint8_t *)SSH_VERSION_STRING, length);
	if (error != 0) {
		ssh_transport_free(transport);
		return ENOMEM;
	}

	/* Ends the line with CR LF. */
	error = ssh_packet_send_raw(&transport->packet, line_end, sizeof(line_end));
	if (error != 0) {
		ssh_transport_free(transport);
		return ENOMEM;
	}

	/* Succeeded: the line waits in the output. */
	return 0;
}

/*
 * Wipes every secret (keys, k, h, the client's half, held payloads) and
 * frees the memory.
 */
void
ssh_transport_free(
	struct ssh_transport *transport)
{
	/* Wipes the client's half, k and h, and frees the held payloads. */
	transport_forget_secrets(transport);

	/* Wipes the keys and the buffers of the packet layer. */
	ssh_packet_free(&transport->packet);

	/* Frees the KEXINIT payloads. */
	ssh_buf_free(&transport->kexinit_local);
	ssh_buf_free(&transport->kexinit_peer);

	/* Leaves nothing behind: the session identifier and every counter go too. */
	ssh_wipe(transport, sizeof(*transport));
}

/*
 * Appends size received bytes.
 *
 * A closed transport takes no more bytes (EPIPE).
 */
int
ssh_transport_input(
	struct ssh_transport *transport,
	const uint8_t *data,
	size_t size)
{
	int error;

	/* Refuses bytes after the connection ended. */
	if (transport->state == SSH_TRANSPORT_CLOSED)
		return EPIPE;

	/* Appends the bytes to the packet layer's input. */
	error = ssh_packet_feed(&transport->packet, data, size);
	if (error != 0)
		return error;

	/* Succeeded: the bytes wait for ssh_transport_next. */
	return 0;
}

/*
 * Processes received input until one upper-layer message is ready or the
 * input is used up.
 *
 * Each turn of the loop takes one line or one packet from the input, so
 * the loop ends when the input does.  A closed transport answers EPIPE.
 */
int
ssh_transport_next(
	struct ssh_transport *transport,
	struct ssh_buf *payload)
{
	int delivered;
	int error;

	/* Refuses to read after the connection ended. */
	if (transport->state == SSH_TRANSPORT_CLOSED)
		return EPIPE;

	/* Takes lines and packets, each from an empty payload, until a message for the upper layer comes out. */
	for (;;) {
		ssh_buf_reset(payload);

		/* Reads the peer's identification line, and the lines before it. */
		if (transport->state == SSH_TRANSPORT_VERSION) {
			error = transport_version(transport);
			if (error != 0)
				break;
			continue;
		}

		/* Reads one packet and acts on it; stops when it was for the upper layer. */
		error = transport_receive(transport, payload, &delivered);
		if (error != 0)
			break;
		if (delivered != 0)
			break;
	}

	/* Reports that more input is needed, or why the connection ended, leaving no partial message behind. */
	if (error != 0) {
		ssh_buf_reset(payload);
		return error;
	}

	/* Succeeded: payload holds one upper-layer message. */
	return 0;
}

/*
 * Sends one upper-layer message (message number first; 5, 6, or 50 and
 * above).
 *
 * A message is held, rather than framed, while this side's exchange is in
 * progress, before the initial exchange has given this side a key, and
 * while earlier held messages still wait, so that the order is kept.
 */
int
ssh_transport_send(
	struct ssh_transport *transport,
	const uint8_t *payload,
	size_t size)
{
	unsigned message;
	int holding;
	int room;
	int error;

	/* Refuses to send after the connection ended. */
	if (transport->state == SSH_TRANSPORT_CLOSED)
		return EPIPE;

	/* Refuses a missing or empty payload. */
	if (payload == NULL || size == 0)
		return EINVAL;

	/* Refuses a message number the transport owns: only 5, 6, and 50 and above are taken. */
	message = payload[0];
	if (message < TRANSPORT_MSG_UPPER_FIRST && message != SSH_MSG_SERVICE_REQUEST && message != SSH_MSG_SERVICE_ACCEPT)
		return EINVAL;

	/* Refuses a payload longer than one packet carries. */
	if (size > TRANSPORT_PAYLOAD_MAX)
		return EINVAL;

	/* Holds the message while this side may not send it yet (RFC 4253 7.1). */
	holding = transport_holding(transport);
	if (holding != 0) {
		/* Refuses a message the held room cannot take, keeping the held ones as they are. */
		if (transport->held.size > SSH_TRANSPORT_HELD_MAX - TRANSPORT_HELD_LENGTH)
			return ENOBUFS;
		if (size > SSH_TRANSPORT_HELD_MAX - TRANSPORT_HELD_LENGTH - transport->held.size)
			return ENOBUFS;

		/* Makes room for the length and the payload, so the two writes below cannot fail. */
		error = ssh_buf_reserve(&transport->held, TRANSPORT_HELD_LENGTH + size);
		if (error != 0) {
			transport->held.error = 0;
			return error;
		}

		/* Appends the payload with its length; it goes out after this side's NEWKEYS. */
		ssh_buf_put_u32(&transport->held, (uint32_t)size);
		ssh_buf_put_bytes(&transport->held, payload, size);
		return 0;
	}

	/* Refuses a message the output cannot take without eating into the transport's reserve. */
	room = transport_upper_room(transport, size);
	if (room == 0)
		return ENOBUFS;

	/* Frames and queues the message; a random source that failed ends the connection. */
	error = ssh_packet_send(&transport->packet, payload, size);
	if (error == EIO)
		return transport_fail(transport, EIO, SSH_DISCONNECT_PROTOCOL_ERROR, "random source failed");
	if (error != 0)
		return error;

	/* Succeeded: the message is in the output. */
	return 0;
}

/*
 * Gives the current time (seconds, monotonic) and starts a re-exchange
 * when a limit is reached and none is in progress (state RUNNING).
 *
 * A time earlier than the last key (a clock that went back) starts
 * nothing.  A failure to send KEXINIT ends the connection.
 */
int
ssh_transport_tick(
	struct ssh_transport *transport,
	uint64_t now)
{
	const struct ssh_packet_direction *send;
	const struct ssh_packet_direction *receive;
	int due;
	int error;

	/* Remembers the time, which also stamps the end of the next exchange. */
	transport->now = now;

	/* Starts nothing outside RUNNING or while an exchange is in progress. */
	if (transport->state != SSH_TRANSPORT_RUNNING || transport->kex_step != 0)
		return 0;

	/* Decides whether a limit of RFC 4253 9 is reached: the time, then the bytes, then the packets of either direction. */
	send = &transport->packet.send;
	receive = &transport->packet.receive;
	due = 0;
	if (now >= transport->kex_time && now - transport->kex_time >= SSH_TRANSPORT_REKEY_SECONDS) {
		due = 1;
	} else if (send->bytes >= SSH_TRANSPORT_REKEY_BYTES) {
		due = 1;
	} else if (receive->bytes >= SSH_TRANSPORT_REKEY_BYTES) {
		due = 1;
	} else if (send->packets >= SSH_TRANSPORT_REKEY_PACKETS) {
		due = 1;
	} else if (receive->packets >= SSH_TRANSPORT_REKEY_PACKETS) {
		due = 1;
	}

	/* Leaves the keys in place while no limit is reached. */
	if (due == 0)
		return 0;

	/* Starts the re-exchange with this side's KEXINIT. */
	error = transport_send_kexinit(transport);
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "cannot start key re-exchange");

	/* Succeeded: the re-exchange has started. */
	return 0;
}

/*
 * Queues SSH_MSG_DISCONNECT with code and description (NUL-terminated,
 * cut to SSH_TRANSPORT_REASON_MAX) and sets the state CLOSED.
 *
 * Nothing is queued when the transport is already closed.  The message is
 * framed with whatever key the sending direction has; when the output has
 * no room it is left out, and the transport is closed all the same.
 */
void
ssh_transport_disconnect(
	struct ssh_transport *transport,
	uint32_t code,
	const char *description)
{
	struct ssh_buf message;
	size_t length;

	/* Sends nothing more once the connection has ended. */
	if (transport->state == SSH_TRANSPORT_CLOSED)
		return;

	/* Measures the description, cut to the limit. */
	length = 0;
	if (description != NULL) {
		while (length < SSH_TRANSPORT_REASON_MAX && description[length] != '\0')
			length++;
	}

	/* Builds the message (RFC 4253 11.1): code, description, empty language tag. */
	ssh_buf_init(&message, TRANSPORT_DISCONNECT_MAX, 0);
	ssh_buf_put_u8(&message, SSH_MSG_DISCONNECT);
	ssh_buf_put_u32(&message, code);
	ssh_buf_put_string(&message, description, length);
	ssh_buf_put_cstring(&message, "");

	/* Queues it when it was built; a failure to queue cannot be reported to anyone. */
	if (message.error == 0)
		(void)ssh_packet_send(&transport->packet, message.data, message.size);
	ssh_buf_free(&message);

	/*
	 * CLOSED tells every later call that only the pending output remains:
	 * sends fail with EPIPE and nothing more is read.
	 */
	transport->state = SSH_TRANSPORT_CLOSED;

	/* Drops the secrets the connection no longer needs. */
	transport_forget_secrets(transport);
}

/*
 * Returns the bytes waiting to be written (as ssh_packet_pending).
 */
const uint8_t *
ssh_transport_pending(
	const struct ssh_transport *transport,
	size_t *size)
{
	const uint8_t *pending;

	/* Asks the packet layer for its output. */
	pending = ssh_packet_pending(&transport->packet, size);

	/* Reports where the bytes start (NULL when there are none). */
	return pending;
}

/*
 * Removes size written bytes from the output.
 *
 * The room this makes lets held messages that did not fit go out.
 */
void
ssh_transport_written(
	struct ssh_transport *transport,
	size_t size)
{
	int error;

	/* Drops the written bytes. */
	ssh_packet_written(&transport->packet, size);

	/* Leaves held messages alone after the end, before a key, or during this side's exchange. */
	if (transport->state == SSH_TRANSPORT_CLOSED || transport->state == SSH_TRANSPORT_VERSION)
		return;
	if ((transport->kex_step & TRANSPORT_KEXINIT_SENT) != 0 && (transport->kex_step & TRANSPORT_NEWKEYS_SENT) == 0)
		return;

	/* Has nothing to do when nothing is held. */
	if (transport->held.size == 0)
		return;

	/* Sends held messages into the room just made; a framing failure ends the connection. */
	error = transport_flush_held(transport);
	if (error != 0)
		(void)transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "cannot send queued messages");
}

/*
 * Appends everything another process needs to go on with this connection
 * after the initial exchange and authentication (a privilege-separated
 * server hands its connection from the pre-authentication process to the
 * session process): role, both identification lines, the session
 * identifier, the strict flag, both directions' keys, sequence numbers
 * and counters, the time of the last key, and the received bytes not yet
 * processed.
 *
 * The layout is TRANSPORT_EXPORT_FORMAT's, above.
 */
int
ssh_transport_export(
	const struct ssh_transport *transport,
	struct ssh_buf *out)
{
	size_t version_local_size;
	size_t version_peer_size;
	int error;

	/* Refuses a connection that is not in a steady state with nothing in flight. */
	if (transport->state != SSH_TRANSPORT_RUNNING)
		return EBUSY;
	if (transport->kex_step != 0)
		return EBUSY;
	if (transport->held.size != 0)
		return EBUSY;
	if (transport->packet.output.size != 0)
		return EBUSY;

	/* Measures the identification lines, which are NUL-terminated within their arrays. */
	version_local_size = strlen(transport->version_local);
	version_peer_size = strlen(transport->version_peer);

	/* Writes the format, the role, the lines, the session identifier and the strict flag. */
	ssh_buf_put_u8(out, TRANSPORT_EXPORT_FORMAT);
	ssh_buf_put_u8(out, (uint8_t)transport->role);
	ssh_buf_put_string(out, transport->version_local, version_local_size);
	ssh_buf_put_string(out, transport->version_peer, version_peer_size);
	ssh_buf_put_string(out, transport->session_id, SSH_KEX_HASH_SIZE);
	ssh_buf_put_u8(out, (uint8_t)transport->choice.strict);

	/* Writes the two directions. */
	transport_put_direction(&transport->packet.send, out);
	transport_put_direction(&transport->packet.receive, out);

	/* Writes the time of the last key and the bytes received but not yet processed. */
	ssh_buf_put_u64(out, transport->kex_time);
	ssh_buf_put_string(out, transport->packet.input.data, transport->packet.input.size);

	/* Reports a failure the buffer recorded. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Succeeded: out holds the connection. */
	return 0;
}

/*
 * Rebuilds a transport from size bytes made by ssh_transport_export,
 * with new ops.
 *
 * Every field is checked before anything is taken; on an error the
 * transport is left freed.
 */
int
ssh_transport_import(
	struct ssh_transport *transport,
	const uint8_t *data,
	size_t size,
	const struct ssh_transport_ops *ops,
	uint64_t now)
{
	struct ssh_reader reader;
	const uint8_t *version_local;
	const uint8_t *version_peer;
	const uint8_t *session_id;
	const uint8_t *send_key;
	const uint8_t *receive_key;
	const uint8_t *input;
	size_t version_local_size;
	size_t version_peer_size;
	size_t session_id_size;
	size_t input_size;
	uint32_t send_seq;
	uint32_t receive_seq;
	uint64_t send_bytes;
	uint64_t receive_bytes;
	uint64_t send_packets;
	uint64_t receive_packets;
	uint64_t kex_time;
	int send_wrapped;
	int receive_wrapped;
	unsigned format;
	unsigned role;
	unsigned strict;
	int checked;
	int error;

	/* Starts an empty transport, so that every failure below leaves one that can be freed. */
	transport_setup(transport, SSH_ROLE_CLIENT, ops, now);
	error = EINVAL;

	/* Reads the format and the role. */
	ssh_reader_init(&reader, data, size);
	format = ssh_reader_get_u8(&reader);
	role = ssh_reader_get_u8(&reader);
	if (format != TRANSPORT_EXPORT_FORMAT || role > (unsigned)SSH_ROLE_SERVER)
		goto done;

	/* Reads the identification lines and checks each as a line of the peer would be. */
	ssh_reader_get_string(&reader, &version_local, &version_local_size);
	checked = transport_check_line(version_local, version_local_size);
	if (checked != 0)
		goto done;
	ssh_reader_get_string(&reader, &version_peer, &version_peer_size);
	checked = transport_check_line(version_peer, version_peer_size);
	if (checked != 0)
		goto done;

	/* Reads the session identifier and the strict flag, which strict key exchange requires to be 1. */
	ssh_reader_get_string(&reader, &session_id, &session_id_size);
	strict = ssh_reader_get_u8(&reader);
	if (session_id_size != SSH_KEX_HASH_SIZE || strict != 1U)
		goto done;

	/* Reads the two directions. */
	checked = transport_get_direction(&reader, &send_key, &send_seq, &send_wrapped, &send_bytes, &send_packets);
	if (checked != 0)
		goto done;
	checked = transport_get_direction(&reader, &receive_key, &receive_seq, &receive_wrapped, &receive_bytes, &receive_packets);
	if (checked != 0)
		goto done;

	/* Reads the time of the last key and the unprocessed bytes, which must end the data exactly. */
	kex_time = ssh_reader_get_u64(&reader);
	ssh_reader_get_string(&reader, &input, &input_size);
	checked = ssh_reader_finish(&reader);
	if (checked != 0)
		goto done;
	if (input_size > SSH_PACKET_INPUT_MAX)
		goto done;

	/* Takes the role, the lines and the session identifier. */
	transport->role = (enum ssh_role)role;
	memcpy(transport->version_local, version_local, version_local_size);
	transport->version_local[version_local_size] = '\0';
	memcpy(transport->version_peer, version_peer, version_peer_size);
	transport->version_peer[version_peer_size] = '\0';
	memcpy(transport->session_id, session_id, SSH_KEX_HASH_SIZE);
	transport->have_session_id = 1;

	/* Takes the keys, then the counters, which setting a key clears. */
	ssh_packet_set_send_key(&transport->packet, send_key);
	ssh_packet_set_receive_key(&transport->packet, receive_key);
	transport_set_direction(&transport->packet.send, send_seq, send_wrapped, send_bytes, send_packets);
	transport_set_direction(&transport->packet.receive, receive_seq, receive_wrapped, receive_bytes, receive_packets);

	/* Puts back the bytes not yet processed; the size was checked, so only memory can fail. */
	checked = ssh_packet_feed(&transport->packet, input, input_size);
	if (checked != 0) {
		error = ENOMEM;
		goto done;
	}

	/*
	 * The initial exchange is over and strict key exchange is in force:
	 * the transport goes on as RUNNING with no exchange in progress.
	 */
	transport->choice.strict = 1;
	transport->initial_done = 1;
	transport->kex_time = kex_time;
	transport->state = SSH_TRANSPORT_RUNNING;
	error = 0;

done:
	/* Leaves a refused import freed, with nothing of the bytes kept. */
	if (error != 0) {
		ssh_transport_free(transport);
		return error;
	}

	/* Succeeded: the transport continues the connection. */
	return 0;
}

/*
 * Takes one line before the first packet: a line the client skips, or the
 * peer's identification line, after which this side sends its KEXINIT.
 * Returns 0 when a line was taken, EAGAIN, or a fatal error (already
 * acted on).
 */
static int
transport_version(
	struct ssh_transport *transport)
{
	char line[TRANSPORT_LINE_CAPACITY];
	size_t before;
	size_t length;
	size_t consumed;
	int ssh;
	int v2;
	int v199;
	int error;

	/* Takes one line from the input, remembering how many bytes it took with its ending. */
	before = transport->packet.input.size;
	error = ssh_packet_take_line(&transport->packet, line, sizeof(line), &length);
	if (error == EAGAIN)
		return EAGAIN;
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "bad identification line");
	consumed = before - transport->packet.input.size;

	/* A line that does not start with "SSH-" is not the identification line (RFC 4253 4.2). */
	ssh = transport_has_prefix(line, length, TRANSPORT_PREFIX_SSH);
	if (ssh == 0) {
		/* Only a server may send other lines first. */
		if (transport->role == SSH_ROLE_SERVER)
			return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "no identification line");

		/* Counts the line against the limits on what may come before the identification line. */
		transport->prelude_lines++;
		transport->prelude_bytes += consumed;
		if (transport->prelude_lines > SSH_VERSION_PRELUDE_LINES)
			return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "too many lines before identification");
		if (transport->prelude_bytes > SSH_VERSION_PRELUDE_BYTES)
			return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "too many bytes before identification");
		return 0;
	}

	/* Refuses a protocol version other than 2.0 (or 1.99, which includes it). */
	v2 = transport_has_prefix(line, length, TRANSPORT_PREFIX_V2);
	v199 = transport_has_prefix(line, length, TRANSPORT_PREFIX_V199);
	if (v2 == 0 && v199 == 0)
		return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_VERSION_NOT_SUPPORTED, "protocol version not supported");

	/* Keeps the peer's line for the exchange hash. */
	memcpy(transport->version_peer, line, length + 1U);

	/* The initial exchange starts: this side sends its KEXINIT. */
	transport->state = SSH_TRANSPORT_INITIAL_KEX;
	error = transport_send_kexinit(transport);
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "cannot send KEXINIT");

	/* Succeeded: the identification line was taken. */
	return 0;
}

/*
 * Takes one packet and acts on it.  *delivered is 1 when payload holds an
 * upper-layer message for the caller.  Returns 0 (a packet was taken),
 * EAGAIN, ECONNABORTED or a fatal error (already acted on).
 */
static int
transport_receive(
	struct ssh_transport *transport,
	struct ssh_buf *payload,
	int *delivered)
{
	const struct ssh_packet_direction *send;
	const struct ssh_packet_direction *receive;
	int error;

	/* Starts with nothing for the caller. */
	*delivered = 0;

	/* Takes the next whole packet; a bad tag is a MAC error (RFC 4253 6.4: not skipped). */
	error = ssh_packet_next(&transport->packet, payload);
	if (error == EAGAIN)
		return EAGAIN;
	if (error == EBADMSG)
		return transport_fail(transport, EBADMSG, SSH_DISCONNECT_MAC_ERROR, "corrupted packet");
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "bad packet");

	/* Refuses a sequence number that wrapped before the initial exchange ended (strict key exchange 3.2). */
	send = &transport->packet.send;
	receive = &transport->packet.receive;
	if (transport->initial_done == 0) {
		if (send->wrapped != 0 || receive->wrapped != 0)
			return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "sequence number wrapped");
	}

	/* Refuses a packet without a message number. */
	if (payload->size == 0)
		return transport_fail(transport, EINVAL, SSH_DISCONNECT_PROTOCOL_ERROR, "empty packet");

	/*
	 * IGNORE_NEXT means the peer's guessed key exchange packet was wrong;
	 * the packet right after its KEXINIT is dropped (RFC 4253 7).
	 */
	if (transport->choice.ignore_next != 0) {
		transport->choice.ignore_next = 0;
		return 0;
	}

	/* Acts on the message. */
	error = transport_dispatch(transport, payload, delivered);
	if (error != 0)
		return error;

	/* Succeeded: the packet was taken. */
	return 0;
}

/*
 * Acts on one message by its number.  Returns 0 (handled, or *delivered 1
 * for an upper-layer message), ECONNABORTED or a fatal error (already
 * acted on).
 */
static int
transport_dispatch(
	struct ssh_transport *transport,
	struct ssh_buf *payload,
	int *delivered)
{
	unsigned message;
	int peer_exchange;
	int handled;
	int upper;
	int error;

	/* The message number is the first byte (the caller refused an empty payload). */
	message = payload->data[0];

	/* Ends the connection the peer ends, at any point (RFC 4253 11.1). */
	if (message == SSH_MSG_DISCONNECT) {
		transport_receive_disconnect(transport, payload);
		return ECONNABORTED;
	}

	/* Accepts, during the initial exchange, KEXINIT first and then only the exchange's own messages (strict key exchange 3.2). */
	if (transport->initial_done == 0) {
		if ((transport->kex_step & TRANSPORT_KEXINIT_RECEIVED) == 0 && message != SSH_MSG_KEXINIT)
			return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "first message is not KEXINIT");
		if (message != SSH_MSG_KEXINIT &&
		    message != SSH_MSG_NEWKEYS &&
		    message != SSH_MSG_KEX_HYBRID_INIT &&
		    message != SSH_MSG_KEX_HYBRID_REPLY)
			return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "message not allowed in initial key exchange");
	}

	/* Acts on the generic and key exchange messages. */
	handled = 1;
	error = 0;
	switch (message) {
	case SSH_MSG_IGNORE:
	case SSH_MSG_UNIMPLEMENTED:
	case SSH_MSG_DEBUG:
	case SSH_MSG_EXT_INFO:
		/* Drops them: nothing here asked for EXT_INFO, and the others carry nothing to act on. */
		break;
	case SSH_MSG_KEXINIT:
		error = transport_receive_kexinit(transport, payload);
		break;
	case SSH_MSG_NEWKEYS:
		error = transport_receive_newkeys(transport, payload);
		break;
	case SSH_MSG_KEX_HYBRID_INIT:
		error = transport_receive_hybrid_init(transport, payload);
		break;
	case SSH_MSG_KEX_HYBRID_REPLY:
		error = transport_receive_hybrid_reply(transport, payload);
		break;
	default:
		/* Leaves the other numbers to the ranges below. */
		handled = 0;
		break;
	}

	/* Reports what a message of the transport's own came to. */
	if (handled != 0) {
		if (error != 0)
			return error;
		return 0;
	}

	/* Refuses the other key exchange method numbers, which this method never uses. */
	if (message > SSH_MSG_KEX_HYBRID_REPLY && message <= TRANSPORT_MSG_KEX_LAST)
		return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "unexpected key exchange message");

	/* Decides whether the peer is between its KEXINIT and its NEWKEYS. */
	peer_exchange = 0;
	if ((transport->kex_step & TRANSPORT_KEXINIT_RECEIVED) != 0 && (transport->kex_step & TRANSPORT_NEWKEYS_RECEIVED) == 0)
		peer_exchange = 1;

	/* Hands an upper-layer message to the caller, unless the peer may not send it now (RFC 4253 7.1). */
	upper = transport_is_upper(message);
	if (message == SSH_MSG_SERVICE_REQUEST || message == SSH_MSG_SERVICE_ACCEPT)
		upper = 1;
	if (upper != 0 && peer_exchange == 0) {
		*delivered = 1;
		return 0;
	}

	/* Answers a number outside every layer, or out of its time, with UNIMPLEMENTED (RFC 4253 11.4). */
	error = transport_send_unimplemented(transport);
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "cannot send UNIMPLEMENTED");

	/* Succeeded: the message was answered and dropped. */
	return 0;
}

/*
 * Keeps the code and description of the peer's SSH_MSG_DISCONNECT and
 * closes.  A malformed message closes all the same, with what could be
 * read (code 0 and an empty reason when nothing could).  Control
 * characters of the description are kept as '?', so a caller can print
 * it.
 */
static void
transport_receive_disconnect(
	struct ssh_transport *transport,
	const struct ssh_buf *payload)
{
	struct ssh_reader reader;
	const uint8_t *description;
	size_t description_size;
	size_t index;
	uint32_t code;

	/* Reads the reason code and the description (RFC 4253 11.1); the language tag is not looked at. */
	ssh_reader_init(&reader, payload->data, payload->size);
	(void)ssh_reader_get_u8(&reader);
	code = ssh_reader_get_u32(&reader);
	ssh_reader_get_string(&reader, &description, &description_size);

	/* Keeps the code and the description, cut to the room and made printable. */
	transport->reason_code = code;
	if (description_size > SSH_TRANSPORT_REASON_MAX - 1U)
		description_size = SSH_TRANSPORT_REASON_MAX - 1U;
	for (index = 0; index < description_size; index++) {
		if (description[index] < 0x20U || description[index] == 0x7fU) {
			transport->reason[index] = '?';
		} else {
			transport->reason[index] = (char)description[index];
		}
	}
	transport->reason[description_size] = '\0';

	/*
	 * CLOSED tells every later call that the peer has gone; no DISCONNECT
	 * is sent back (RFC 4253 11.1).
	 */
	transport->state = SSH_TRANSPORT_CLOSED;
	transport_forget_secrets(transport);
}

/*
 * Takes the peer's KEXINIT: answers with this side's when the peer starts
 * the exchange, decides the algorithms, and, on a client, sends the
 * hybrid INIT.
 */
static int
transport_receive_kexinit(
	struct ssh_transport *transport,
	const struct ssh_buf *payload)
{
	static const char prefix[] = "no matching ";
	struct ssh_kexinit kexinit;
	char description[SSH_TRANSPORT_REASON_MAX];
	const char *kind;
	size_t length;
	int initial;
	int error;

	/* Refuses a second KEXINIT in one exchange. */
	if ((transport->kex_step & TRANSPORT_KEXINIT_RECEIVED) != 0)
		return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "unexpected KEXINIT");

	/* Answers a re-exchange the peer starts with this side's KEXINIT (RFC 4253 9). */
	if ((transport->kex_step & TRANSPORT_KEXINIT_SENT) == 0) {
		error = transport_send_kexinit(transport);
		if (error != 0)
			return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "cannot send KEXINIT");
	}

	/* Reads the peer's lists. */
	error = ssh_kexinit_parse(payload->data, payload->size, &kexinit);
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "bad KEXINIT");

	/* Decides the algorithms, and in the initial exchange strict key exchange. */
	initial = 0;
	if (transport->initial_done == 0)
		initial = 1;
	error = ssh_kex_choose(transport->role == SSH_ROLE_SERVER, initial, &kexinit, &transport->choice, &kind);
	if (error == ENOTSUP || error == EPROTO) {
		/* Names the kind that failed in the message. */
		memcpy(description, prefix, sizeof(prefix));
		length = sizeof(prefix) - 1U;
		while (kind != NULL && *kind != '\0' && length < sizeof(description) - 1U) {
			description[length] = *kind;
			length++;
			kind++;
		}
		description[length] = '\0';
		return transport_fail(transport, error, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, description);
	}
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "key exchange failed");

	/* Keeps the peer's payload as it is, for the exchange hash. */
	ssh_buf_reset(&transport->kexinit_peer);
	ssh_buf_put_bytes(&transport->kexinit_peer, payload->data, payload->size);
	error = ssh_buf_error(&transport->kexinit_peer);
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "out of memory");

	/* KEXINIT_RECEIVED marks the peer as inside its exchange until its NEWKEYS. */
	transport->kex_step |= TRANSPORT_KEXINIT_RECEIVED;

	/* Leaves the next step to the client on a server. */
	if (transport->role == SSH_ROLE_SERVER)
		return 0;

	/* Sends the client's half. */
	error = transport_send_hybrid_init(transport);
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "cannot start key exchange");

	/* Succeeded: the client waits for the reply. */
	return 0;
}

/*
 * Takes the client's SSH_MSG_KEX_HYBRID_INIT on a server: makes S_REPLY
 * and K, the exchange hash and its signature, and sends the reply and
 * NEWKEYS.
 */
static int
transport_receive_hybrid_init(
	struct ssh_transport *transport,
	const struct ssh_buf *payload)
{
	struct ssh_kex_hash_input input;
	struct ssh_buf host_key;
	struct ssh_buf signature;
	struct ssh_buf reply;
	uint8_t s_reply[SSH_HYBRID_REPLY_SIZE];
	const uint8_t *c_init;
	size_t c_init_size;
	uint32_t code;
	int error;

	/* Accepts the message only on a server after the client's KEXINIT, and once. */
	if (transport->role != SSH_ROLE_SERVER)
		return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "unexpected KEX_HYBRID_INIT");
	if ((transport->kex_step & TRANSPORT_KEXINIT_RECEIVED) == 0)
		return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "unexpected KEX_HYBRID_INIT");
	if ((transport->kex_step & TRANSPORT_HYBRID_RECEIVED) != 0)
		return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "unexpected KEX_HYBRID_INIT");

	/* Reads C_INIT. */
	error = ssh_kex_parse_hybrid_init(payload->data, payload->size, &c_init, &c_init_size);
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "bad KEX_HYBRID_INIT");

	/* HYBRID_RECEIVED marks the client's half as taken, so a second one is refused. */
	transport->kex_step |= TRANSPORT_HYBRID_RECEIVED;

	/* Starts the three buffers empty, so the cleanup below frees whatever was made. */
	ssh_buf_init(&host_key, TRANSPORT_BLOB_MAX, 0);
	ssh_buf_init(&signature, TRANSPORT_BLOB_MAX, 0);
	ssh_buf_init(&reply, TRANSPORT_REPLY_MAX, 0);
	code = SSH_DISCONNECT_PROTOCOL_ERROR;

	/* Checks C_INIT and makes S_REPLY and K (draft-ietf-sshm-mlkem-hybrid-kex-10 2.1); a refused value fails the exchange. */
	error = ssh_hybrid_server(c_init, c_init_size, s_reply, transport->k);
	if (error == EINVAL)
		code = SSH_DISCONNECT_KEY_EXCHANGE_FAILED;
	if (error != 0)
		goto done;

	/* Asks for the host key blob. */
	error = EINVAL;
	if (transport->ops.host_key == NULL || transport->ops.sign == NULL)
		goto done;
	error = transport->ops.host_key(transport->ops.context, &host_key);
	if (error != 0)
		goto done;
	error = ssh_buf_error(&host_key);
	if (error != 0)
		goto done;

	/* Computes H over the client's and this side's lines and KEXINITs, the host key, C_INIT, S_REPLY and K. */
	input.version_client = (const uint8_t *)transport->version_peer;
	input.version_client_size = strlen(transport->version_peer);
	input.version_server = (const uint8_t *)transport->version_local;
	input.version_server_size = strlen(transport->version_local);
	input.kexinit_client = transport->kexinit_peer.data;
	input.kexinit_client_size = transport->kexinit_peer.size;
	input.kexinit_server = transport->kexinit_local.data;
	input.kexinit_server_size = transport->kexinit_local.size;
	input.host_key = host_key.data;
	input.host_key_size = host_key.size;
	input.c_init = c_init;
	input.c_init_size = c_init_size;
	input.s_reply = s_reply;
	input.s_reply_size = sizeof(s_reply);
	input.k = transport->k;
	error = ssh_kex_hash(&input, transport->h);
	if (error != 0)
		goto done;

	/* The first exchange's H is the session identifier for the whole connection (RFC 4253 7.2). */
	if (transport->have_session_id == 0) {
		memcpy(transport->session_id, transport->h, SSH_KEX_HASH_SIZE);
		transport->have_session_id = 1;
	}

	/* Asks for the signature of H. */
	error = transport->ops.sign(transport->ops.context, transport->h, &signature);
	if (error != 0)
		goto done;
	error = ssh_buf_error(&signature);
	if (error != 0)
		goto done;

	/* Builds and sends the reply. */
	ssh_kex_put_hybrid_reply(host_key.data, host_key.size, s_reply, signature.data, signature.size, &reply);
	error = ssh_buf_error(&reply);
	if (error != 0)
		goto done;
	error = transport_send_packet(transport, reply.data, reply.size);
	if (error != 0)
		goto done;

	/* HYBRID_SENT marks the reply as gone: only NEWKEYS remains on this side. */
	transport->kex_step |= TRANSPORT_HYBRID_SENT;

	/* Switches the sending direction to the new key. */
	error = transport_send_newkeys(transport);

done:
	/* Frees the buffers of the reply on every path. */
	ssh_buf_free(&host_key);
	ssh_buf_free(&signature);
	ssh_buf_free(&reply);

	/* Ends the connection after any failure of the exchange. */
	if (error != 0)
		return transport_fail(transport, error, code, "key exchange failed");

	/* Succeeded: the server waits for the client's NEWKEYS. */
	return 0;
}

/*
 * Takes the server's SSH_MSG_KEX_HYBRID_REPLY on a client: checks the
 * length of S_REPLY, the host key (ops->check_host_key), makes K and H,
 * verifies the signature, and sends NEWKEYS.
 */
static int
transport_receive_hybrid_reply(
	struct ssh_transport *transport,
	const struct ssh_buf *payload)
{
	struct ssh_kex_hash_input input;
	uint8_t public_key[SSH_ED25519_PUBLIC];
	const uint8_t *host_key;
	const uint8_t *s_reply;
	const uint8_t *signature;
	size_t host_key_size;
	size_t s_reply_size;
	size_t signature_size;
	int error;

	/* Accepts the message only on a client that sent its half, and once. */
	if (transport->role != SSH_ROLE_CLIENT)
		return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "unexpected KEX_HYBRID_REPLY");
	if ((transport->kex_step & TRANSPORT_HYBRID_SENT) == 0)
		return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "unexpected KEX_HYBRID_REPLY");
	if ((transport->kex_step & TRANSPORT_HYBRID_RECEIVED) != 0)
		return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "unexpected KEX_HYBRID_REPLY");

	/* Reads K_S, S_REPLY and the signature. */
	error = ssh_kex_parse_hybrid_reply(payload->data, payload->size, &host_key, &host_key_size, &s_reply, &s_reply_size, &signature, &signature_size);
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "bad KEX_HYBRID_REPLY");

	/* HYBRID_RECEIVED marks the reply as taken, so a second one is refused. */
	transport->kex_step |= TRANSPORT_HYBRID_RECEIVED;

	/* Refuses an S_REPLY of the wrong length before anything else is done with the reply (draft 2.1). */
	if (s_reply_size != SSH_HYBRID_REPLY_SIZE)
		return transport_fail(transport, EINVAL, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "bad S_REPLY length");

	/* Reads the host key, which must be the ssh-ed25519 key that was negotiated. */
	error = ssh_key_parse_public_blob(host_key, host_key_size, public_key);
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "bad host key");

	/* Asks the caller whether the host key is the right one for this server. */
	if (transport->ops.check_host_key == NULL)
		return transport_fail(transport, EINVAL, SSH_DISCONNECT_PROTOCOL_ERROR, "no host key check");
	error = transport->ops.check_host_key(transport->ops.context, host_key, host_key_size);
	if (error == EACCES)
		return transport_fail(transport, EACCES, SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE, "host key not verifiable");
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "host key check failed");

	/* Finishes the client's half, giving K; a refused server value fails the exchange. */
	error = ssh_hybrid_client_finish(&transport->hybrid, s_reply, s_reply_size, transport->k);
	if (error == EINVAL)
		return transport_fail(transport, EINVAL, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "bad key exchange value");
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "key exchange failed");

	/* Computes H over this side's and the server's lines and KEXINITs, the host key, C_INIT, S_REPLY and K. */
	input.version_client = (const uint8_t *)transport->version_local;
	input.version_client_size = strlen(transport->version_local);
	input.version_server = (const uint8_t *)transport->version_peer;
	input.version_server_size = strlen(transport->version_peer);
	input.kexinit_client = transport->kexinit_local.data;
	input.kexinit_client_size = transport->kexinit_local.size;
	input.kexinit_server = transport->kexinit_peer.data;
	input.kexinit_server_size = transport->kexinit_peer.size;
	input.host_key = host_key;
	input.host_key_size = host_key_size;
	input.c_init = transport->c_init;
	input.c_init_size = sizeof(transport->c_init);
	input.s_reply = s_reply;
	input.s_reply_size = s_reply_size;
	input.k = transport->k;
	error = ssh_kex_hash(&input, transport->h);
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "key exchange failed");

	/* Verifies the server's signature over H with the host key. */
	error = ssh_key_verify(public_key, transport->h, SSH_KEX_HASH_SIZE, signature, signature_size);
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_KEY_EXCHANGE_FAILED, "bad host key signature");

	/* The first exchange's H is the session identifier for the whole connection (RFC 4253 7.2). */
	if (transport->have_session_id == 0) {
		memcpy(transport->session_id, transport->h, SSH_KEX_HASH_SIZE);
		transport->have_session_id = 1;
	}

	/* Switches the sending direction to the new key. */
	error = transport_send_newkeys(transport);
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "cannot send NEWKEYS");

	/* Succeeded: the client waits for the server's NEWKEYS. */
	return 0;
}

/*
 * Takes the peer's SSH_MSG_NEWKEYS: the receiving direction switches to
 * the new key (RFC 4253 7.3) and, with strict key exchange, its sequence
 * number starts again at 0 (strict key exchange 3.3).
 */
static int
transport_receive_newkeys(
	struct ssh_transport *transport,
	const struct ssh_buf *payload)
{
	uint8_t key[SSH_CHACHAPOLY_KEY];
	char letter;
	int error;

	/* Accepts NEWKEYS only after this side's, and once. */
	if ((transport->kex_step & TRANSPORT_NEWKEYS_SENT) == 0)
		return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "unexpected NEWKEYS");
	if ((transport->kex_step & TRANSPORT_NEWKEYS_RECEIVED) != 0)
		return transport_fail(transport, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "unexpected NEWKEYS");

	/* Refuses a NEWKEYS with anything after the message number. */
	if (payload->size != 1U)
		return transport_fail(transport, EINVAL, SSH_DISCONNECT_PROTOCOL_ERROR, "bad NEWKEYS");

	/* Derives the receiving key: the server's 'D' on a client, the client's 'C' on a server (RFC 4253 7.2). */
	letter = 'C';
	if (transport->role == SSH_ROLE_CLIENT)
		letter = 'D';
	error = ssh_kex_derive(transport->k, transport->h, transport->session_id, letter, key, sizeof(key));
	if (error != 0)
		return transport_fail(transport, error, SSH_DISCONNECT_PROTOCOL_ERROR, "key derivation failed");

	/* Switches the receiving direction; the next packet is opened with the new key and, strict, numbered from 0. */
	ssh_packet_set_receive_key(&transport->packet, key);
	ssh_wipe(key, sizeof(key));
	if (transport->choice.strict != 0)
		ssh_packet_reset_seq(&transport->packet, 0);

	/* NEWKEYS_RECEIVED marks the peer as out of its exchange: its upper-layer messages are allowed again. */
	transport->kex_step |= TRANSPORT_NEWKEYS_RECEIVED;
	transport_finish_exchange(transport);

	/* Succeeded: the receiving direction uses the new key. */
	return 0;
}

/*
 * Sends this side's KEXINIT, kept in kexinit_local for the exchange hash,
 * with the strict markers in the initial exchange only.
 */
static int
transport_send_kexinit(
	struct ssh_transport *transport)
{
	int initial;
	int error;

	/* Builds the payload. */
	initial = 0;
	if (transport->initial_done == 0)
		initial = 1;
	ssh_buf_reset(&transport->kexinit_local);
	error = ssh_kexinit_put(transport->role == SSH_ROLE_SERVER, initial, &transport->kexinit_local);
	if (error != 0)
		return error;

	/* Sends it. */
	error = transport_send_packet(transport, transport->kexinit_local.data, transport->kexinit_local.size);
	if (error != 0)
		return error;

	/*
	 * KEXINIT_SENT marks this side as inside its exchange: upper-layer
	 * messages are held from here until its NEWKEYS (RFC 4253 7.1).
	 */
	transport->kex_step |= TRANSPORT_KEXINIT_SENT;

	/* Succeeded: the KEXINIT is in the output. */
	return 0;
}

/*
 * Starts the client's half of the hybrid exchange and sends
 * SSH_MSG_KEX_HYBRID_INIT; C_INIT stays in c_init for the exchange hash.
 */
static int
transport_send_hybrid_init(
	struct ssh_transport *transport)
{
	struct ssh_buf message;
	int error;

	/* Makes the client's key pairs and C_INIT. */
	error = ssh_hybrid_client_start(&transport->hybrid, transport->c_init);
	if (error != 0)
		return error;

	/* Builds the message. */
	ssh_buf_init(&message, TRANSPORT_INIT_MAX, 0);
	ssh_kex_put_hybrid_init(transport->c_init, &message);
	error = ssh_buf_error(&message);
	if (error != 0) {
		ssh_buf_free(&message);
		return error;
	}

	/* Sends it. */
	error = transport_send_packet(transport, message.data, message.size);
	ssh_buf_free(&message);
	if (error != 0)
		return error;

	/* HYBRID_SENT marks the client's half as sent: the reply is expected now. */
	transport->kex_step |= TRANSPORT_HYBRID_SENT;

	/* Succeeded: the client's half is in the output. */
	return 0;
}

/*
 * Sends SSH_MSG_NEWKEYS and switches the sending direction to the new key
 * (RFC 4253 7.3); with strict key exchange the sending sequence number
 * starts again at 0 (strict key exchange 3.3).  The held upper-layer
 * messages follow.
 */
static int
transport_send_newkeys(
	struct ssh_transport *transport)
{
	static const uint8_t newkeys[1] = { SSH_MSG_NEWKEYS };
	uint8_t key[SSH_CHACHAPOLY_KEY];
	char letter;
	int error;

	/* Derives the sending key first, so a failure leaves no NEWKEYS without a key behind it. */
	letter = 'D';
	if (transport->role == SSH_ROLE_CLIENT)
		letter = 'C';
	error = ssh_kex_derive(transport->k, transport->h, transport->session_id, letter, key, sizeof(key));
	if (error != 0)
		return error;

	/* Sends NEWKEYS under the old key. */
	error = transport_send_packet(transport, newkeys, sizeof(newkeys));
	if (error != 0) {
		ssh_wipe(key, sizeof(key));
		return error;
	}

	/* Switches the sending direction for every later packet and, strict, numbers them from 0. */
	ssh_packet_set_send_key(&transport->packet, key);
	ssh_wipe(key, sizeof(key));
	if (transport->choice.strict != 0)
		ssh_packet_reset_seq(&transport->packet, 1);

	/* NEWKEYS_SENT marks this side as out of its exchange: held messages may go now. */
	transport->kex_step |= TRANSPORT_NEWKEYS_SENT;

	/* Sends the held messages, in order. */
	error = transport_flush_held(transport);
	if (error != 0)
		return error;

	/* Ends the exchange when the peer's NEWKEYS has also come. */
	transport_finish_exchange(transport);

	/* Succeeded: the sending direction uses the new key. */
	return 0;
}

/*
 * Answers the packet just received with SSH_MSG_UNIMPLEMENTED carrying its
 * sequence number (RFC 4253 11.4; strict key exchange 3.3: the number as
 * counted since the last reset).
 */
static int
transport_send_unimplemented(
	struct ssh_transport *transport)
{
	uint8_t message[TRANSPORT_UNIMPLEMENTED_SIZE];
	uint32_t seq;
	int error;

	/* The packet just taken had the number before the current one (modulo 2^32). */
	seq = transport->packet.receive.seq - 1U;

	/* Builds and sends the answer. */
	message[0] = SSH_MSG_UNIMPLEMENTED;
	ssh_store_be32(message + 1, seq);
	error = transport_send_packet(transport, message, sizeof(message));
	if (error != 0)
		return error;

	/* Succeeded: the answer is in the output. */
	return 0;
}

/*
 * Frames one of the transport's own messages.  The output keeps room for
 * them; when even that is used up the connection cannot go on, and the
 * failure is EMSGSIZE (the output passed its limit) rather than the
 * back-pressure ENOBUFS.
 */
static int
transport_send_packet(
	struct ssh_transport *transport,
	const uint8_t *payload,
	size_t size)
{
	int error;

	/* Frames the message into the output. */
	error = ssh_packet_send(&transport->packet, payload, size);
	if (error == ENOBUFS)
		return EMSGSIZE;
	if (error != 0)
		return error;

	/* Succeeded: the message is in the output. */
	return 0;
}

/*
 * Sends the held upper-layer messages in order, as far as the output has
 * room outside the transport's reserve; the rest stay held for
 * ssh_transport_written.  Returns 0 or an error of framing.
 */
static int
transport_flush_held(
	struct ssh_transport *transport)
{
	struct ssh_buf *held;
	size_t offset;
	size_t size;
	int room;
	int error;

	/* Sends the messages from the front, stopping where the output is full. */
	held = &transport->held;
	offset = 0;
	error = 0;
	while (held->size - offset >= TRANSPORT_HELD_LENGTH) {
		/* Reads the length this side wrote in front of the payload. */
		size = ssh_load_be32(held->data + offset);

		/* Leaves this message and the rest for when the output has room. */
		room = transport_upper_room(transport, size);
		if (room == 0)
			break;

		/* Frames the message and steps over it. */
		error = ssh_packet_send(&transport->packet, held->data + offset + TRANSPORT_HELD_LENGTH, size);
		if (error != 0)
			break;
		offset += TRANSPORT_HELD_LENGTH + size;
	}

	/* Drops the messages that went out, all at once. */
	ssh_buf_consume(held, offset);

	/* Leaves the rest held when the output turned out to be full. */
	if (error == ENOBUFS)
		return 0;

	/* Reports a framing failure. */
	if (error != 0)
		return error;

	/* Succeeded: what fit has been sent. */
	return 0;
}

/*
 * Ends the exchange once NEWKEYS has gone both ways: the first one also
 * ends the initial exchange.  The values of the exchange are wiped.
 */
static void
transport_finish_exchange(
	struct ssh_transport *transport)
{
	int done;

	/* Waits for the other NEWKEYS. */
	done = TRANSPORT_NEWKEYS_SENT | TRANSPORT_NEWKEYS_RECEIVED;
	if ((transport->kex_step & done) != done)
		return;

	/*
	 * kex_step 0 tells every later call that no exchange is in progress;
	 * initial_done ends the restrictions of strict key exchange 3.2 and
	 * lets the peer start re-exchanges; kex_time starts the clock of the
	 * next re-key.
	 */
	transport->kex_step = 0;
	transport->initial_done = 1;
	transport->choice.ignore_next = 0;
	transport->state = SSH_TRANSPORT_RUNNING;
	transport->kex_time = transport->now;

	/* Wipes K, H and C_INIT and drops the KEXINIT payloads, which the next exchange replaces. */
	ssh_wipe(transport->k, sizeof(transport->k));
	ssh_wipe(transport->h, sizeof(transport->h));
	ssh_wipe(transport->c_init, sizeof(transport->c_init));
	ssh_buf_reset(&transport->kexinit_local);
	ssh_buf_reset(&transport->kexinit_peer);
}

/*
 * Ends the connection after a fatal error: queues a DISCONNECT with code
 * and description and closes.  Returns failure, for the caller to pass
 * on.
 */
static int
transport_fail(
	struct ssh_transport *transport,
	int failure,
	uint32_t code,
	const char *description)
{
	/* Tells the peer why, and closes. */
	ssh_transport_disconnect(transport, code, description);

	/* Reports the error that ended the connection. */
	return failure;
}

/*
 * Starts a transport with nothing in it: the role, a copy of ops (zeros
 * for none), an unkeyed packet layer, empty buffers, state VERSION, and
 * the clock.
 */
static void
transport_setup(
	struct ssh_transport *transport,
	enum ssh_role role,
	const struct ssh_transport_ops *ops,
	uint64_t now)
{
	/* Clears every field. */
	memset(transport, 0, sizeof(*transport));

	/* Takes the role and the host key operations. */
	transport->role = role;
	if (ops != NULL)
		transport->ops = *ops;

	/* Starts the packet layer and the buffers; held payloads are plaintext packets, so they are secret. */
	ssh_packet_init(&transport->packet);
	ssh_buf_init(&transport->kexinit_local, SSH_KEXINIT_MAX, 0);
	ssh_buf_init(&transport->kexinit_peer, SSH_KEXINIT_MAX, 0);
	ssh_buf_init(&transport->held, SSH_TRANSPORT_HELD_MAX, 1);

	/* Starts the clock: the keys of the first exchange are timed from its end. */
	transport->kex_time = now;
	transport->now = now;
	transport->state = SSH_TRANSPORT_VERSION;
}

/*
 * Wipes the values of an unfinished exchange (the client's half, C_INIT,
 * K and H) and the held payloads.  The keys of the packet layer stay
 * until ssh_transport_free (the pending output was framed with them
 * already).
 */
static void
transport_forget_secrets(
	struct ssh_transport *transport)
{
	/* Wipes and frees the client's half. */
	ssh_hybrid_client_clear(&transport->hybrid);

	/* Wipes C_INIT, the shared secret and the exchange hash. */
	ssh_wipe(transport->c_init, sizeof(transport->c_init));
	ssh_wipe(transport->k, sizeof(transport->k));
	ssh_wipe(transport->h, sizeof(transport->h));

	/* Wipes and frees the held messages, which will never be sent. */
	ssh_buf_free(&transport->held);
}

/*
 * Reports whether an upper-layer message must be held rather than
 * framed: before this side has a key (VERSION, or the initial exchange
 * before its NEWKEYS), during this side's exchange, and while earlier
 * messages are still held.
 */
static int
transport_holding(
	const struct ssh_transport *transport)
{
	/* No key has been agreed yet. */
	if (transport->state == SSH_TRANSPORT_VERSION)
		return 1;

	/* This side is between its KEXINIT and its NEWKEYS (RFC 4253 7.1). */
	if ((transport->kex_step & TRANSPORT_KEXINIT_SENT) != 0 && (transport->kex_step & TRANSPORT_NEWKEYS_SENT) == 0)
		return 1;

	/* Earlier messages still wait, and the order is kept. */
	if (transport->held.size != 0)
		return 1;

	/* Reports that the message may be framed now. */
	return 0;
}

/*
 * Reports whether a payload of size bytes, framed, fits in the output
 * outside the transport's reserve.
 */
static int
transport_upper_room(
	const struct ssh_transport *transport,
	size_t size)
{
	size_t limit;
	size_t used;

	/* The upper layer may use the output up to the reserve. */
	limit = SSH_PACKET_OUTPUT_MAX - TRANSPORT_OUTPUT_RESERVE;
	used = transport->packet.output.size;

	/* Refuses when the output has no room for even the framing. */
	if (used > limit)
		return 0;
	if (limit - used < TRANSPORT_FRAME_EXTRA)
		return 0;

	/* Refuses when the framed payload would pass the reserve. */
	if (size > limit - used - TRANSPORT_FRAME_EXTRA)
		return 0;

	/* Reports that the message fits. */
	return 1;
}

/*
 * Reports whether a message number belongs to the authentication or the
 * connection protocol or to a local extension (50 to 127, 192 to 255).
 * The service messages 5 and 6 are told apart by the callers.
 */
static int
transport_is_upper(
	unsigned message)
{
	/* Below the user authentication protocol. */
	if (message < TRANSPORT_MSG_UPPER_FIRST)
		return 0;

	/* The reserved range (RFC 4250 4.1.2). */
	if (message >= TRANSPORT_MSG_RESERVED_FIRST && message <= TRANSPORT_MSG_RESERVED_LAST)
		return 0;

	/* Reports an upper-layer number. */
	return 1;
}

/*
 * Reports whether the line of length characters starts with prefix.
 */
static int
transport_has_prefix(
	const char *line,
	size_t length,
	const char *prefix)
{
	size_t prefix_length;
	int difference;

	/* A line shorter than the prefix cannot start with it. */
	prefix_length = strlen(prefix);
	if (length < prefix_length)
		return 0;

	/* Compares the first characters (public text, so memcmp). */
	difference = memcmp(line, prefix, prefix_length);
	if (difference != 0)
		return 0;

	/* Reports that the line starts with the prefix. */
	return 1;
}

/*
 * Checks an identification line from exported bytes: 1 to 253 characters,
 * no NUL, a protocol version 2 prefix.  Returns 0 or EINVAL.
 */
static int
transport_check_line(
	const uint8_t *line,
	size_t size)
{
	const void *nul;
	int v2;
	int v199;

	/* Refuses a missing, empty or overlong line. */
	if (line == NULL || size == 0 || size > TRANSPORT_LINE_CAPACITY - 1U)
		return EINVAL;

	/* Refuses a NUL inside. */
	nul = memchr(line, 0, size);
	if (nul != NULL)
		return EINVAL;

	/* Refuses a line that is not of protocol version 2. */
	v2 = transport_has_prefix((const char *)line, size, TRANSPORT_PREFIX_V2);
	v199 = transport_has_prefix((const char *)line, size, TRANSPORT_PREFIX_V199);
	if (v2 == 0 && v199 == 0)
		return EINVAL;

	/* Succeeded: the line is well formed. */
	return 0;
}

/*
 * Appends one direction of the packet layer in the export layout: key
 * material, seq, wrapped, bytes, packets.  Errors are recorded in out.
 */
static void
transport_put_direction(
	const struct ssh_packet_direction *direction,
	struct ssh_buf *out)
{
	/* Writes K_1 then K_2 as one string of the key material (ssh-cipher.h). */
	ssh_buf_put_u32(out, SSH_CHACHAPOLY_KEY);
	ssh_buf_put_bytes(out, direction->cipher.main_key, sizeof(direction->cipher.main_key));
	ssh_buf_put_bytes(out, direction->cipher.header_key, sizeof(direction->cipher.header_key));

	/* Writes the sequence number, its wrapped flag and the counters of the rekeying limits. */
	ssh_buf_put_u32(out, direction->seq);
	ssh_buf_put_u8(out, (uint8_t)direction->wrapped);
	ssh_buf_put_u64(out, direction->bytes);
	ssh_buf_put_u64(out, direction->packets);
}

/*
 * Reads one direction in the export layout.  *key points at the 64 bytes
 * of key material inside the reader's data.  Returns 0 or EINVAL.
 */
static int
transport_get_direction(
	struct ssh_reader *reader,
	const uint8_t **key,
	uint32_t *seq,
	int *wrapped,
	uint64_t *bytes,
	uint64_t *packets)
{
	size_t key_size;
	unsigned flag;
	int error;

	/* Reads the fields. */
	ssh_reader_get_string(reader, key, &key_size);
	*seq = ssh_reader_get_u32(reader);
	flag = ssh_reader_get_u8(reader);
	*bytes = ssh_reader_get_u64(reader);
	*packets = ssh_reader_get_u64(reader);

	/* Refuses fields that ran past the end. */
	error = ssh_reader_error(reader);
	if (error != 0)
		return EINVAL;

	/* Refuses key material of another size and a flag other than 0 or 1. */
	if (key_size != SSH_CHACHAPOLY_KEY || flag > 1U)
		return EINVAL;

	/* Hands out the flag as an int. */
	*wrapped = (int)flag;

	/* Succeeded: the direction was read. */
	return 0;
}

/*
 * Puts back a direction's sequence number, wrapped flag and counters
 * after its key was set (which cleared the counters).
 */
static void
transport_set_direction(
	struct ssh_packet_direction *direction,
	uint32_t seq,
	int wrapped,
	uint64_t bytes,
	uint64_t packets)
{
	/* Continues the numbering and the rekeying counts where the other process left them. */
	direction->seq = seq;
	direction->wrapped = wrapped;
	direction->bytes = bytes;
	direction->packets = packets;
}
