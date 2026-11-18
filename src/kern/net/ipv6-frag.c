/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * IPv6 fragment reassembly (RFC 8200 section 4.5).
 *
 * The input hands every packet with a fragment header here.  An atomic
 * fragment (offset 0, no more fragments) goes on at once, on its own
 * (RFC 6946).  Any other is kept, as the packet it came in, in the record
 * of its datagram: the source, the destination, the identification and
 * the device it came in on.  When the record holds the whole of the
 * fragmentable part, the pieces are copied into one packet after the
 * offset-zero fragment's unfragmentable part, without the fragment header,
 * and the input walks on from there.
 *
 * A record lasts 60 seconds from its first fragment.  When it runs out
 * with the offset-zero fragment kept, that fragment is quoted in a Time
 * Exceeded (fragment reassembly time exceeded).  Overlapping fragments
 * throw the whole datagram away, and the record stays, empty and dead,
 * until it runs out, so that the datagram's later fragments are thrown
 * away too (RFC 5722); an exact duplicate is the one exception, dropped
 * alone.  A first fragment whose header chain is cut short is refused
 * with a Parameter Problem (RFC 7112).  The pieces' ECN codepoints are
 * merged as RFC 3168 section 5.3 says.
 *
 * A datagram is reassembled only as large as one packet holds, and a TCP
 * segment only as large as a standard packet holds, so that fragments
 * cannot make the connections queue jumbo packets.  The records, the
 * fragments a record keeps, the fragments kept in all and the jumbo
 * packets among them are bounded.  When a bound is reached the oldest
 * record makes way, but not one whose first fragment came less than a
 * second ago: a stream of new datagrams cannot push out one whose
 * fragments are still arriving.
 *
 * One lock covers the records.  Nothing is freed, sent or referenced
 * while it is held: what leaves a record is chained and freed after.
 */

#include "ipv6.h"

#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/lock.h"
#include "wire.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>

/* The fragment header's length, and its offset and more-fragments bits. */
#define IPV6_FRAGMENT_HEADER_LENGTH	8U
#define IPV6_FRAGMENT_OFFSET_MASK	0xfff8U
#define IPV6_FRAGMENT_MORE		0x0001U

/* The largest payload length a reassembled packet may have. */
#define IPV6_FRAGMENT_PAYLOAD_MAX	65535U

/* Where the Payload Length is in the IPv6 header, and the Fragment Offset in the fragment header. */
#define IPV6_FRAGMENT_PAYLOAD_FIELD	4U
#define IPV6_FRAGMENT_OFFSET_FIELD	2U

/* How long a datagram's fragments are waited for (RFC 8200 section 4.5). */
#define IPV6_FRAGMENT_TIMEOUT_MS	60000U

/* How long after its first fragment a live record is kept from making way for another. */
#define IPV6_FRAGMENT_YOUNG_MS		1000U

/* The Time Exceeded code for a reassembly that ran out (RFC 4443 section 3.3). */
#define IPV6_FRAGMENT_TIME_EXCEEDED_CODE	1U

/* The smallest headers of the upper layers a first fragment must hold whole (RFC 7112 section 5). */
#define IPV6_FRAGMENT_UDP_HEADER	8U
#define IPV6_FRAGMENT_TCP_HEADER	20U
#define IPV6_FRAGMENT_ICMPV6_HEADER	4U
#define IPV6_FRAGMENT_PROTOCOL_TCP	6U
#define IPV6_FRAGMENT_PROTOCOL_UDP	17U

/*
 * The ECN field in the IPv6 header's second byte (the low two bits of the
 * traffic class), and its codepoints (RFC 3168 section 5) as the bits of
 * a set of those a datagram's pieces came with.
 */
#define IPV6_FRAGMENT_ECN_SHIFT		4U
#define IPV6_FRAGMENT_ECN_MASK		0x3U
#define IPV6_FRAGMENT_ECN_NOT_ECT	0x1U
#define IPV6_FRAGMENT_ECN_CE		0x8U
#define IPV6_FRAGMENT_ECN_CODE_CE	0x3U

/*
 * The bounds: the records, the fragments one record keeps, the fragments
 * kept in all (an eighth of the standard packets), and the jumbo packets
 * among them (half of those the drivers' receive does not keep back).  A
 * 64-bit machine, with jumbo packets, reassembles up to 9 kilobytes; a
 * 32-bit board's small pool reassembles what two standard packets carry.
 */
#if PACKET_BUF_JUMBO_COUNT != 0
#define IPV6_FRAGMENT_DATAGRAMS		16U
#define IPV6_FRAGMENT_PIECES		32U
#define IPV6_FRAGMENT_HELD		128U
#define IPV6_FRAGMENT_HELD_JUMBO	((PACKET_BUF_JUMBO_COUNT - PACKET_BUF_JUMBO_RESERVE) / 2U)
#else
#define IPV6_FRAGMENT_DATAGRAMS		2U
#define IPV6_FRAGMENT_PIECES		4U
#define IPV6_FRAGMENT_HELD		4U
#define IPV6_FRAGMENT_HELD_JUMBO	0U
#endif

/* The largest reassembled packet, its IPv6 header included: what one packet holds after the headroom. */
#define IPV6_FRAGMENT_LARGEST		(PACKET_BUF_MAX_STORAGE_SIZE - PACKET_BUF_DEFAULT_HEADROOM)

/* The largest reassembled TCP segment's packet: what a standard packet holds after the headroom. */
#define IPV6_FRAGMENT_TCP_LARGEST	(PACKET_BUF_STORAGE_SIZE - PACKET_BUF_DEFAULT_HEADROOM)

/* What a fragment's arrival came to. */
#define IPV6_FRAGMENT_KEPT		1U	/* kept in its record, the datagram not yet whole */
#define IPV6_FRAGMENT_DROPPED		2U	/* not kept: a duplicate, no room, or its datagram is dead */
#define IPV6_FRAGMENT_WHOLE		3U	/* kept, and its datagram is whole */

/* What keeping one more fragment is short of. */
#define IPV6_FRAGMENT_SHORT_NONE	0U	/* nothing: there is room */
#define IPV6_FRAGMENT_SHORT_JUMBO	1U	/* a jumbo packet: the jumbo ones kept are at their bound */
#define IPV6_FRAGMENT_SHORT_ANY		2U	/* a fragment: those kept in all are at their bound */

/*
 * One fragment a record keeps: the packet it came in (its data the
 * fragment's data, its storage still holding the IPv6 header at
 * l3_offset), where its data goes in the fragmentable part, and whether
 * the packet is a jumbo one.
 */
struct ipv6_fragment_piece {
	struct packet_buf *packet;
	unsigned offset;
	unsigned length;
	int jumbo;
};

/*
 * What the input found in an arriving fragment, for the record: its
 * packet (the data the fragment's data), its offset in the fragmentable
 * part and whether more follow, where its fragment header is and the byte
 * that named it, the next header after the fragment header, and for an
 * offset-zero fragment the upper layer its header chain ends in and
 * whether it may be quoted in a Time Exceeded; its ECN codepoint as a bit
 * of the set.
 */
struct ipv6_fragment_arrival {
	struct packet_buf *packet;
	unsigned offset;
	int more;
	size_t start;
	size_t field;
	uint8_t next_header;
	uint8_t upper;
	int quotable;
	unsigned ecn;
};

/*
 * One datagram being reassembled, or a dead one whose fragments are
 * thrown away until it runs out.  The key is the device, the addresses
 * and the identification.  Its pieces are in the order of their offsets
 * and never overlap; jumbo counts the jumbo packets among them.  The end
 * is known once the last fragment came; the offset-zero fragment gives the
 * unfragmentable part's length, the next header after it, the place of the
 * field that named the fragment header, the upper layer, and whether it
 * may be quoted in a Time Exceeded.  ecn is the set of the pieces' ECN
 * codepoints.
 */
struct ipv6_fragment_datagram {
	int used;
	int dead;
	struct net_device *device;
	struct in6_addr source;
	struct in6_addr destination;
	uint32_t identification;
	uint64_t arrived_ms;
	uint64_t expire_ms;
	int end_known;
	unsigned end;
	unsigned received;
	int first_known;
	unsigned unfragmentable;
	uint8_t first_next_header;
	uint8_t first_upper;
	size_t first_field;
	int first_quotable;
	unsigned ecn;
	unsigned count;
	unsigned jumbo;
	struct ipv6_fragment_piece pieces[IPV6_FRAGMENT_PIECES];
};

/*
 * Guards the records and the counts of fragments kept.  Taken by the
 * input and the timer (the network worker) and by a device going away,
 * never with the IPv6 lock held.
 */
static struct spinlock ipv6_fragment_lock;

/*
 * The records.  A record with used 0 is free; a dead one keeps its key
 * and no pieces.  Emptied at boot, filled by the input, emptied by the
 * timer, by a datagram made whole, by a record making way, and by the
 * device going away.
 */
static struct ipv6_fragment_datagram ipv6_fragment_datagrams[IPV6_FRAGMENT_DATAGRAMS];

/*
 * The fragments kept in all the records (the sum of their counts) and the
 * jumbo packets among them (the sum of their jumbo counts), kept at most
 * IPV6_FRAGMENT_HELD and IPV6_FRAGMENT_HELD_JUMBO so that the packet pool
 * keeps the rest for everything else.
 */
static unsigned ipv6_fragment_held;
static unsigned ipv6_fragment_held_jumbo;

static int ipv6_fragment_chain(const struct packet_buf *packet, size_t start, uint8_t next_header, uint8_t *upper, size_t *upper_offset);
static struct ipv6_fragment_datagram *ipv6_fragment_find_locked(const struct net_device *device, const struct in6_addr *source, const struct in6_addr *destination, uint32_t identification);
static struct ipv6_fragment_datagram *ipv6_fragment_take_locked(uint64_t now_ms, struct packet_buf **release);
static unsigned ipv6_fragment_place_locked(struct ipv6_fragment_datagram *datagram, const struct ipv6_fragment_arrival *arrival, uint64_t now_ms, struct packet_buf **release);
static int ipv6_fragment_room_locked(const struct ipv6_fragment_datagram *keep, int jumbo, uint64_t now_ms, struct packet_buf **release);
static unsigned ipv6_fragment_shortage_locked(int jumbo);
static void ipv6_fragment_kill_locked(struct ipv6_fragment_datagram *datagram, struct packet_buf **release);
static void ipv6_fragment_empty_locked(struct ipv6_fragment_datagram *datagram, struct packet_buf **release);
static struct packet_buf *ipv6_fragment_assemble(struct ipv6_fragment_datagram *whole);
static void ipv6_fragment_pieces_free(struct ipv6_fragment_datagram *whole);
static void ipv6_fragment_release(struct packet_buf *release);

/*
 * Initializes the reassembly: no records, nothing kept.
 */
void
ipv6_fragment_init(
	void)
{
	/* The lock, and empty records. */
	spin_init(&ipv6_fragment_lock, LOCK_RANK_NETWORK, "IPv6 fragments");
	kern_memset(ipv6_fragment_datagrams, 0, sizeof(ipv6_fragment_datagrams));
	ipv6_fragment_held = 0U;
	ipv6_fragment_held_jumbo = 0U;
}

/*
 * Takes a packet whose fragment header is at *offset, named by the byte
 * at *field (the packet's data its IPv6 header, at l3_offset).  Returns 0
 * with *packet the packet to walk on with, *next_header the header at the
 * fragmentable part's start, *offset where it is and *field the byte that
 * names it: the same packet past its fragment header for an atomic
 * fragment, or the reassembled datagram (*packet's old value is then gone).
 * Otherwise the packet was consumed (kept, dropped or answered with an
 * error) and an errno value says so: EINPROGRESS when kept.
 */
int
ipv6_fragment_input(
	struct packet_buf **packet,
	uint8_t *next_header,
	size_t *offset,
	size_t *field)
{
	const struct ipv6_fragment_wire *fragment;
	const struct ipv6_wire *header;
	struct ipv6_fragment_datagram *datagram;
	struct ipv6_fragment_datagram whole;
	struct ipv6_fragment_arrival arrival;
	struct packet_buf *arrived;
	struct packet_buf *assembled;
	struct packet_buf *release;
	struct in6_addr source;
	struct in6_addr destination;
	unsigned long irq;
	uint64_t now;
	uint32_t identification;
	uint16_t offset_flags;
	unsigned outcome;
	size_t data_start;
	size_t length;
	size_t upper_offset;
	void *pulled;
	int error;

	/* The packet the walk has reached. */
	arrived = *packet;

	/* The fragment header whole. */
	if (*offset + IPV6_FRAGMENT_HEADER_LENGTH > arrived->length) {
		packet_buf_free(arrived);
		return EINVAL;
	}

	/* Its offset and its flags (the reserved bits are not looked at). */
	fragment = (const struct ipv6_fragment_wire *)(arrived->data + *offset);
	offset_flags = wire_get16(fragment->offset_flags);
	arrival.packet = arrived;
	arrival.offset = offset_flags & IPV6_FRAGMENT_OFFSET_MASK;
	arrival.more = 0;

	/* More fragments follow this one when the flag says so. */
	if ((offset_flags & IPV6_FRAGMENT_MORE) != 0U)
		arrival.more = 1;

	/* Where the fragment header is, what it names, and the fragment's data after it. */
	arrival.start = *offset;
	arrival.field = *field;
	arrival.next_header = fragment->next_header;
	arrival.upper = fragment->next_header;
	arrival.quotable = 1;
	identification = wire_get32(fragment->identification);
	data_start = *offset + IPV6_FRAGMENT_HEADER_LENGTH;
	length = arrived->length - data_start;

	/* An atomic fragment is the whole packet: it goes on alone, past its fragment header (RFC 6946). */
	if (arrival.offset == 0U && !arrival.more) {
		*next_header = arrival.next_header;
		*field = arrival.start;
		*offset = data_start;
		return 0;
	}

	/* A fragment that is not the last is a multiple of eight bytes long: a Parameter Problem at the Payload Length. */
	if (arrival.more && (length % 8U) != 0U) {
		icmp6_error(arrived, ICMP6_PARAMETER_PROBLEM, ICMP6_PARAMETER_HEADER, IPV6_FRAGMENT_PAYLOAD_FIELD);
		return EINVAL;
	}

	/* A fragment that would make the payload longer than 65535 bytes: a Parameter Problem at its Fragment Offset. */
	if ((arrival.start - IPV6_HEADER_LENGTH) + arrival.offset + length > IPV6_FRAGMENT_PAYLOAD_MAX) {
		icmp6_error(arrived, ICMP6_PARAMETER_PROBLEM, ICMP6_PARAMETER_HEADER, (uint32_t)(arrival.start + IPV6_FRAGMENT_OFFSET_FIELD));
		return EINVAL;
	}

	/* A fragment with no data that is not the last carries nothing. */
	if (arrival.more && length == 0U) {
		packet_buf_free(arrived);
		return EINVAL;
	}

	/*
	 * The first fragment holds the whole header chain, up to the upper
	 * layer's header: refused with a Parameter Problem (RFC 7112), or
	 * dropped when it nests another fragment header.
	 */
	if (arrival.offset == 0U) {
		error = ipv6_fragment_chain(arrived, data_start, arrival.next_header, &arrival.upper, &upper_offset);
		if (error == EINVAL) {
			icmp6_error(arrived, ICMP6_PARAMETER_PROBLEM, ICMP6_PARAMETER_FIRST_FRAGMENT, 0U);
			return EINVAL;
		}

		/* A header chain that nests another fragment header is dropped. */
		if (error != 0) {
			packet_buf_free(arrived);
			return EINVAL;
		}

		/* A first fragment that starts an ICMPv6 error message is never quoted in another error (RFC 4443 section 2.4). */
		if (arrival.upper == IPV6_NEXT_ICMPV6 && arrived->data[upper_offset] < 128U)
			arrival.quotable = 0;
	}

	/* The key's addresses, and the ECN codepoint the fragment came with. */
	header = (const struct ipv6_wire *)arrived->data;
	kern_memcpy(source.s6_addr, header->source, 16U);
	kern_memcpy(destination.s6_addr, header->destination, 16U);
	arrival.ecn = 1U << ((header->version_class_flow[1] >> IPV6_FRAGMENT_ECN_SHIFT) & IPV6_FRAGMENT_ECN_MASK);

	/* The packet's data made the fragment's data; the IPv6 header stays in its storage. */
	pulled = packet_buf_pull(arrived, data_start);
	if (pulled == NULL) {
		packet_buf_free(arrived);
		return EINVAL;
	}

	/* The time, and the outcome of a fragment no record takes. */
	now = ipv6_now_ms();
	release = NULL;
	outcome = IPV6_FRAGMENT_DROPPED;

	/*
	 * The fragment placed in its datagram's record, made when it is the
	 * first to come (or dropped when no record can make way); a whole
	 * datagram's record is copied out and emptied.
	 */
	irq = spin_lock_irqsave(&ipv6_fragment_lock);

	/* Finds the record its datagram already has. */
	datagram = ipv6_fragment_find_locked(arrived->device, &source, &destination, identification);

	/* The first fragment of a datagram starts a record, if one is free or can make way. */
	if (datagram == NULL) {
		datagram = ipv6_fragment_take_locked(now, &release);

		/* A record taken is filled with the datagram's key and its time. */
		if (datagram != NULL) {
			datagram->used = 1;
			datagram->device = arrived->device;
			datagram->source = source;
			datagram->destination = destination;
			datagram->identification = identification;
			datagram->arrived_ms = now;
			datagram->expire_ms = now + IPV6_FRAGMENT_TIMEOUT_MS;
		}
	}

	/* A live record takes the fragment; a dead one drops it. */
	if (datagram != NULL && !datagram->dead)
		outcome = ipv6_fragment_place_locked(datagram, &arrival, now, &release);

	/* A datagram made whole is copied out of its record, which is emptied. */
	if (outcome == IPV6_FRAGMENT_WHOLE) {
		kern_memcpy(&whole, datagram, sizeof(whole));
		ipv6_fragment_held -= datagram->count;
		ipv6_fragment_held_jumbo -= datagram->jumbo;
		kern_memset(datagram, 0, sizeof(*datagram));
	}

	spin_unlock_irqrestore(&ipv6_fragment_lock, irq);

	/* What left the records, freed now that the lock is let go. */
	ipv6_fragment_release(release);

	/* A fragment not kept. */
	if (outcome == IPV6_FRAGMENT_DROPPED) {
		packet_buf_free(arrived);
		return EINVAL;
	}

	/* A fragment kept, its datagram still waiting for others. */
	if (outcome == IPV6_FRAGMENT_KEPT)
		return EINPROGRESS;

	/* The whole datagram in one packet; its pieces are freed. */
	assembled = ipv6_fragment_assemble(&whole);
	if (assembled == NULL)
		return ENOBUFS;

	/* Succeeded: the reassembled packet, walked on from its fragmentable part. */
	*packet = assembled;
	*next_header = whole.first_next_header;
	*field = whole.first_field;
	*offset = whole.unfragmentable;
	return 0;
}

/*
 * Runs the reassembly's timer: each record whose time is up is emptied,
 * and a live one that kept its offset-zero fragment answers its source
 * with a Time Exceeded quoting that fragment.
 */
void
ipv6_fragment_timer_run(
	uint64_t now_ms)
{
	struct ipv6_fragment_datagram *datagram;
	struct packet_buf *exceeded[IPV6_FRAGMENT_DATAGRAMS];
	struct packet_buf *release;
	unsigned long irq;
	unsigned exceeded_count;
	unsigned index;
	unsigned piece;

	/* The records that ran out emptied, each live one's quotable offset-zero fragment kept back for its answer. */
	release = NULL;
	exceeded_count = 0U;
	irq = spin_lock_irqsave(&ipv6_fragment_lock);

	/* Each record in use. */
	for (index = 0U; index < IPV6_FRAGMENT_DATAGRAMS; index++) {
		datagram = &ipv6_fragment_datagrams[index];

		/* A record free, or with time left, stays. */
		if (!datagram->used || datagram->expire_ms > now_ms)
			continue;

		/* The offset-zero fragment, when kept, is the first piece. */
		piece = 0U;
		if (!datagram->dead && datagram->first_known && datagram->first_quotable) {
			exceeded[exceeded_count] = datagram->pieces[0].packet;
			exceeded_count++;
			piece = 1U;
		}

		/* The other pieces go with the record, each chained to be freed. */
		while (piece < datagram->count) {
			datagram->pieces[piece].packet->next = release;
			release = datagram->pieces[piece].packet;
			piece++;
		}

		/* The record free, its pieces no longer counted. */
		ipv6_fragment_held -= datagram->count;
		ipv6_fragment_held_jumbo -= datagram->jumbo;
		kern_memset(datagram, 0, sizeof(*datagram));
	}

	spin_unlock_irqrestore(&ipv6_fragment_lock, irq);

	/* The pieces freed. */
	ipv6_fragment_release(release);

	/* Each Time Exceeded sent (it consumes its fragment). */
	for (index = 0U; index < exceeded_count; index++)
		icmp6_error(exceeded[index], ICMP6_TIME_EXCEEDED, IPV6_FRAGMENT_TIME_EXCEEDED_CODE, 0U);
}

/*
 * Gives the time in milliseconds since boot at which the first record
 * runs out (0 for none).
 */
uint64_t
ipv6_fragment_deadline(
	void)
{
	unsigned long irq;
	unsigned index;
	uint64_t deadline;

	/* The earliest time of the records in use. */
	deadline = 0U;
	irq = spin_lock_irqsave(&ipv6_fragment_lock);

	/* Each record in use. */
	for (index = 0U; index < IPV6_FRAGMENT_DATAGRAMS; index++) {
		/* A free record has no time. */
		if (!ipv6_fragment_datagrams[index].used)
			continue;

		/* The earliest time so far, or this one when it is earlier. */
		if (deadline == 0U || ipv6_fragment_datagrams[index].expire_ms < deadline)
			deadline = ipv6_fragment_datagrams[index].expire_ms;
	}

	spin_unlock_irqrestore(&ipv6_fragment_lock, irq);

	/* Succeeded: the earliest, or none. */
	return deadline;
}

/*
 * Forgets the datagrams that came in on a device going away, and frees
 * their fragments (and the device references they hold).
 */
void
ipv6_fragment_purge_device(
	struct net_device *device)
{
	struct packet_buf *release;
	unsigned long irq;
	unsigned index;

	/* The device's records emptied. */
	release = NULL;
	irq = spin_lock_irqsave(&ipv6_fragment_lock);

	/* Each record in use. */
	for (index = 0U; index < IPV6_FRAGMENT_DATAGRAMS; index++) {
		/* A record free, or of another device, stays. */
		if (!ipv6_fragment_datagrams[index].used || ipv6_fragment_datagrams[index].device != device)
			continue;

		/* The device's record emptied, its fragments chained to be freed. */
		ipv6_fragment_empty_locked(&ipv6_fragment_datagrams[index], &release);
	}

	spin_unlock_irqrestore(&ipv6_fragment_lock, irq);

	/* Their fragments freed. */
	ipv6_fragment_release(release);
}

/*
 * Tells whether a first fragment holds its whole header chain, from the
 * next header after the fragment header at start: every extension header
 * the input walks (hop-by-hop, routing, destination options) whole, and
 * the upper layer's smallest header (RFC 7112 section 5).  Sets the upper
 * layer and where its header starts.  Returns 0, EINVAL when the chain is
 * cut short, or ELOOP when it holds another fragment header.
 */
static int
ipv6_fragment_chain(
	const struct packet_buf *packet,
	size_t start,
	uint8_t next_header,
	uint8_t *upper,
	size_t *upper_offset)
{
	const struct ipv6_extension_wire *extension;
	size_t position;
	size_t length;
	size_t needed;

	/* Each extension header in turn, until the upper layer. */
	position = start;
	while (next_header == IPV6_NEXT_HOPOPTS ||
	    next_header == IPV6_NEXT_ROUTING ||
	    next_header == IPV6_NEXT_DSTOPTS ||
	    next_header == IPV6_NEXT_FRAGMENT) {
		/* A fragment header inside a fragment. */
		if (next_header == IPV6_NEXT_FRAGMENT)
			return ELOOP;

		/* The header's start in the fragment. */
		if (position + sizeof(*extension) > packet->length)
			return EINVAL;

		/* The header whole in the fragment. */
		extension = (const struct ipv6_extension_wire *)(packet->data + position);
		length = ((size_t)extension->length + 1U) * 8U;
		if (position + length > packet->length)
			return EINVAL;

		/* The next one. */
		next_header = extension->next_header;
		position += length;
	}

	/* The upper layer's smallest header. */
	needed = 0U;
	if (next_header == IPV6_FRAGMENT_PROTOCOL_UDP)
		needed = IPV6_FRAGMENT_UDP_HEADER;
	else if (next_header == IPV6_FRAGMENT_PROTOCOL_TCP)
		needed = IPV6_FRAGMENT_TCP_HEADER;
	else if (next_header == IPV6_NEXT_ICMPV6)
		needed = IPV6_FRAGMENT_ICMPV6_HEADER;

	/* The upper layer's header whole in the fragment. */
	if (position + needed > packet->length)
		return EINVAL;

	/* Succeeded: the chain is whole, and ends in this upper layer. */
	*upper = next_header;
	*upper_offset = position;
	return 0;
}

/* Finds the record of a datagram by its key (NULL for none). */
static struct ipv6_fragment_datagram *
ipv6_fragment_find_locked(
	const struct net_device *device,
	const struct in6_addr *source,
	const struct in6_addr *destination,
	uint32_t identification)
{
	struct ipv6_fragment_datagram *datagram;
	unsigned index;
	int same;

	/* Each record in use, by its device, identification and addresses. */
	for (index = 0U; index < IPV6_FRAGMENT_DATAGRAMS; index++) {
		datagram = &ipv6_fragment_datagrams[index];

		/* A record free, of another device, or of another identification is not the one. */
		if (!datagram->used ||
		    datagram->device != device ||
		    datagram->identification != identification)
			continue;

		/* A record of another source is not the one. */
		same = in6_equal(&datagram->source, source);
		if (!same)
			continue;

		/* A record with this destination as well is the one. */
		same = in6_equal(&datagram->destination, destination);
		if (same)
			return datagram;
	}

	/* None has the key. */
	return NULL;
}

/*
 * Takes a free record, emptying one to make way when none is: the
 * oldest dead one, else the oldest live one that is not young.  Its
 * fragments are chained on *release.  Returns NULL when every record is
 * live and young.
 */
static struct ipv6_fragment_datagram *
ipv6_fragment_take_locked(
	uint64_t now_ms,
	struct packet_buf **release)
{
	struct ipv6_fragment_datagram *datagram;
	struct ipv6_fragment_datagram *oldest_dead;
	struct ipv6_fragment_datagram *oldest_live;
	unsigned index;

	/* A free record, or the oldest of each kind that may make way. */
	oldest_dead = NULL;
	oldest_live = NULL;
	for (index = 0U; index < IPV6_FRAGMENT_DATAGRAMS; index++) {
		datagram = &ipv6_fragment_datagrams[index];

		/* A free record is taken at once. */
		if (!datagram->used)
			return datagram;

		/* A dead record, or a live one that is not young, may make way; the oldest of each kind is kept in view. */
		if (datagram->dead) {
			/* The oldest dead record so far. */
			if (oldest_dead == NULL || datagram->arrived_ms < oldest_dead->arrived_ms)
				oldest_dead = datagram;
		} else if (now_ms - datagram->arrived_ms >= IPV6_FRAGMENT_YOUNG_MS) {
			/* The oldest live record that is not young so far. */
			if (oldest_live == NULL || datagram->arrived_ms < oldest_live->arrived_ms)
				oldest_live = datagram;
		}
	}

	/* A dead record makes way before a live one; a young one never does. */
	datagram = oldest_dead;
	if (datagram == NULL)
		datagram = oldest_live;

	/* Every record is live and young: none makes way. */
	if (datagram == NULL)
		return NULL;

	/* The record chosen emptied, its fragments chained to be freed. */
	ipv6_fragment_empty_locked(datagram, release);

	/* Succeeded: the record made free. */
	return datagram;
}

/*
 * Places an arriving fragment in its datagram's live record: kept,
 * dropped as an exact duplicate, or, on an overlap, a contradiction or a
 * bound reached, the whole datagram killed and the fragment dropped.
 * What leaves the records is chained on *release.  Returns
 * IPV6_FRAGMENT_KEPT, IPV6_FRAGMENT_DROPPED or IPV6_FRAGMENT_WHOLE.
 */
static unsigned
ipv6_fragment_place_locked(
	struct ipv6_fragment_datagram *datagram,
	const struct ipv6_fragment_arrival *arrival,
	uint64_t now_ms,
	struct packet_buf **release)
{
	struct ipv6_fragment_piece *neighbor;
	struct ipv6_fragment_piece *last;
	struct packet_buf *packet;
	unsigned length;
	unsigned end;
	unsigned furthest;
	unsigned position;
	unsigned index;
	size_t unfragmentable;
	size_t largest;
	uint8_t upper;
	int jumbo;
	int same;
	int room;

	/* The fragment's packet, its length, and where its data ends in the fragmentable part. */
	packet = arrival->packet;
	length = (unsigned)packet->length;
	end = arrival->offset + length;

	/* A packet with more than a standard packet's storage is a jumbo one. */
	jumbo = 0;
	if (packet->capacity > PACKET_BUF_STORAGE_SIZE)
		jumbo = 1;

	/* The furthest any fragment reaches, this one's included: the last piece reaches furthest of those kept. */
	furthest = end;
	if (datagram->count != 0U) {
		/* The last piece kept reaches furthest of those kept. */
		last = &datagram->pieces[datagram->count - 1U];
		if (last->offset + last->length > furthest)
			furthest = last->offset + last->length;
	}

	/* The unfragmentable part and the upper layer, once the offset-zero fragment says (40 bytes at least until then). */
	unfragmentable = IPV6_HEADER_LENGTH;
	upper = 0U;
	if (datagram->first_known) {
		unfragmentable = datagram->unfragmentable;
		upper = datagram->first_upper;
	}

	/* This fragment, when it is the offset-zero one, says it itself. */
	if (arrival->offset == 0U) {
		unfragmentable = arrival->start;
		upper = arrival->upper;
	}

	/* The reassembled packet must fit one packet, and a TCP segment's a standard one. */
	largest = IPV6_FRAGMENT_LARGEST;
	if (upper == IPV6_FRAGMENT_PROTOCOL_TCP)
		largest = IPV6_FRAGMENT_TCP_LARGEST;

	/* A datagram too large for that is killed. */
	if (unfragmentable + furthest > largest) {
		ipv6_fragment_kill_locked(datagram, release);
		return IPV6_FRAGMENT_DROPPED;
	}

	/* The last fragment fixes the end: another end, or a fragment beyond it, contradicts it. */
	if (!arrival->more) {
		/* A second last fragment with another end: the datagram is killed. */
		if (datagram->end_known && end != datagram->end) {
			ipv6_fragment_kill_locked(datagram, release);
			return IPV6_FRAGMENT_DROPPED;
		}

		/* A piece already kept that reaches beyond this end: the datagram is killed. */
		if (furthest > end) {
			ipv6_fragment_kill_locked(datagram, release);
			return IPV6_FRAGMENT_DROPPED;
		}
	} else if (datagram->end_known && end > datagram->end) {
		ipv6_fragment_kill_locked(datagram, release);
		return IPV6_FRAGMENT_DROPPED;
	}

	/* Where it goes: before the first piece that starts at or after it. */
	position = 0U;
	while (position < datagram->count && datagram->pieces[position].offset < arrival->offset)
		position++;

	/* A piece at the same place and of the same length: an exact duplicate is dropped alone, another is an overlap. */
	if (position < datagram->count &&
	    datagram->pieces[position].offset == arrival->offset &&
	    datagram->pieces[position].length == length) {
		/* Compares the two pieces' bytes. */
		same = kern_memcmp(datagram->pieces[position].packet->data, packet->data, length);

		/* The same bytes: an exact duplicate, dropped alone. */
		if (same == 0)
			return IPV6_FRAGMENT_DROPPED;

		/* Other bytes in the same place: an overlap, and the datagram is killed. */
		ipv6_fragment_kill_locked(datagram, release);
		return IPV6_FRAGMENT_DROPPED;
	}

	/* The piece before it must end where it starts or earlier (RFC 5722). */
	if (position > 0U) {
		neighbor = &datagram->pieces[position - 1U];

		/* A piece that reaches into this one: an overlap, and the datagram is killed. */
		if (neighbor->offset + neighbor->length > arrival->offset) {
			ipv6_fragment_kill_locked(datagram, release);
			return IPV6_FRAGMENT_DROPPED;
		}
	}

	/* The piece after it must start where it ends or later (RFC 5722). */
	if (position < datagram->count) {
		neighbor = &datagram->pieces[position];

		/* A piece that starts inside this one: an overlap, and the datagram is killed. */
		if (end > neighbor->offset) {
			ipv6_fragment_kill_locked(datagram, release);
			return IPV6_FRAGMENT_DROPPED;
		}
	}

	/* A datagram cut in more pieces than a record keeps. */
	if (datagram->count >= IPV6_FRAGMENT_PIECES) {
		ipv6_fragment_kill_locked(datagram, release);
		return IPV6_FRAGMENT_DROPPED;
	}

	/* Room among the fragments kept in all (and the jumbo packets), made by older datagrams. */
	room = ipv6_fragment_room_locked(datagram, jumbo, now_ms, release);
	if (!room) {
		ipv6_fragment_kill_locked(datagram, release);
		return IPV6_FRAGMENT_DROPPED;
	}

	/* The pieces after it moved up one. */
	for (index = datagram->count; index > position; index--)
		datagram->pieces[index] = datagram->pieces[index - 1U];

	/*
	 * The piece kept: it counts among the fragments kept in all (and the
	 * jumbo packets when it is one), and its bytes toward the whole.
	 */
	datagram->pieces[position].packet = packet;
	datagram->pieces[position].offset = arrival->offset;
	datagram->pieces[position].length = length;
	datagram->pieces[position].jumbo = jumbo;
	datagram->count++;
	datagram->received += length;
	datagram->ecn |= arrival->ecn;
	ipv6_fragment_held++;

	/* A jumbo packet counts among the jumbo ones kept as well. */
	if (jumbo) {
		datagram->jumbo++;
		ipv6_fragment_held_jumbo++;
	}

	/* The offset-zero fragment gives the unfragmentable part, the next header after it and the upper layer. */
	if (arrival->offset == 0U) {
		datagram->first_known = 1;
		datagram->unfragmentable = (unsigned)arrival->start;
		datagram->first_next_header = arrival->next_header;
		datagram->first_upper = arrival->upper;
		datagram->first_field = arrival->field;
		datagram->first_quotable = arrival->quotable;
	}

	/* The last fragment gives the end. */
	if (!arrival->more) {
		datagram->end_known = 1;
		datagram->end = end;
	}

	/*
	 * Whole when the end and the start are known and the bytes kept add
	 * up to the end: the pieces never overlap, so no gap is left.
	 */
	if (datagram->end_known && datagram->first_known && datagram->received == datagram->end)
		return IPV6_FRAGMENT_WHOLE;

	/* Succeeded: kept, the datagram still waiting. */
	return IPV6_FRAGMENT_KEPT;
}

/*
 * Makes room for one more fragment among those kept in all (and, for a
 * jumbo one, among the jumbo packets), emptying the oldest live records
 * other than keep that are not young.  Returns 1 when there is room.
 */
static int
ipv6_fragment_room_locked(
	const struct ipv6_fragment_datagram *keep,
	int jumbo,
	uint64_t now_ms,
	struct packet_buf **release)
{
	struct ipv6_fragment_datagram *datagram;
	struct ipv6_fragment_datagram *oldest;
	unsigned index;
	unsigned shortage;

	/* The oldest other record that may make way and frees what is short, until nothing is. */
	shortage = ipv6_fragment_shortage_locked(jumbo);
	while (shortage != IPV6_FRAGMENT_SHORT_NONE) {
		/* The oldest record that is not this one, not young, and keeps what is short. */
		oldest = NULL;
		for (index = 0U; index < IPV6_FRAGMENT_DATAGRAMS; index++) {
			datagram = &ipv6_fragment_datagrams[index];

			/* A record free, empty, or the one to keep frees nothing. */
			if (!datagram->used || datagram == keep || datagram->count == 0U)
				continue;

			/* A record with no jumbo packet frees none when those are short. */
			if (shortage == IPV6_FRAGMENT_SHORT_JUMBO && datagram->jumbo == 0U)
				continue;

			/* A young record never makes way. */
			if (now_ms - datagram->arrived_ms < IPV6_FRAGMENT_YOUNG_MS)
				continue;

			/* The oldest of those left so far. */
			if (oldest == NULL || datagram->arrived_ms < oldest->arrived_ms)
				oldest = datagram;
		}

		/* None may make way: no room. */
		if (oldest == NULL)
			return 0;

		/* It makes way; what is short is looked at again. */
		ipv6_fragment_empty_locked(oldest, release);
		shortage = ipv6_fragment_shortage_locked(jumbo);
	}

	/* Succeeded: room for one more. */
	return 1;
}

/*
 * Tells what keeping one more fragment (a jumbo one when jumbo is set)
 * is short of: IPV6_FRAGMENT_SHORT_NONE, IPV6_FRAGMENT_SHORT_JUMBO or
 * IPV6_FRAGMENT_SHORT_ANY.
 */
static unsigned
ipv6_fragment_shortage_locked(
	int jumbo)
{
	/* The jumbo packets kept are at their bound (a board without jumbo packets has a bound of 0). */
	if (jumbo && ipv6_fragment_held_jumbo + 1U > IPV6_FRAGMENT_HELD_JUMBO)
		return IPV6_FRAGMENT_SHORT_JUMBO;

	/* The fragments kept in all are at their bound. */
	if (ipv6_fragment_held >= IPV6_FRAGMENT_HELD)
		return IPV6_FRAGMENT_SHORT_ANY;

	/* Succeeded: room for one more. */
	return IPV6_FRAGMENT_SHORT_NONE;
}

/*
 * Kills a datagram: its fragments are chained on *release, and its
 * record stays, dead, until it runs out, so that its later fragments
 * are dropped too (RFC 5722).
 */
static void
ipv6_fragment_kill_locked(
	struct ipv6_fragment_datagram *datagram,
	struct packet_buf **release)
{
	unsigned index;

	/* The pieces chained to be freed. */
	for (index = 0U; index < datagram->count; index++) {
		datagram->pieces[index].packet->next = *release;
		*release = datagram->pieces[index].packet;
	}

	/*
	 * Dead: no pieces, no start or end known, nothing to answer when it
	 * runs out; the key and the times stay.
	 */
	ipv6_fragment_held -= datagram->count;
	ipv6_fragment_held_jumbo -= datagram->jumbo;
	datagram->dead = 1;
	datagram->count = 0U;
	datagram->jumbo = 0U;
	datagram->received = 0U;
	datagram->end_known = 0;
	datagram->end = 0U;
	datagram->first_known = 0;
	datagram->ecn = 0U;
}

/* Empties a record, chaining its fragments on *release. */
static void
ipv6_fragment_empty_locked(
	struct ipv6_fragment_datagram *datagram,
	struct packet_buf **release)
{
	unsigned index;

	/* The pieces chained to be freed. */
	for (index = 0U; index < datagram->count; index++) {
		datagram->pieces[index].packet->next = *release;
		*release = datagram->pieces[index].packet;
	}

	/* The record free, its pieces no longer counted. */
	ipv6_fragment_held -= datagram->count;
	ipv6_fragment_held_jumbo -= datagram->jumbo;
	kern_memset(datagram, 0, sizeof(*datagram));
}

/*
 * Copies a whole datagram's pieces into one packet: the offset-zero
 * fragment's IPv6 header and unfragmentable part, with its Payload Length
 * worked out again and the field that named the fragment header naming
 * the next header instead (RFC 8200 section 4.5), the ECN codepoint
 * merged (RFC 3168 section 5.3), then the fragmentable part.  The pieces
 * are freed.  Returns the packet, or NULL when none was free or the
 * pieces' ECN codepoints do not go together.
 */
static struct packet_buf *
ipv6_fragment_assemble(
	struct ipv6_fragment_datagram *whole)
{
	struct ipv6_wire *header;
	struct packet_buf *first;
	struct packet_buf *packet;
	uint8_t *data;
	size_t total;
	unsigned index;
	int error;

	/* A piece that is not ECN-capable among ones that are: the datagram is dropped. */
	if ((whole->ecn & IPV6_FRAGMENT_ECN_NOT_ECT) != 0U && whole->ecn != IPV6_FRAGMENT_ECN_NOT_ECT) {
		ipv6_fragment_pieces_free(whole);
		return NULL;
	}

	/* The packet for the whole: the unfragmentable part and the end. */
	first = whole->pieces[0].packet;
	total = (size_t)whole->unfragmentable + whole->end;
	packet = packet_buf_alloc_for(PACKET_BUF_DEFAULT_HEADROOM, total, 0U, &error);
	if (packet == NULL) {
		ipv6_fragment_pieces_free(whole);
		return NULL;
	}

	/* Room in it for the whole. */
	data = packet_buf_append(packet, total);
	if (data == NULL) {
		packet_buf_free(packet);
		ipv6_fragment_pieces_free(whole);
		return NULL;
	}

	/* The headers from the offset-zero fragment, with the payload's new length and the fragment header's next header. */
	kern_memcpy(data, first->storage + first->l3_offset, whole->unfragmentable);
	header = (struct ipv6_wire *)data;
	wire_put16(header->payload_length, (uint16_t)(total - IPV6_HEADER_LENGTH));
	data[whole->first_field] = whole->first_next_header;

	/* Congestion a piece met is the datagram's. */
	if ((whole->ecn & IPV6_FRAGMENT_ECN_CE) != 0U)
		header->version_class_flow[1] |= (uint8_t)(IPV6_FRAGMENT_ECN_CODE_CE << IPV6_FRAGMENT_ECN_SHIFT);

	/* Each piece in its place in the fragmentable part. */
	for (index = 0U; index < whole->count; index++) {
		kern_memcpy(
			data + whole->unfragmentable + whole->pieces[index].offset,
			whole->pieces[index].packet->data,
			whole->pieces[index].length);
	}

	/* Where its header is, and the device it came in on (with a reference of its own). */
	packet->l3_offset = (uint16_t)(packet->data - packet->storage);
	packet->l3_length = (uint16_t)total;
	packet->protocol = first->protocol;
	/* A device checks no transport checksum of a fragment: the whole one is checked above. */
	packet->flags = first->flags & ~PACKET_BUF_RX_CSUM_L4;
	packet->device = first->device;

	/* The reassembled packet holds a reference to the device of its own. */
	if (packet->device != NULL)
		net_device_ref(packet->device);

	/* Where the link-layer source came from. */
	packet->source_length = first->source_length;
	kern_memcpy(packet->source_address, first->source_address, sizeof(packet->source_address));

	/* The pieces freed. */
	ipv6_fragment_pieces_free(whole);

	/* Succeeded: the reassembled packet. */
	return packet;
}

/* Frees the pieces of a record copied out of the records. */
static void
ipv6_fragment_pieces_free(
	struct ipv6_fragment_datagram *whole)
{
	struct packet_buf *release;
	unsigned index;

	/* The pieces chained, then freed. */
	release = NULL;
	for (index = 0U; index < whole->count; index++) {
		whole->pieces[index].packet->next = release;
		release = whole->pieces[index].packet;
	}

	/* The chain freed. */
	ipv6_fragment_release(release);
}

/* Frees a chain of packets that left the records. */
static void
ipv6_fragment_release(
	struct packet_buf *release)
{
	struct packet_buf *next;

	/* Each in turn, its link cleared first. */
	while (release != NULL) {
		next = release->next;
		release->next = NULL;
		packet_buf_free(release);
		release = next;
	}
}
