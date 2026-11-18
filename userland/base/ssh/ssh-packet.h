/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The binary packet protocol (RFC 4253 section 6) of one connection: the
 * framing, the padding, the sequence numbers, and the cipher of each
 * direction (none until the first SSH_MSG_NEWKEYS, then
 * chacha20-poly1305@openssh.com).  No MAC algorithm and no compression
 * are ever in use.
 *
 * The layer does no I/O.  Received bytes are appended with
 * ssh_packet_feed and whole payloads are taken out with ssh_packet_next;
 * payloads to send are framed with ssh_packet_send and the bytes to write
 * are taken from out with ssh_packet_pending and ssh_packet_written.
 *
 * Framing of one packet (RFC 4253 6): uint32 packet_length, byte
 * padding_length, payload, random padding, then (keyed) the 16-byte tag.
 * padding_length is 4 to 255.  Unkeyed, the four fields together are a
 * multiple of 8 bytes; keyed, padding_length + payload + padding is a
 * multiple of SSH_CHACHAPOLY_BLOCK (ssh-cipher.h).  The padding bytes come
 * from ssh_random.  A received packet's alignment is not checked (RFC
 * 4253 6 does not tell the receiver to refuse one, and being lenient there
 * costs nothing), but its lengths are.
 */

#ifndef SSH_SSH_PACKET_H
#define SSH_SSH_PACKET_H

#include "ssh-buf.h"
#include "ssh-cipher.h"

/* The smallest padding (RFC 4253 6: at least four bytes). */
#define SSH_PACKET_PADDING_MIN	4U

/* The most bytes of received data held while a packet is incomplete (one packet and its tag, and a little more). */
#define SSH_PACKET_INPUT_MAX	(SSH_PACKET_LIMIT + 4U + SSH_CHACHAPOLY_TAG + 4096U)

/* The most bytes of framed output held before the caller writes them. */
#define SSH_PACKET_OUTPUT_MAX	(4U * 1024U * 1024U)

/*
 * The state of one direction.
 *
 * keyed is 0 until a key is set (the null cipher of RFC 4253 6.3), then 1.
 * cipher holds the keys while keyed.  seq is the sequence number of the
 * next packet (RFC 4253 6.4; it starts at 0, counts every packet, and
 * wraps modulo 2^32 unless ssh_packet_reset_seq sets it to 0).
 * wrapped is 1 once seq has wrapped since the last reset (the transport
 * refuses a wrap before the initial key exchange ends).  bytes and
 * packets count what passed since the last key was set (the rekeying
 * limits of RFC 4253 9).
 */
struct ssh_packet_direction {
	int keyed;
	struct ssh_chachapoly cipher;
	uint32_t seq;
	int wrapped;
	uint64_t bytes;
	uint64_t packets;
};

/*
 * The packet layer of one connection.
 *
 * input holds received bytes not yet made into a packet (secret once
 * keyed: it may hold decrypted bytes of a packet being opened); output
 * holds framed bytes not yet written.  length is the packet_length of the
 * packet at the front of input once it is known (decrypted, when keyed),
 * or 0 while it is not.  The owner calls ssh_packet_free.
 */
struct ssh_packet {
	struct ssh_packet_direction send;
	struct ssh_packet_direction receive;
	struct ssh_buf input;
	struct ssh_buf output;
	uint32_t length;
};

/* Starts a packet layer with both directions unkeyed and seq 0. */
void ssh_packet_init(struct ssh_packet *packet);

/* Wipes the keys and the buffers and frees the memory. */
void ssh_packet_free(struct ssh_packet *packet);

/*
 * Appends size received bytes to the input.
 *
 * Returns 0, or EMSGSIZE when the input would pass SSH_PACKET_INPUT_MAX
 * (the caller stops reading until ssh_packet_next has taken a packet), or
 * ENOMEM.
 */
int ssh_packet_feed(struct ssh_packet *packet, const uint8_t *data, size_t size);

/*
 * Takes the next whole packet from the input and appends its payload to
 * payload (which the caller has reset).
 *
 * Unkeyed: reads packet_length when 4 bytes are there.  Keyed: decrypts
 * it with ssh_chachapoly_length.  packet_length below 5, or above
 * SSH_PACKET_LIMIT, is EMSGSIZE (RFC 4253 6.1); then waits for
 * 4 + packet_length bytes (and the tag when keyed); keyed, opens the
 * packet with ssh_chachapoly_open (EBADMSG on a bad tag).  padding_length
 * below SSH_PACKET_PADDING_MIN or not leaving room for the payload
 * (padding_length + 1 > packet_length) is EINVAL.  On success the packet
 * is removed from the input, receive.seq is incremented (setting wrapped
 * when it passes 2^32 - 1), and receive.bytes and receive.packets grow.
 *
 * Returns 0 (one payload delivered; it may be empty, which the transport
 * refuses), EAGAIN (more bytes are needed), or EMSGSIZE, EINVAL, EBADMSG,
 * ENOMEM, EIO.  After an error other than EAGAIN the layer is unusable and
 * the caller closes the connection (RFC 4253 6.4: a bad packet is not
 * skipped).
 */
int ssh_packet_next(struct ssh_packet *packet, struct ssh_buf *payload);

/*
 * Frames size bytes of payload as one packet and appends it to the
 * output: chooses the padding (at least SSH_PACKET_PADDING_MIN bytes,
 * alignment as above), fills it with ssh_random, and, keyed, encrypts with
 * ssh_chachapoly_seal and appends the tag.  send.seq is incremented
 * (setting wrapped as for receive), send.bytes and send.packets grow.
 *
 * size is 1 to SSH_PACKET_LIMIT - 255 - 1 (EINVAL otherwise; the first
 * payload byte is the message number).  Returns 0, EINVAL, ENOBUFS (the
 * output would pass SSH_PACKET_OUTPUT_MAX: write pending output and send
 * again), ENOMEM or EIO; on an error nothing is appended and the sequence
 * number is unchanged.
 */
int ssh_packet_send(struct ssh_packet *packet, const uint8_t *payload, size_t size);

/*
 * Takes 64 bytes of key material for the sending direction (the next
 * packet sent is encrypted with it) and clears send.bytes and
 * send.packets.  The key is copied.
 */
void ssh_packet_set_send_key(struct ssh_packet *packet, const uint8_t *key);

/* As ssh_packet_set_send_key, for the receiving direction. */
void ssh_packet_set_receive_key(struct ssh_packet *packet, const uint8_t *key);

/*
 * Sets a direction's sequence number to 0 and clears its wrapped flag
 * (strict key exchange's reset after SSH_MSG_NEWKEYS, draft-ietf-sshm-
 * strict-kex-02 3.3).  send is 1 for the sending direction, 0 for the
 * receiving one.
 */
void ssh_packet_reset_seq(struct ssh_packet *packet, int send);

/*
 * Appends size bytes to the output as they are, unframed (the
 * identification line of RFC 4253 4.2, sent before any packet).
 *
 * Returns 0, ENOBUFS or ENOMEM.
 */
int ssh_packet_send_raw(struct ssh_packet *packet, const uint8_t *data, size_t size);

/*
 * Takes one line from the front of the input, before any packet (RFC 4253
 * 4.2): the bytes up to the first LF, without the LF and without one CR
 * just before it, copied to line with a NUL after them.
 *
 * capacity is the room at line including the NUL.  A NUL byte inside the
 * line is EINVAL.  A line whose content (without the CR LF) is longer than
 * capacity - 1 bytes is EMSGSIZE, decided as soon as that many bytes and
 * one more arrived with no LF among them (so a peer cannot make the input
 * grow without a line).  *length receives the line's length.  Returns 0,
 * EAGAIN (no whole line yet), EINVAL or EMSGSIZE.  The transport passes
 * capacity SSH_VERSION_LINE_MAX - 1 (253 characters of content, RFC 4253
 * 4.2's 255 with CR LF).
 */
int ssh_packet_take_line(struct ssh_packet *packet, char *line, size_t capacity, size_t *length);

/*
 * Returns where the framed bytes not yet written start and, in *size, how
 * many there are (NULL and 0 when none).
 */
const uint8_t *ssh_packet_pending(const struct ssh_packet *packet, size_t *size);

/* Removes size written bytes from the front of the output. */
void ssh_packet_written(struct ssh_packet *packet, size_t size);

#endif
