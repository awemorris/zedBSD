/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The binary packet protocol of one connection.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-packet.h"

/*
 * Starts a packet layer with both directions unkeyed and seq 0.
 */
void
ssh_packet_init(
	struct ssh_packet *packet)
{
	/* Marks the arguments used until the function is implemented. */
	(void)packet;
}

/*
 * Wipes the keys and the buffers and frees the memory.
 */
void
ssh_packet_free(
	struct ssh_packet *packet)
{
	/* Marks the arguments used until the function is implemented. */
	(void)packet;
}

/*
 * Appends size received bytes to the input.
 */
int
ssh_packet_feed(
	struct ssh_packet *packet,
	const uint8_t *data,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)packet;
	(void)data;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Takes the next whole packet from the input and appends its payload to
 * payload (which the caller has reset).
 */
int
ssh_packet_next(
	struct ssh_packet *packet,
	struct ssh_buf *payload)
{
	/* Marks the arguments used until the function is implemented. */
	(void)packet;
	(void)payload;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Frames size bytes of payload as one packet and appends it to the
 * output: chooses the padding (at least SSH_PACKET_PADDING_MIN bytes,
 * alignment as above), fills it with ssh_random, and, keyed, encrypts
 * with ssh_chachapoly_seal and appends the tag.
 */
int
ssh_packet_send(
	struct ssh_packet *packet,
	const uint8_t *payload,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)packet;
	(void)payload;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
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
	/* Marks the arguments used until the function is implemented. */
	(void)packet;
	(void)key;
}

/*
 * As ssh_packet_set_send_key, for the receiving direction.
 */
void
ssh_packet_set_receive_key(
	struct ssh_packet *packet,
	const uint8_t *key)
{
	/* Marks the arguments used until the function is implemented. */
	(void)packet;
	(void)key;
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
	/* Marks the arguments used until the function is implemented. */
	(void)packet;
	(void)send;
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
	/* Marks the arguments used until the function is implemented. */
	(void)packet;
	(void)data;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Takes one line from the front of the input, before any packet (RFC
 * 4253 4.2): the bytes up to the first LF, without the LF and without
 * one CR just before it, copied to line with a NUL after them.
 */
int
ssh_packet_take_line(
	struct ssh_packet *packet,
	char *line,
	size_t capacity,
	size_t *length)
{
	/* Marks the arguments used until the function is implemented. */
	(void)packet;
	(void)line;
	(void)capacity;
	(void)length;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
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
	/* Marks the arguments used until the function is implemented. */
	(void)packet;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return NULL;
}

/*
 * Removes size written bytes from the front of the output.
 */
void
ssh_packet_written(
	struct ssh_packet *packet,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)packet;
	(void)size;
}
