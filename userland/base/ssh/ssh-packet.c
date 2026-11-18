/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The binary packet protocol of one connection.
 *
 * Written from RFC 4253 sections 4.2, 6 and 6.4, and from sections 6 and 7
 * of draft-ietf-sshm-chacha20-poly1305-04 for the keyed form.  The receive
 * side reads the length before the rest of the packet is there, checks it
 * against the limits before it waits for more, verifies the tag before it
 * looks at any decrypted byte, and treats every field of the packet as
 * hostile until then.
 */

#include <string.h>

#include "ssh-packet.h"

/* The size of the packet_length field in front of every packet (RFC 4253 6). */
#define PACKET_LENGTH_SIZE	4U

/* The smallest packet_length a receiver accepts: padding_length, the minimum padding, no payload (RFC 4253 6). */
#define PACKET_LENGTH_MIN	5U

/* The largest padding_length, because it is one byte (RFC 4253 6). */
#define PACKET_PADDING_MAX	255U

/* The largest payload ssh_packet_send frames: the largest packet_length less padding_length and the most padding. */
#define PACKET_PAYLOAD_MAX	(SSH_PACKET_LIMIT - PACKET_PADDING_MAX - 1U)

/* The sequence number after which the next one wraps to 0 (RFC 4253 6.4). */
#define PACKET_SEQ_LAST		0xffffffffU

static void packet_direction_clear(struct ssh_packet_direction *direction);
static void packet_direction_advance(struct ssh_packet_direction *direction, size_t wire_size);
static size_t packet_padding_size(int keyed, size_t payload_size);
static int packet_output_room(struct ssh_packet *packet, size_t size);
static void packet_output_rollback(struct ssh_buf *output, size_t old_size);

/*
 * Starts a packet layer with both directions unkeyed and seq 0.
 *
 * Both buffers are secret: once keyed, the input holds decrypted bytes
 * while a packet is opened and the output holds plaintext while it is
 * framed.
 */
void
ssh_packet_init(
	struct ssh_packet *packet)
{
	/* Starts both directions unkeyed with sequence number 0. */
	memset(&packet->send, 0, sizeof(packet->send));
	memset(&packet->receive, 0, sizeof(packet->receive));

	/* Starts the two buffers empty, each with its limit. */
	ssh_buf_init(&packet->input, SSH_PACKET_INPUT_MAX, 1);
	ssh_buf_init(&packet->output, SSH_PACKET_OUTPUT_MAX, 1);

	/* Marks the length of the packet at the front of the input as not yet known. */
	packet->length = 0;
}

/*
 * Wipes the keys and the buffers and frees the memory.
 */
void
ssh_packet_free(
	struct ssh_packet *packet)
{
	/* Wipes the keys and the counters of both directions. */
	packet_direction_clear(&packet->send);
	packet_direction_clear(&packet->receive);

	/* Wipes and frees the two buffers. */
	ssh_buf_free(&packet->input);
	ssh_buf_free(&packet->output);

	/* Marks the length of the packet at the front of the input as not known. */
	packet->length = 0;
}

/*
 * Appends size received bytes to the input.
 *
 * A refusal because of the limit is not recorded in the buffer: the
 * caller reads again after it has taken a packet.
 */
int
ssh_packet_feed(
	struct ssh_packet *packet,
	const uint8_t *data,
	size_t size)
{
	int error;

	/* Refuses bytes that are missing. */
	if (data == NULL && size != 0)
		return EINVAL;

	/* Refuses bytes that would take the input past its limit, leaving it as it is. */
	if (size > SSH_PACKET_INPUT_MAX - packet->input.size)
		return EMSGSIZE;

	/* Makes room for the bytes. */
	error = ssh_buf_reserve(&packet->input, size);
	if (error != 0) {
		/* Forgets the failure, because nothing was appended and the caller may retry. */
		packet->input.error = 0;
		return error;
	}

	/* Appends the bytes. */
	ssh_buf_put_bytes(&packet->input, data, size);
	error = ssh_buf_error(&packet->input);
	if (error != 0) {
		packet->input.error = 0;
		return error;
	}

	/* Succeeded: the bytes are in the input. */
	return 0;
}

/*
 * Takes the next whole packet from the input and appends its payload to
 * payload (which the caller has reset).
 *
 * The packet_length is decoded and checked first and remembered in
 * packet->length, so waiting for the rest of the packet does not decrypt
 * it again.  The padding fields are read only after the packet is
 * complete and, when keyed, authenticated.
 */
int
ssh_packet_next(
	struct ssh_packet *packet,
	struct ssh_buf *payload)
{
	struct ssh_packet_direction *receive;
	uint32_t packet_length;
	size_t tag_size;
	size_t frame_size;
	size_t total;
	size_t padding;
	size_t payload_size;
	int error;

	/* Points at the receive state of the packet layer. */
	receive = &packet->receive;

	/* Reads the packet_length once four bytes of the packet are there. */
	if (packet->length == 0) {
		if (packet->input.size < PACKET_LENGTH_SIZE)
			return EAGAIN;

		/* Decodes the length: decrypted with the length key when keyed, plain otherwise. */
		if (receive->keyed != 0) {
			error = ssh_chachapoly_length(&receive->cipher, receive->seq, packet->input.data, &packet_length);
			if (error != 0)
				return error;
		} else {
			packet_length = ssh_load_be32(packet->input.data);
		}

		/* Refuses a packet_length outside the limits before waiting for the packet. */
		if (packet_length < PACKET_LENGTH_MIN || packet_length > SSH_PACKET_LIMIT)
			return EMSGSIZE;

		/* Remembers the length until the packet is complete. */
		packet->length = packet_length;
	}

	/* Works out how many bytes the whole packet takes on the wire. */
	tag_size = 0;
	if (receive->keyed != 0)
		tag_size = SSH_CHACHAPOLY_TAG;
	frame_size = (size_t)PACKET_LENGTH_SIZE + (size_t)packet->length;
	total = frame_size + tag_size;

	/* Waits for the rest of the packet and its tag. */
	if (packet->input.size < total)
		return EAGAIN;

	/* Verifies the tag over the encrypted bytes and decrypts them in place. */
	if (receive->keyed != 0) {
		error = ssh_chachapoly_open(&receive->cipher, receive->seq, packet->input.data, frame_size, packet->input.data + frame_size);
		if (error != 0)
			return error;
	}

	/* Refuses a padding_length below the minimum. */
	padding = packet->input.data[PACKET_LENGTH_SIZE];
	if (padding < SSH_PACKET_PADDING_MIN)
		return EINVAL;

	/* Refuses a padding_length that leaves no room for the padding_length byte itself and the padding in the packet. */
	if (padding + 1U > (size_t)packet->length)
		return EINVAL;

	/* Hands the payload to the caller. */
	payload_size = (size_t)packet->length - 1U - padding;
	ssh_buf_put_bytes(payload, packet->input.data + PACKET_LENGTH_SIZE + 1U, payload_size);
	error = ssh_buf_error(payload);
	if (error != 0)
		return error;

	/* Removes the packet from the input, wiping it. */
	ssh_buf_consume(&packet->input, total);
	packet->length = 0;

	/* Counts the packet, which takes the next sequence number. */
	packet_direction_advance(receive, total);

	/* Succeeded: one payload was delivered. */
	return 0;
}

/*
 * Frames size bytes of payload as one packet and appends it to the
 * output: chooses the padding (at least SSH_PACKET_PADDING_MIN bytes,
 * alignment as above), fills it with ssh_random, and, keyed, encrypts
 * with ssh_chachapoly_seal and appends the tag.
 *
 * The whole frame is reserved before anything is written, so a failure
 * before the last step leaves the output as it was.
 */
int
ssh_packet_send(
	struct ssh_packet *packet,
	const uint8_t *payload,
	size_t size)
{
	struct ssh_packet_direction *send;
	uint8_t *frame;
	size_t padding;
	size_t packet_length;
	size_t frame_size;
	size_t total;
	size_t old_size;
	int error;

	/* Points at the send state of the packet layer. */
	send = &packet->send;

	/* Refuses a payload that is missing, empty or longer than a packet can carry. */
	if (payload == NULL || size == 0 || size > PACKET_PAYLOAD_MAX)
		return EINVAL;

	/* Works out the padding, the packet_length and the bytes the frame takes on the wire. */
	padding = packet_padding_size(send->keyed, size);
	packet_length = 1U + size + padding;
	frame_size = (size_t)PACKET_LENGTH_SIZE + packet_length;
	total = frame_size;
	if (send->keyed != 0)
		total += SSH_CHACHAPOLY_TAG;

	/* Makes room for the whole frame, or reports that the output is full. */
	error = packet_output_room(packet, total);
	if (error != 0)
		return error;

	/* Takes the space of the frame at the end of the output. */
	old_size = packet->output.size;
	frame = ssh_buf_put_space(&packet->output, total);
	if (frame == NULL) {
		error = packet->output.error;
		packet->output.error = 0;
		return error;
	}

	/* Writes packet_length, padding_length and the payload. */
	ssh_store_be32(frame, (uint32_t)packet_length);
	frame[PACKET_LENGTH_SIZE] = (uint8_t)padding;
	memcpy(frame + PACKET_LENGTH_SIZE + 1U, payload, size);

	/* Fills the padding with random bytes. */
	error = ssh_random(frame + PACKET_LENGTH_SIZE + 1U + size, padding);
	if (error != 0) {
		packet_output_rollback(&packet->output, old_size);
		return error;
	}

	/* Encrypts the frame in place and writes the tag after it. */
	if (send->keyed != 0) {
		error = ssh_chachapoly_seal(&send->cipher, send->seq, frame, frame_size, frame + frame_size);
		if (error != 0) {
			packet_output_rollback(&packet->output, old_size);
			return error;
		}
	}

	/* Counts the packet, which takes the next sequence number. */
	packet_direction_advance(send, total);

	/* Succeeded: the frame is in the output. */
	return 0;
}

/*
 * Takes 64 bytes of key material for the sending direction (the next
 * packet sent is encrypted with it) and clears send.bytes and
 * send.packets.
 */
void
ssh_packet_set_send_key(
	struct ssh_packet *packet,
	const uint8_t *key)
{
	/* Takes the keys and switches the direction from the null cipher to chacha20-poly1305. */
	ssh_chachapoly_init(&packet->send.cipher, key);
	packet->send.keyed = 1;

	/* Starts counting what the new key protects. */
	packet->send.bytes = 0;
	packet->send.packets = 0;
}

/*
 * As ssh_packet_set_send_key, for the receiving direction.
 */
void
ssh_packet_set_receive_key(
	struct ssh_packet *packet,
	const uint8_t *key)
{
	/* Takes the keys and switches the direction from the null cipher to chacha20-poly1305. */
	ssh_chachapoly_init(&packet->receive.cipher, key);
	packet->receive.keyed = 1;

	/* Starts counting what the new key protects. */
	packet->receive.bytes = 0;
	packet->receive.packets = 0;

	/* Forgets a length read under the earlier cipher, which does not apply to the next packet. */
	packet->length = 0;
}

/*
 * Sets a direction's sequence number to 0 and clears its wrapped flag
 * (strict key exchange's reset after SSH_MSG_NEWKEYS, draft-ietf-sshm-
 * strict-kex-02 3.3).
 */
void
ssh_packet_reset_seq(
	struct ssh_packet *packet,
	int send)
{
	struct ssh_packet_direction *direction;

	/* Chooses the direction. */
	if (send != 0)
		direction = &packet->send;
	else
		direction = &packet->receive;

	/* Starts the numbering again, as if no packet had passed. */
	direction->seq = 0;
	direction->wrapped = 0;
}

/*
 * Appends size bytes to the output as they are, unframed (the
 * identification line of RFC 4253 4.2, sent before any packet).
 */
int
ssh_packet_send_raw(
	struct ssh_packet *packet,
	const uint8_t *data,
	size_t size)
{
	int error;

	/* Refuses bytes that are missing. */
	if (data == NULL && size != 0)
		return EINVAL;

	/* Makes room for the bytes, or reports that the output is full. */
	error = packet_output_room(packet, size);
	if (error != 0)
		return error;

	/* Appends the bytes. */
	ssh_buf_put_bytes(&packet->output, data, size);
	error = ssh_buf_error(&packet->output);
	if (error != 0) {
		packet->output.error = 0;
		return error;
	}

	/* Succeeded: the bytes are in the output. */
	return 0;
}

/*
 * Takes one line from the front of the input, before any packet (RFC
 * 4253 4.2): the bytes up to the first LF, without the LF and without
 * one CR just before it, copied to line with a NUL after them.
 *
 * Only the first capacity + 1 bytes are searched for the LF: a longer line
 * cannot fit whatever its ending is, so the input need not be scanned
 * further and a peer cannot make the search long.
 */
int
ssh_packet_take_line(
	struct ssh_packet *packet,
	char *line,
	size_t capacity,
	size_t *length)
{
	const uint8_t *data;
	size_t available;
	size_t scan;
	size_t max_content;
	size_t index;
	size_t content;
	size_t shortest;
	const void *nul;
	int found;

	/* Refuses a line buffer that cannot even hold the NUL. */
	if (capacity == 0)
		return EINVAL;

	/* Starts with an empty answer, so an error leaves a valid empty string. */
	line[0] = '\0';
	*length = 0;

	/* Takes the buffered input to search for the end of the line. */
	data = packet->input.data;
	available = packet->input.size;
	max_content = capacity - 1U;

	/* Limits the search to the bytes that could still belong to a line that fits: its content, a CR and the LF. */
	scan = available;
	if (available > capacity)
		scan = capacity + 1U;

	/* Looks for the first LF. */
	found = 0;
	index = 0;
	while (index < scan) {
		if (data[index] == '\n') {
			found = 1;
			break;
		}

		/* Advances to the next input byte. */
		index++;
	}

	/* No LF yet: decides whether the line is already too long to fit, or whether to wait. */
	if (found == 0) {
		if (scan == 0)
			return EAGAIN;

		/* A CR at the end may still be the CR of the CR LF ending, so it does not count as content. */
		shortest = scan;
		if (data[scan - 1U] == '\r')
			shortest = scan - 1U;

		/* The content is already longer than the buffer allows whatever comes next. */
		if (shortest > max_content)
			return EMSGSIZE;

		/* Asks for more input, because the line has no end yet. */
		return EAGAIN;
	}

	/* The content is the bytes before the LF, less one CR directly before it. */
	content = index;
	if (content > 0 && data[content - 1U] == '\r')
		content--;

	/* Refuses a line that does not fit. */
	if (content > max_content)
		return EMSGSIZE;

	/* Refuses a NUL inside the line (RFC 4253 4.2). */
	nul = NULL;
	if (content > 0)
		nul = memchr(data, 0, content);
	if (nul != NULL)
		return EINVAL;

	/* Copies the line and ends it with a NUL. */
	if (content > 0)
		memcpy(line, data, content);
	line[content] = '\0';
	*length = content;

	/* Removes the line and its ending from the input. */
	ssh_buf_consume(&packet->input, index + 1U);

	/* Succeeded: one line was taken. */
	return 0;
}

/*
 * Returns where the framed bytes not yet written start and, in *size,
 * how many there are (NULL and 0 when none).
 */
const uint8_t *
ssh_packet_pending(
	const struct ssh_packet *packet,
	size_t *size)
{
	/* Reports that nothing is waiting. */
	if (packet->output.size == 0) {
		*size = 0;
		return NULL;
	}

	/* Reports the bytes waiting to be written. */
	*size = packet->output.size;
	return packet->output.data;
}

/*
 * Removes size written bytes from the front of the output.
 */
void
ssh_packet_written(
	struct ssh_packet *packet,
	size_t size)
{
	/* Drops the bytes the caller has written; more than there are empties the output. */
	ssh_buf_consume(&packet->output, size);
}

/*
 * Wipes the keys of one direction and returns it to the null cipher with
 * all its counters at zero.
 */
static void
packet_direction_clear(
	struct ssh_packet_direction *direction)
{
	/* Wipes the keys. */
	ssh_chachapoly_clear(&direction->cipher);

	/* Returns the direction to its starting state. */
	memset(direction, 0, sizeof(*direction));
}

/*
 * Counts one packet of wire_size bytes that passed in a direction and
 * takes the next sequence number.
 */
static void
packet_direction_advance(
	struct ssh_packet_direction *direction,
	size_t wire_size)
{
	/*
	 * WRAPPED tells the transport that a sequence number has been used
	 * twice since the last reset (RFC 4253 6.4); it is set when the number
	 * passes its largest value.
	 */
	if (direction->seq == PACKET_SEQ_LAST)
		direction->wrapped = 1;
	direction->seq++;

	/* Counts what passed since the key was set, for the rekeying limits (RFC 4253 9). */
	direction->bytes += (uint64_t)wire_size;
	direction->packets++;
}

/*
 * Returns the padding length for a payload: at least the minimum, and so
 * that the packet is aligned to the block size (RFC 4253 6).
 *
 * Unkeyed, the four bytes of the length count in the alignment; keyed,
 * they do not (SSH_CHACHAPOLY_BLOCK in ssh-cipher.h).
 */
static size_t
packet_padding_size(
	int keyed,
	size_t payload_size)
{
	size_t aligned;
	size_t padding;

	/* Counts the bytes that must make a multiple of the block size together with the padding. */
	aligned = 1U + payload_size;
	if (keyed == 0)
		aligned += PACKET_LENGTH_SIZE;

	/* Pads to the next multiple of the block size; a full block is added when the data is already aligned. */
	padding = SSH_CHACHAPOLY_BLOCK - (aligned % SSH_CHACHAPOLY_BLOCK);

	/* Adds a block when the padding falls short of the minimum. */
	if (padding < SSH_PACKET_PADDING_MIN)
		padding += SSH_CHACHAPOLY_BLOCK;

	/* Reports the padding length. */
	return padding;
}

/*
 * Makes room in the output for size more bytes, or reports ENOBUFS when
 * that would pass the output limit.
 *
 * A failed reservation is not left recorded in the buffer: the caller may
 * write the output and try again.
 */
static int
packet_output_room(
	struct ssh_packet *packet,
	size_t size)
{
	int error;

	/* Reports back-pressure when the bytes would not fit under the limit. */
	if (size > SSH_PACKET_OUTPUT_MAX - packet->output.size)
		return ENOBUFS;

	/* Allocates the room. */
	error = ssh_buf_reserve(&packet->output, size);
	if (error != 0) {
		packet->output.error = 0;
		return error;
	}

	/* Succeeded: the output has room for the bytes. */
	return 0;
}

/*
 * Takes back what a frame wrote after old_size, wiping it, so a failed
 * send leaves the output as it was.
 */
static void
packet_output_rollback(
	struct ssh_buf *output,
	size_t old_size)
{
	/* Wipes the part of the frame that may hold plaintext or key-dependent bytes. */
	ssh_wipe(output->data + old_size, output->size - old_size);

	/* Shortens the output to what it held before. */
	output->size = old_size;
}
