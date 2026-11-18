/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Internet checksum.
 */

#include "internal.h"
#include "wire.h"
#include "kern/net/packet-buf.h"

#include <stdint.h>
#include <uapi/netinet.h>

static uint32_t checksum_add(uint32_t sum, const uint8_t *data, size_t length);
static uint16_t checksum_finish(uint32_t sum);

/*
 * Computes the Internet checksum of one buffer.
 */
uint16_t
net_checksum(
	const void *data,
	size_t length)
{
	uint32_t sum;
	uint16_t checksum;

	/* Sums the buffer and folds the carries. */
	sum = checksum_add(0, data, length);
	checksum = checksum_finish(sum);

	/* Reports the checksum. */
	return checksum;
}

/*
 * Computes the Internet checksum of a transport segment with its IPv4
 * pseudo-header.
 */
uint16_t
net_checksum_pseudo(
	uint32_t source,
	uint32_t destination,
	uint8_t protocol,
	const void *data,
	size_t length)
{
	uint8_t pseudo[12];
	uint32_t sum;
	uint16_t checksum;

	/* Builds the pseudo-header in network byte order. */
	pseudo[0] = (uint8_t)(source >> 24);
	pseudo[1] = (uint8_t)(source >> 16);
	pseudo[2] = (uint8_t)(source >> 8);
	pseudo[3] = (uint8_t)source;
	pseudo[4] = (uint8_t)(destination >> 24);
	pseudo[5] = (uint8_t)(destination >> 16);
	pseudo[6] = (uint8_t)(destination >> 8);
	pseudo[7] = (uint8_t)destination;
	pseudo[8] = 0;
	pseudo[9] = protocol;
	pseudo[10] = (uint8_t)(length >> 8);
	pseudo[11] = (uint8_t)length;

	/* Sums the pseudo-header and the segment, then folds the carries. */
	sum = 0;
	sum = checksum_add(sum, pseudo, sizeof(pseudo));
	sum = checksum_add(sum, data, length);
	checksum = checksum_finish(sum);

	/* Reports the checksum. */
	return checksum;
}

/*
 * Computes the folded sum of an IPv4 pseudo-header, not complemented: what
 * a transport checksum's field holds for a device that completes the
 * checksum over the segment (PACKET_BUF_TX_CSUM_TCP).  The sum of the
 * segment with that in its field is then the whole checksum's.
 */
uint16_t
net_checksum_pseudo_partial(
	uint32_t source,
	uint32_t destination,
	uint8_t protocol,
	size_t length)
{
	uint32_t sum;

	/* The addresses, the protocol and the length, as 16-bit words. */
	sum = (source >> 16) + (source & 0xffffU);
	sum += (destination >> 16) + (destination & 0xffffU);
	sum += protocol;
	sum += (uint32_t)length;

	/* Folds the carries. */
	while ((sum >> 16) != 0)
		sum = (sum & 0xffffU) + (sum >> 16);

	/* Reports the sum. */
	return (uint16_t)sum;
}

/*
 * Computes the checksums a packet left to its device (PACKET_BUF_TX_CSUM_*)
 * for a device or a frame that cannot: the IPv4 header's and the TCP
 * segment's, whose field holds the pseudo-header's partial sum.  The
 * packet's l3_offset names the IPv4 header.  A packet that left none is
 * not changed.
 */
void
packet_buf_checksum_finish(
	struct packet_buf *packet)
{
	struct ipv4_wire *header;
	uint8_t *segment;
	uint16_t checksum;
	size_t header_length;
	size_t total;

	/* Nothing was left to the device. */
	if (packet == NULL || (packet->flags & (PACKET_BUF_TX_CSUM_TCP | PACKET_BUF_TX_CSUM_IPV4)) == 0)
		return;
	packet->flags &= ~(PACKET_BUF_TX_CSUM_TCP | PACKET_BUF_TX_CSUM_IPV4);
	if (packet->l3_offset == PACKET_OFFSET_NONE)
		return;

	/* The headers, as the IPv4 output put them. */
	header = (struct ipv4_wire *)(void *)(packet->storage + packet->l3_offset);
	header_length = (size_t)(header->version_ihl & 0x0fU) * 4U;
	total = wire_get16(header->total_length);
	segment = (uint8_t *)header + header_length;

	/* The segment's checksum: the sum over it with the partial one in its field. */
	if (header->protocol == IPPROTO_TCP && total > header_length) {
		checksum = net_checksum(segment, total - header_length);
		wire_put16(segment + 16, checksum);
	}

	/* The header's, over the header with its field zero. */
	wire_put16(header->checksum, 0);
	checksum = net_checksum(header, header_length);
	wire_put16(header->checksum, checksum);
}

/*
 * Computes the Internet checksum of an upper-layer message with its IPv6
 * pseudo-header (RFC 8200 section 8.1): the addresses, the length and
 * the next header.
 */
uint16_t
net_checksum_pseudo6(
	const uint8_t source[16],
	const uint8_t destination[16],
	uint8_t next_header,
	const void *data,
	size_t length)
{
	uint8_t pseudo[8];
	uint32_t sum;
	uint16_t checksum;

	/* The length and the next header, in network byte order. */
	pseudo[0] = (uint8_t)(length >> 24);
	pseudo[1] = (uint8_t)(length >> 16);
	pseudo[2] = (uint8_t)(length >> 8);
	pseudo[3] = (uint8_t)length;
	pseudo[4] = 0;
	pseudo[5] = 0;
	pseudo[6] = 0;
	pseudo[7] = next_header;

	/* Sums the addresses, the rest of the pseudo-header and the message, then folds the carries. */
	sum = 0;
	sum = checksum_add(sum, source, 16U);
	sum = checksum_add(sum, destination, 16U);
	sum = checksum_add(sum, pseudo, sizeof(pseudo));
	sum = checksum_add(sum, data, length);
	checksum = checksum_finish(sum);

	/* Reports the checksum. */
	return checksum;
}

/*
 * Adds the big-endian 16-bit words of a buffer to a running sum.
 *
 * A buffer at an even address is summed a machine word at a time once it
 * is word-aligned: the one's complement sum of a buffer's native-order
 * words, folded to 16 bits, is its big-endian sum with the two bytes
 * swapped on a little-endian machine (RFC 1071 section 2(B)).  The words
 * are only ever read aligned.  An odd address keeps the byte loop.
 */
static uint32_t
checksum_add(
	uint32_t sum,
	const uint8_t *data,
	size_t length)
{
	const uintptr_t *words;
	uint64_t wide;
	uint64_t wide2;
	uintptr_t word;
	uintptr_t word2;
	uint32_t folded;

	/* The leading words, two bytes at a time, until a machine word is aligned; an odd address all the way. */
	while (length >= 2U && (((uintptr_t)data & 1U) != 0 || (uintptr_t)data % sizeof(uintptr_t) != 0)) {
		sum += (uint16_t)((uint16_t)data[0] << 8) | data[1];
		data += 2;
		length -= 2;
	}

	/*
	 * The aligned machine words, their 32-bit halves added into 64 bits,
	 * which cannot overflow here.  Four words a step go into two sums, so
	 * that the additions of one step do not each wait for the one before.
	 */
	if (length >= sizeof(uintptr_t)) {
		wide = 0;
		wide2 = 0;
		words = (const uintptr_t *)(const void *)data;
		while (length >= 4U * sizeof(uintptr_t)) {
			word = words[0];
			word2 = words[1];
			wide += (uint32_t)word;
			wide2 += (uint32_t)word2;
#if UINTPTR_MAX > 0xffffffffU
			wide += (uint32_t)((uint64_t)word >> 32);
			wide2 += (uint32_t)((uint64_t)word2 >> 32);
#endif
			word = words[2];
			word2 = words[3];
			wide += (uint32_t)word;
			wide2 += (uint32_t)word2;
#if UINTPTR_MAX > 0xffffffffU
			wide += (uint32_t)((uint64_t)word >> 32);
			wide2 += (uint32_t)((uint64_t)word2 >> 32);
#endif
			words += 4;
			length -= 4U * sizeof(uintptr_t);
		}
		wide += wide2;
		while (length >= sizeof(uintptr_t)) {
			word = *words;
			wide += (uint32_t)word;
#if UINTPTR_MAX > 0xffffffffU
			wide += (uint32_t)((uint64_t)word >> 32);
#endif
			words++;
			length -= sizeof(uintptr_t);
		}

		/* Continues after the words, with the bytes left over. */
		data = (const uint8_t *)words;

		/* Folds them to 16 bits, in big-endian order, into the running sum. */
		wide = (wide & 0xffffffffU) + (wide >> 32);
		wide = (wide & 0xffffffffU) + (wide >> 32);
		folded = (uint32_t)wide;
		folded = (folded & 0xffffU) + (folded >> 16);
		folded = (folded & 0xffffU) + (folded >> 16);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		folded = ((folded & 0xffU) << 8) | (folded >> 8);
#endif
		sum += folded;
	}

	/* The words left after the last machine word. */
	while (length >= 2U) {
		sum += (uint16_t)((uint16_t)data[0] << 8) | data[1];
		data += 2;
		length -= 2;
	}

	/* Adds a trailing odd byte as the high half of a word. */
	if (length != 0)
		sum += (uint16_t)data[0] << 8;

	/* Reports the running sum. */
	return sum;
}

/* Folds the carries of a running sum and complements it. */
static uint16_t
checksum_finish(
	uint32_t sum)
{
	/* Folds the carry bits back into the low word until none remain. */
	while ((sum >> 16) != 0)
		sum = (sum & 0xffffU) + (sum >> 16);

	/* Reports the one's complement. */
	return (uint16_t)~sum;
}
