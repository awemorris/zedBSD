/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of IPv6 fragment reassembly (src/kern/net/ipv6-frag.c
 * with the real packet pool, built as the kernel builds them): fragments
 * in order, reversed and shuffled; an atomic fragment; overlaps, exact and
 * differing duplicates; a missing fragment and the timer, with and
 * without the offset-zero fragment, and for an ICMPv6 error message; the
 * length checks and the first fragment's header chain; the size, record
 * and fragment bounds; contradicting ends; an unfragmentable part; a
 * device going away; the key's device.  Every test ends with the pool and
 * the device references back where they started.  Built twice by
 * run-frag-host-test.sh: a 64-bit machine and a 32-bit board.  Prints
 * "frag-host-test: PASS" or the checks that failed.
 */

#include "src/kern/net/ipv6.h"

#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/lock.h"
#include "src/kern/net/wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/errno.h>

#include <stdint.h>

/* The headers a test fragment has before its fragment header. */
#define TEST_PLAIN		0U	/* the IPv6 header alone */
#define TEST_DSTOPTS		1U	/* the IPv6 header and 8 bytes of destination options */

/* The next headers the tests carry. */
#define TEST_UDP		17U
#define TEST_TCP		6U

/* The largest reassembled packet the kernel builds (one packet after the headroom). */
#define TEST_LARGEST		(PACKET_BUF_MAX_STORAGE_SIZE - PACKET_BUF_DEFAULT_HEADROOM)

/* The bounds the reassembly is built with: records, fragments a record keeps, fragments kept in all. */
#if PACKET_BUF_JUMBO_COUNT != 0
#define TEST_DATAGRAMS		16U
#define TEST_PIECES		32U
#define TEST_HELD		128U
#define TEST_HELD_JUMBO		((PACKET_BUF_JUMBO_COUNT - PACKET_BUF_JUMBO_RESERVE) / 2U)
#else
#define TEST_DATAGRAMS		2U
#define TEST_PIECES		4U
#define TEST_HELD		4U
#endif

/* One ICMPv6 error the reassembly sent: its type, code and data, and the identification and offset of the fragment it quoted. */
struct test_error {
	unsigned type;
	unsigned code;
	uint32_t data;
	uint32_t identification;
	unsigned offset;
	unsigned quoted_length;
};

/* How many checks ran, and how many failed. */
static unsigned checks;
static unsigned failures;

/* The clock the reassembly reads, moved by the tests. */
static uint64_t test_now_ms;

/* The ICMPv6 errors sent since the last reset, and the last of them. */
static unsigned error_count;
static struct test_error last_error;

/* The two devices fragments come in on, and the references taken on them less those dropped. */
static struct net_device test_devices[2];
static int device_references;

/* The bytes of the datagrams the tests send. */
static uint8_t pattern[65536];

/* The ECN codepoint the next fragments are made with. */
static unsigned test_ecn;

static void check(int condition, const char *what);
static struct packet_buf *fragment_make(struct net_device *device, uint32_t identification, unsigned offset, int more, uint8_t next_header, const uint8_t *data, size_t length, unsigned before);
static int fragment_feed(struct packet_buf **packet, uint8_t *next_header, size_t *offset, size_t *field);
static int fragment_send(struct net_device *device, uint32_t identification, unsigned offset, int more, size_t length, struct packet_buf **whole);
static int whole_check(struct packet_buf *whole, size_t length, size_t unfragmentable);
static void settle(const char *what);
static void test_orders(void);
static void test_atomic(void);
static void test_overlap(void);
static void test_duplicates(void);
static void test_timeout(void);
static void test_lengths(void);
static void test_chain(void);
static void test_size(void);
static void test_records(void);
static void test_held(void);
static void test_pieces(void);
static void test_ends(void);
static void test_unfragmentable(void);
static void test_purge(void);
static void test_devices_key(void);
static void test_jumbo(void);
static void test_tcp_size(void);
static void test_ecn_merge(void);
static void test_mixed_unfragmentable(void);
static void test_allocation(void);
static void test_quoted_error(void);

/* The kcrt copies are the host's. */
void *
kern_memcpy(
	void *destination,
	const void *source,
	size_t count)
{
	return memcpy(destination, source, count);
}

/* The kcrt fill is the host's. */
void *
kern_memset(
	void *destination,
	int value,
	size_t count)
{
	return memset(destination, value, count);
}

/* The kcrt comparison is the host's. */
int
kern_memcmp(
	const void *left,
	const void *right,
	size_t count)
{
	return memcmp(left, right, count);
}

/* The kernel's zeroed allocation is the host's. */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	return calloc(count, size);
}

/* A 32-bit board's atomics enter and leave nothing on the host's one thread. */
uintptr_t
hal_atomic_enter(void)
{
	return 0;
}

/* Leaves what hal_atomic_enter entered. */
void
hal_atomic_leave(
	uintptr_t cookie)
{
	(void)cookie;
}

/* The lock has nothing to guard on the host's one thread. */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	(void)lock;
	(void)rank;
	(void)name;
}

/* Takes nothing; the interrupts were never there. */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	(void)lock;
	return 0;
}

/* Lets go of nothing. */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)lock;
	(void)enabled;
}

/* The test's clock. */
uint64_t
ipv6_now_ms(
	void)
{
	return test_now_ms;
}

/* Counts a device reference taken. */
void
net_device_ref(
	struct net_device *device)
{
	(void)device;
	device_references++;
}

/* Counts a device reference dropped. */
void
net_device_release(
	struct net_device *device)
{
	(void)device;
	device_references--;
}

/* Records an ICMPv6 error and the fragment it quotes (from the packet's IPv6 header at l3_offset), and consumes the packet. */
void
icmp6_error(
	struct packet_buf *packet,
	uint8_t type,
	uint8_t code,
	uint32_t data)
{
	const uint8_t *quoted;
	const struct ipv6_fragment_wire *fragment;

	/* The error, and the fragment header after the IPv6 header (or after 8 bytes of options). */
	quoted = packet->storage + packet->l3_offset;
	last_error.type = type;
	last_error.code = code;
	last_error.data = data;
	fragment = (const struct ipv6_fragment_wire *)(quoted + IPV6_HEADER_LENGTH);
	if (quoted[6] == IPV6_NEXT_DSTOPTS)
		fragment = (const struct ipv6_fragment_wire *)(quoted + IPV6_HEADER_LENGTH + 8U);
	last_error.identification = wire_get32(fragment->identification);
	last_error.offset = wire_get16(fragment->offset_flags) & 0xfff8U;
	last_error.quoted_length = packet->l3_length;
	error_count++;
	packet_buf_free(packet);
}

/* Runs every test and reports the result. */
int
main(void)
{
	unsigned index;

	/* The datagrams' bytes: each a function of its place. */
	for (index = 0U; index < sizeof(pattern); index++)
		pattern[index] = (uint8_t)(index * 7U + (index >> 8));

	/* Each test starts from an empty pool and empty records. */
	packet_buf_pool_init();
	ipv6_fragment_init();
	test_orders();
	test_atomic();
	test_overlap();
	test_duplicates();
	test_timeout();
	test_lengths();
	test_chain();
	test_size();
	test_records();
	test_held();
	test_pieces();
	test_ends();
	test_unfragmentable();
	test_purge();
	test_devices_key();
	test_jumbo();
	test_tcp_size();
	test_ecn_merge();
	test_mixed_unfragmentable();
	test_allocation();
	test_quoted_error();

	/* The verdict. */
	if (failures != 0U) {
		printf("frag-host-test: FAIL (%u of %u)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("frag-host-test: PASS (%u checks, largest %u)\n", checks, (unsigned)TEST_LARGEST);
	return 0;
}

/* Counts and reports a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A check that held says nothing. */
	checks++;
	if (condition)
		return;

	/* A check that failed is counted and named. */
	failures++;
	printf("FAIL: %s\n", what);
}

/*
 * Makes a fragment as the input hands it on: an IPv6 header from
 * 2001:db8::1 to 2001:db8::2, the headers before the fragment header
 * (TEST_PLAIN or TEST_DSTOPTS), the fragment header and the data; its
 * device referenced, l3_offset and l3_length set.
 */
static struct packet_buf *
fragment_make(
	struct net_device *device,
	uint32_t identification,
	unsigned offset,
	int more,
	uint8_t next_header,
	const uint8_t *data,
	size_t length,
	unsigned before)
{
	struct packet_buf *packet;
	struct ipv6_wire *header;
	struct ipv6_fragment_wire *fragment;
	uint8_t *bytes;
	size_t headers;
	uint16_t offset_flags;
	int error;

	/* The packet for the headers and the data. */
	headers = IPV6_HEADER_LENGTH + 8U;
	if (before == TEST_DSTOPTS)
		headers += 8U;
	packet = packet_buf_alloc_for(PACKET_BUF_DEFAULT_HEADROOM, headers + length, PACKET_BUF_ALLOC_RECEIVE, &error);
	if (packet == NULL) {
		check(0, "fragment packet allocated");
		return NULL;
	}

	/* Its bytes. */
	bytes = packet_buf_append(packet, headers + length);
	memset(bytes, 0, headers);
	header = (struct ipv6_wire *)bytes;
	header->version_class_flow[0] = 0x60U;
	header->version_class_flow[1] = (uint8_t)(test_ecn << 4);
	wire_put16(header->payload_length, (uint16_t)(headers + length - IPV6_HEADER_LENGTH));
	header->next_header = IPV6_NEXT_FRAGMENT;
	header->hop_limit = 64U;
	header->source[0] = 0x20U;
	header->source[1] = 0x01U;
	header->source[2] = 0x0dU;
	header->source[3] = 0xb8U;
	header->source[15] = 1U;
	memcpy(header->destination, header->source, 16U);
	header->destination[15] = 2U;

	/* The destination options (one PadN) before the fragment header. */
	fragment = (struct ipv6_fragment_wire *)(bytes + IPV6_HEADER_LENGTH);
	if (before == TEST_DSTOPTS) {
		header->next_header = IPV6_NEXT_DSTOPTS;
		bytes[IPV6_HEADER_LENGTH] = IPV6_NEXT_FRAGMENT;
		bytes[IPV6_HEADER_LENGTH + 1U] = 0U;
		bytes[IPV6_HEADER_LENGTH + 2U] = 1U;
		bytes[IPV6_HEADER_LENGTH + 3U] = 4U;
		fragment = (struct ipv6_fragment_wire *)(bytes + IPV6_HEADER_LENGTH + 8U);
	}

	/* The fragment header and the data. */
	fragment->next_header = next_header;
	offset_flags = (uint16_t)offset;
	if (more)
		offset_flags |= 1U;
	wire_put16(fragment->offset_flags, offset_flags);
	wire_put32(fragment->identification, identification);
	if (length != 0U)
		memcpy(bytes + headers, data, length);

	/* As the input leaves it: the device held, the header's place known. */
	packet->device = device;
	net_device_ref(device);
	packet->l3_offset = (uint16_t)(packet->data - packet->storage);
	packet->l3_length = (uint16_t)(headers + length);

	/* Succeeded: the fragment. */
	return packet;
}

/*
 * Walks a test fragment's headers to its fragment header the way the
 * input does and hands it to the reassembly.  Returns what the
 * reassembly returned.
 */
static int
fragment_feed(
	struct packet_buf **packet,
	uint8_t *next_header,
	size_t *offset,
	size_t *field)
{
	const struct ipv6_wire *header;
	int error;

	/* The IPv6 header's next header, or the destination options'. */
	header = (const struct ipv6_wire *)(*packet)->data;
	*next_header = header->next_header;
	*offset = IPV6_HEADER_LENGTH;
	*field = 6U;
	if (*next_header == IPV6_NEXT_DSTOPTS) {
		*next_header = (*packet)->data[IPV6_HEADER_LENGTH];
		*field = IPV6_HEADER_LENGTH;
		*offset = IPV6_HEADER_LENGTH + 8U;
	}

	/* The reassembly's answer. */
	error = ipv6_fragment_input(packet, next_header, offset, field);
	return error;
}

/*
 * Sends one UDP fragment of the pattern (offset to offset + length) and
 * returns the reassembly's answer; *whole is the reassembled packet when
 * it answered 0, else NULL.
 */
static int
fragment_send(
	struct net_device *device,
	uint32_t identification,
	unsigned offset,
	int more,
	size_t length,
	struct packet_buf **whole)
{
	struct packet_buf *packet;
	uint8_t next_header;
	size_t place;
	size_t field;
	int error;

	/* The fragment, fed. */
	*whole = NULL;
	packet = fragment_make(device, identification, offset, more, TEST_UDP, pattern + offset, length, TEST_PLAIN);
	if (packet == NULL)
		return ENOBUFS;
	error = fragment_feed(&packet, &next_header, &place, &field);
	if (error != 0)
		return error;

	/* The reassembled packet: walked on from after the IPv6 header, at UDP, named by its byte 6. */
	check(next_header == TEST_UDP, "reassembled next header");
	check(place == IPV6_HEADER_LENGTH, "reassembled offset");
	check(field == 6U, "reassembled field");
	*whole = packet;
	return 0;
}

/*
 * Checks a reassembled packet: its total length, Payload Length, its
 * device, its l3 place, the next header naming UDP, and the pattern
 * after the unfragmentable part.  Frees it.  Returns 1 when it is right.
 */
static int
whole_check(
	struct packet_buf *whole,
	size_t length,
	size_t unfragmentable)
{
	const struct ipv6_wire *header;
	uint16_t payload_length;
	int different;
	int right;

	/* Nothing came. */
	if (whole == NULL)
		return 0;

	/* Its header and its bytes. */
	right = 1;
	header = (const struct ipv6_wire *)whole->data;
	if (whole->length != unfragmentable + length)
		right = 0;
	payload_length = wire_get16(header->payload_length);
	if (payload_length != unfragmentable + length - IPV6_HEADER_LENGTH)
		right = 0;
	if (whole->l3_offset != (uint16_t)(whole->data - whole->storage) || whole->l3_length != whole->length)
		right = 0;
	if (whole->device == NULL)
		right = 0;
	if (unfragmentable == IPV6_HEADER_LENGTH && header->next_header != TEST_UDP)
		right = 0;
	if (right) {
		different = memcmp(whole->data + unfragmentable, pattern, length);
		if (different != 0)
			right = 0;
	}

	/* The packet is no longer needed. */
	packet_buf_free(whole);

	/* Whether it was right. */
	return right;
}

/* Runs out every record and checks that nothing is left: no packet, no device reference. */
static void
settle(
	const char *what)
{
	char message[128];

	/* Every record run out, and the errors that made counted apart. */
	test_now_ms += 120000U;
	ipv6_fragment_timer_run(test_now_ms);
	snprintf(message, sizeof(message), "%s: no packet left", what);
	check(packet_buf_in_use() == 0U, message);
	snprintf(message, sizeof(message), "%s: no device reference left", what);
	check(device_references == 0, message);
	snprintf(message, sizeof(message), "%s: no record left", what);
	check(ipv6_fragment_deadline() == 0U, message);
	error_count = 0U;
	memset(&last_error, 0, sizeof(last_error));
}

/* Three fragments in order, reversed, and five shuffled. */
static void
test_orders(void)
{
	struct packet_buf *whole;
	size_t total;
	int error;

	/* In order: kept, kept, then whole. */
	total = 1600U;
	error = fragment_send(&test_devices[0], 1U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "order: first kept");
	error = fragment_send(&test_devices[0], 1U, 600U, 1, 600U, &whole);
	check(error == EINPROGRESS, "order: second kept");
	check(packet_buf_in_use() == 2U, "order: two packets held");
	error = fragment_send(&test_devices[0], 1U, 1200U, 0, 400U, &whole);
	check(error == 0, "order: third makes it whole");
	check(packet_buf_in_use() == 1U, "order: only the whole packet left");
	check(whole_check(whole, total, IPV6_HEADER_LENGTH), "order: reassembled bytes");
	check(error_count == 0U, "order: no error sent");
	settle("order");

	/* Reversed. */
	error = fragment_send(&test_devices[0], 2U, 1200U, 0, 400U, &whole);
	check(error == EINPROGRESS, "reverse: last kept");
	error = fragment_send(&test_devices[0], 2U, 600U, 1, 600U, &whole);
	check(error == EINPROGRESS, "reverse: middle kept");
	error = fragment_send(&test_devices[0], 2U, 0U, 1, 600U, &whole);
	check(error == 0, "reverse: first makes it whole");
	check(whole_check(whole, total, IPV6_HEADER_LENGTH), "reverse: reassembled bytes");
	settle("reverse");

	/* Shuffled, in pieces that need not be the same size. */
	error = fragment_send(&test_devices[0], 3U, 800U, 1, 400U, &whole);
	check(error == EINPROGRESS, "shuffle: 800 kept");
	error = fragment_send(&test_devices[0], 3U, 0U, 1, 400U, &whole);
	check(error == EINPROGRESS, "shuffle: 0 kept");
	error = fragment_send(&test_devices[0], 3U, 1200U, 0, 400U, &whole);
	check(error == EINPROGRESS, "shuffle: last kept");
	error = fragment_send(&test_devices[0], 3U, 400U, 1, 400U, &whole);
	check(error == 0, "shuffle: the gap filled makes it whole");
	check(whole_check(whole, total, IPV6_HEADER_LENGTH), "shuffle: reassembled bytes");
	settle("shuffle");
}

/* An atomic fragment goes on alone, past its header, beside a datagram of the same key. */
static void
test_atomic(void)
{
	struct packet_buf *packet;
	struct packet_buf *whole;
	uint8_t next_header;
	size_t offset;
	size_t field;
	int error;

	/* A datagram of identification 7 under way. */
	error = fragment_send(&test_devices[0], 7U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "atomic: other datagram kept");

	/* The atomic fragment of the same key: the same packet, walked on past the fragment header. */
	packet = fragment_make(&test_devices[0], 7U, 0U, 0, TEST_UDP, pattern, 100U, TEST_PLAIN);
	whole = packet;
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == 0, "atomic: goes on");
	check(packet == whole, "atomic: the same packet");
	check(next_header == TEST_UDP, "atomic: next header");
	check(offset == IPV6_HEADER_LENGTH + 8U, "atomic: offset past the fragment header");
	check(field == IPV6_HEADER_LENGTH, "atomic: field is the fragment header's");
	check(packet_buf_in_use() == 2U, "atomic: the other datagram still kept");
	packet_buf_free(packet);

	/* The other datagram still completes. */
	error = fragment_send(&test_devices[0], 7U, 600U, 0, 200U, &whole);
	check(error == 0, "atomic: other datagram whole");
	check(whole_check(whole, 800U, IPV6_HEADER_LENGTH), "atomic: other datagram bytes");
	settle("atomic");
}

/* An overlap throws the datagram away, its later fragments too, without an error. */
static void
test_overlap(void)
{
	struct packet_buf *whole;
	int error;

	/* Two fragments that overlap by eight bytes. */
	error = fragment_send(&test_devices[0], 10U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "overlap: first kept");
	error = fragment_send(&test_devices[0], 10U, 592U, 1, 600U, &whole);
	check(error == EINVAL, "overlap: overlapping one dropped");
	check(packet_buf_in_use() == 0U, "overlap: the first freed too");

	/* The rest of the datagram, dropped, and nothing answered when it runs out. */
	error = fragment_send(&test_devices[0], 10U, 600U, 0, 400U, &whole);
	check(error == EINVAL, "overlap: later fragment dropped");
	error = fragment_send(&test_devices[0], 10U, 0U, 1, 600U, &whole);
	check(error == EINVAL, "overlap: first again dropped");
	check(packet_buf_in_use() == 0U, "overlap: nothing kept");
	test_now_ms += 60000U;
	ipv6_fragment_timer_run(test_now_ms);
	check(error_count == 0U, "overlap: no Time Exceeded");

	/* The key free again after it ran out. */
	error = fragment_send(&test_devices[0], 10U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "overlap: key free after the time");
	error = fragment_send(&test_devices[0], 10U, 600U, 0, 400U, &whole);
	check(whole_check(whole, 1000U, IPV6_HEADER_LENGTH), "overlap: new datagram whole");

	/* A fragment inside another: an overlap too. */
	error = fragment_send(&test_devices[0], 11U, 0U, 1, 800U, &whole);
	check(error == EINPROGRESS, "overlap: wide kept");
	error = fragment_send(&test_devices[0], 11U, 200U, 1, 200U, &whole);
	check(error == EINVAL, "overlap: inner dropped");
	check(packet_buf_in_use() == 0U, "overlap: wide freed");
	settle("overlap");
}

/* An exact duplicate is dropped alone; a duplicate with other bytes kills the datagram. */
static void
test_duplicates(void)
{
	struct packet_buf *packet;
	struct packet_buf *whole;
	uint8_t other[600];
	uint8_t next_header;
	size_t offset;
	size_t field;
	int error;

	/* The same fragment twice, then the rest. */
	error = fragment_send(&test_devices[0], 20U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "duplicate: first kept");
	error = fragment_send(&test_devices[0], 20U, 0U, 1, 600U, &whole);
	check(error == EINVAL, "duplicate: second copy dropped");
	check(packet_buf_in_use() == 1U, "duplicate: the first still kept");
	error = fragment_send(&test_devices[0], 20U, 600U, 0, 100U, &whole);
	check(error == 0, "duplicate: datagram whole");
	check(whole_check(whole, 700U, IPV6_HEADER_LENGTH), "duplicate: bytes");
	settle("duplicate");

	/* The same place and length with other bytes. */
	error = fragment_send(&test_devices[0], 21U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "differing: first kept");
	memcpy(other, pattern, sizeof(other));
	other[300] ^= 0xffU;
	packet = fragment_make(&test_devices[0], 21U, 0U, 1, TEST_UDP, other, sizeof(other), TEST_PLAIN);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == EINVAL, "differing: dropped");
	check(packet_buf_in_use() == 0U, "differing: datagram thrown away");
	error = fragment_send(&test_devices[0], 21U, 600U, 0, 100U, &whole);
	check(error == EINVAL, "differing: later fragment dropped");
	settle("differing");
}

/* A missing fragment: the timer, with and without the offset-zero fragment, and for an ICMPv6 error message. */
static void
test_timeout(void)
{
	struct packet_buf *packet;
	struct packet_buf *whole;
	uint8_t message[600];
	uint8_t next_header;
	size_t offset;
	size_t field;
	uint64_t started;
	int error;

	/* The first and the last of three, the middle never coming. */
	started = test_now_ms;
	error = fragment_send(&test_devices[0], 30U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "timeout: first kept");
	test_now_ms += 1000U;
	error = fragment_send(&test_devices[0], 30U, 1200U, 0, 400U, &whole);
	check(error == EINPROGRESS, "timeout: last kept");
	check(ipv6_fragment_deadline() == started + 60000U, "timeout: due 60 s after the first arrival");

	/* Not yet at 59.999 s. */
	test_now_ms = started + 59999U;
	ipv6_fragment_timer_run(test_now_ms);
	check(error_count == 0U, "timeout: nothing before 60 s");
	check(packet_buf_in_use() == 2U, "timeout: both still kept");

	/* At 60 s: one Time Exceeded, code 1, quoting the offset-zero fragment. */
	test_now_ms = started + 60000U;
	ipv6_fragment_timer_run(test_now_ms);
	check(error_count == 1U, "timeout: one error");
	check(last_error.type == ICMP6_TIME_EXCEEDED && last_error.code == 1U, "timeout: Time Exceeded code 1");
	check(last_error.identification == 30U && last_error.offset == 0U, "timeout: quotes the offset-zero fragment");
	check(last_error.quoted_length == IPV6_HEADER_LENGTH + 8U + 600U, "timeout: quotes the whole fragment");
	check(packet_buf_in_use() == 0U, "timeout: everything freed");
	settle("timeout");

	/* Without the offset-zero fragment: nothing is sent. */
	error = fragment_send(&test_devices[0], 31U, 600U, 1, 600U, &whole);
	check(error == EINPROGRESS, "timeout without first: kept");
	error = fragment_send(&test_devices[0], 31U, 1200U, 0, 400U, &whole);
	check(error == EINPROGRESS, "timeout without first: last kept");
	test_now_ms += 60000U;
	ipv6_fragment_timer_run(test_now_ms);
	check(error_count == 0U, "timeout without first: no error");
	settle("timeout without first");

	/* An ICMPv6 error message in fragments: never answered with another error. */
	memcpy(message, pattern, sizeof(message));
	message[0] = 1U;
	packet = fragment_make(&test_devices[0], 32U, 0U, 1, IPV6_NEXT_ICMPV6, message, sizeof(message), TEST_PLAIN);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == EINPROGRESS, "timeout of an error message: kept");
	test_now_ms += 60000U;
	ipv6_fragment_timer_run(test_now_ms);
	check(error_count == 0U, "timeout of an error message: no error");
	settle("timeout of an error message");

	/* An echo request in fragments is answered. */
	message[0] = ICMP6_ECHO_REQUEST;
	packet = fragment_make(&test_devices[0], 33U, 0U, 1, IPV6_NEXT_ICMPV6, message, sizeof(message), TEST_PLAIN);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == EINPROGRESS, "timeout of an echo: kept");
	test_now_ms += 60000U;
	ipv6_fragment_timer_run(test_now_ms);
	check(error_count == 1U, "timeout of an echo: Time Exceeded");
	settle("timeout of an echo");
}

/* A fragment not a multiple of eight, and one past 65535 bytes: Parameter Problems at the right fields; an empty one: dropped. */
static void
test_lengths(void)
{
	struct packet_buf *whole;
	int error;

	/* Not the last, and 601 bytes long. */
	error = fragment_send(&test_devices[0], 40U, 0U, 1, 601U, &whole);
	check(error == EINVAL, "length: odd fragment refused");
	check(error_count == 1U, "length: one error");
	check(last_error.type == ICMP6_PARAMETER_PROBLEM && last_error.code == 0U && last_error.data == 4U, "length: Parameter Problem at the Payload Length");
	settle("length");

	/* The last fragment of an odd length is fine. */
	error = fragment_send(&test_devices[0], 41U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "odd last: first kept");
	error = fragment_send(&test_devices[0], 41U, 600U, 0, 5U, &whole);
	check(error == 0, "odd last: whole");
	check(whole_check(whole, 605U, IPV6_HEADER_LENGTH), "odd last: bytes");
	settle("odd last");

	/* Past 65535 bytes of payload. */
	error = fragment_send(&test_devices[0], 42U, 65528U, 0, 8U, &whole);
	check(error == EINVAL, "too long: refused");
	check(error_count == 1U, "too long: one error");
	check(last_error.type == ICMP6_PARAMETER_PROBLEM && last_error.code == 0U && last_error.data == IPV6_HEADER_LENGTH + 2U, "too long: Parameter Problem at the Fragment Offset");
	settle("too long");

	/* No data and more to come. */
	error = fragment_send(&test_devices[0], 43U, 600U, 1, 0U, &whole);
	check(error == EINVAL, "empty: dropped");
	check(error_count == 0U, "empty: no error");
	settle("empty");
}

/* The first fragment's header chain: cut short (code 3), whole, and a nested fragment header. */
static void
test_chain(void)
{
	struct packet_buf *packet;
	uint8_t data[16];
	uint8_t next_header;
	size_t offset;
	size_t field;
	int error;

	/* Destination options claiming 24 bytes in a first fragment of 16. */
	memset(data, 0, sizeof(data));
	data[0] = TEST_UDP;
	data[1] = 2U;
	packet = fragment_make(&test_devices[0], 50U, 0U, 1, IPV6_NEXT_DSTOPTS, data, sizeof(data), TEST_PLAIN);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == EINVAL, "chain: cut options refused");
	check(last_error.type == ICMP6_PARAMETER_PROBLEM && last_error.code == 3U && last_error.data == 0U, "chain: Parameter Problem code 3");
	settle("chain options");

	/* A TCP header of 20 bytes in 16. */
	packet = fragment_make(&test_devices[0], 51U, 0U, 1, TEST_TCP, data, sizeof(data), TEST_PLAIN);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == EINVAL, "chain: cut TCP refused");
	check(last_error.code == 3U, "chain: TCP code 3");
	settle("chain tcp");

	/* Destination options of 8 bytes and UDP's 8 in 16: whole. */
	data[1] = 0U;
	packet = fragment_make(&test_devices[0], 52U, 0U, 1, IPV6_NEXT_DSTOPTS, data, sizeof(data), TEST_PLAIN);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == EINPROGRESS, "chain: whole chain kept");
	check(error_count == 0U, "chain: no error");
	settle("chain whole");

	/* A fragment header inside the first fragment: dropped without an error. */
	data[0] = IPV6_NEXT_FRAGMENT;
	packet = fragment_make(&test_devices[0], 53U, 0U, 1, IPV6_NEXT_DSTOPTS, data, sizeof(data), TEST_PLAIN);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == EINVAL, "chain: nested fragment dropped");
	check(error_count == 0U, "chain: nested no error");
	settle("chain nested");
}

/* The largest datagram one packet holds is reassembled; eight bytes more kill it. */
static void
test_size(void)
{
	struct packet_buf *whole;
	size_t fragmentable;
	size_t piece;
	size_t done;
	int error;

	/* The largest, in pieces of 1232 bytes. */
	fragmentable = TEST_LARGEST - IPV6_HEADER_LENGTH;
	done = 0U;
	error = 0;
	while (done < fragmentable) {
		piece = 1232U;
		if (fragmentable - done <= piece)
			piece = fragmentable - done;
		error = fragment_send(&test_devices[0], 60U, (unsigned)done, done + piece < fragmentable, piece, &whole);
		done += piece;
	}

	/* The largest datagram whole, and its bytes right. */
	check(error == 0, "size: largest whole");
	check(whole_check(whole, fragmentable, IPV6_HEADER_LENGTH), "size: largest bytes");
	settle("size largest");

	/* Eight bytes past it: the datagram dies, its later fragments are dropped. */
	error = fragment_send(&test_devices[0], 61U, 0U, 1, 504U, &whole);
	check(error == EINPROGRESS, "size: first kept");
	error = fragment_send(&test_devices[0], 61U, (unsigned)((fragmentable + 8U) & ~(size_t)7U) - 8U, 0, 16U, &whole);
	check(error == EINVAL, "size: past the largest dropped");
	check(packet_buf_in_use() == 0U, "size: first freed");
	error = fragment_send(&test_devices[0], 61U, 504U, 1, 504U, &whole);
	check(error == EINVAL, "size: later fragment dropped");
	test_now_ms += 60000U;
	ipv6_fragment_timer_run(test_now_ms);
	check(error_count == 0U, "size: no Time Exceeded");
	settle("size past");
}

/* More datagrams than records: a young record stays, an older one makes way, a dead one first. */
static void
test_records(void)
{
	struct packet_buf *whole;
	unsigned index;
	int error;

	/* Every record filled at once. */
	for (index = 0U; index < TEST_DATAGRAMS; index++) {
		error = fragment_send(&test_devices[0], 100U + index, 8U, 1, 8U, &whole);
		check(error == EINPROGRESS, "records: kept");
	}

	/* Every record keeps one packet. */
	check(packet_buf_in_use() == TEST_DATAGRAMS, "records: one packet a record");

	/* One more while they are all young: it is the one dropped. */
	error = fragment_send(&test_devices[0], 199U, 8U, 1, 8U, &whole);
	check(error == EINVAL, "records: no young record makes way");
	check(packet_buf_in_use() == TEST_DATAGRAMS, "records: all still kept");

	/* A second on, the oldest makes way, and the newcomer completes. */
	test_now_ms += 1100U;
	error = fragment_send(&test_devices[0], 199U, 8U, 1, 8U, &whole);
	check(error == EINPROGRESS, "records: an old record made way");
	check(packet_buf_in_use() == TEST_DATAGRAMS, "records: still one packet a record");
	error = fragment_send(&test_devices[0], 199U, 0U, 1, 8U, &whole);
	error = fragment_send(&test_devices[0], 199U, 16U, 0, 8U, &whole);
	check(error == 0, "records: newcomer completes");
	if (whole != NULL)
		packet_buf_free(whole);
	settle("records");

	/* A dead record makes way even when young; the live ones stay. */
	error = fragment_send(&test_devices[0], 200U, 0U, 1, 16U, &whole);
	check(error == EINPROGRESS, "dead first: live kept");
	error = fragment_send(&test_devices[0], 201U, 0U, 1, 16U, &whole);
	error = fragment_send(&test_devices[0], 201U, 8U, 1, 16U, &whole);
	check(error == EINVAL, "dead first: overlap kills");
	for (index = 0U; index + 2U < TEST_DATAGRAMS; index++)
		error = fragment_send(&test_devices[0], 300U + index, 8U, 1, 8U, &whole);
	error = fragment_send(&test_devices[0], 400U, 8U, 1, 8U, &whole);
	check(error == EINPROGRESS, "dead first: one more kept in the dead one's place");
	error = fragment_send(&test_devices[0], 200U, 16U, 0, 8U, &whole);
	check(error == 0, "dead first: the live one stayed");
	if (whole != NULL)
		packet_buf_free(whole);
	settle("dead first");
}

/* More fragments in all than are kept: the oldest other datagram makes way. */
static void
test_held(void)
{
	struct packet_buf *whole;
	unsigned pieces;
	unsigned held;
	unsigned in_use;
	unsigned index;
	unsigned identification;
	int error;

	/* Datagrams of several pieces with gaps, a second apart, until well past the bound of fragments kept in all. */
	pieces = (TEST_PIECES - 1U) / 2U;
	if (pieces < 2U)
		pieces = TEST_PIECES - 1U;
	held = 0U;
	error = 0;
	for (identification = 500U; identification < 500U + 2U * TEST_HELD / pieces + 2U; identification++) {
		for (index = 0U; index < pieces; index++) {
			error = fragment_send(&test_devices[0], identification, 16U * index + 8U, 1, 8U, &whole);
			in_use = packet_buf_in_use();
			if (in_use > held)
				held = in_use;
		}

	/* A second on, so that the next datagram is the newer. */
		test_now_ms += 1100U;
	}

	/* The newest datagram is still kept. */
	check(error == EINPROGRESS, "held: the newest still kept");
	check(held == TEST_HELD, "held: reaches the bound and no further");

	/* The newest datagram is whole once its gaps come, when a record keeps that many pieces. */
	identification--;
	if (2U * pieces + 1U <= TEST_PIECES) {
		for (index = 0U; index < pieces; index++)
			error = fragment_send(&test_devices[0], identification, 16U * index, 1, 8U, &whole);
		error = fragment_send(&test_devices[0], identification, 16U * pieces, 0, 8U, &whole);
		check(error == 0, "held: newest whole");
		check(whole_check(whole, 16U * pieces + 8U, IPV6_HEADER_LENGTH), "held: newest bytes");
	}

	/* Every record run out, nothing left. */
	settle("held");
}

/* A datagram in more pieces than a record keeps dies. */
static void
test_pieces(void)
{
	struct packet_buf *whole;
	unsigned index;
	unsigned kept;
	int error;

	/* Eight-byte pieces with gaps between, until one is refused. */
	kept = 0U;
	error = EINPROGRESS;
	for (index = 0U; index < 200U && error == EINPROGRESS; index++) {
		error = fragment_send(&test_devices[0], 600U, 16U * index + 8U, 1, 8U, &whole);
		if (error == EINPROGRESS)
			kept++;
	}

	/* The datagram is refused at the bound, and thrown away. */
	check(error == EINVAL, "pieces: refused at the bound");
	check(kept == TEST_PIECES, "pieces: as many as the bound");
	check(packet_buf_in_use() == 0U, "pieces: datagram thrown away");
	settle("pieces");
}

/* Two last fragments with other ends, and a fragment past the end: the datagram dies. */
static void
test_ends(void)
{
	struct packet_buf *whole;
	int error;

	/* Two ends. */
	error = fragment_send(&test_devices[0], 700U, 600U, 0, 400U, &whole);
	check(error == EINPROGRESS, "ends: last kept");
	error = fragment_send(&test_devices[0], 700U, 1000U, 0, 400U, &whole);
	check(error == EINVAL, "ends: another end dropped");
	check(packet_buf_in_use() == 0U, "ends: datagram thrown away");
	settle("ends two");

	/* A fragment past the known end. */
	error = fragment_send(&test_devices[0], 701U, 600U, 0, 400U, &whole);
	check(error == EINPROGRESS, "ends: last kept again");
	error = fragment_send(&test_devices[0], 701U, 1000U, 1, 400U, &whole);
	check(error == EINVAL, "ends: past the end dropped");
	settle("ends past");

	/* A last fragment before a kept one's end. */
	error = fragment_send(&test_devices[0], 702U, 600U, 1, 600U, &whole);
	check(error == EINPROGRESS, "ends: middle kept");
	error = fragment_send(&test_devices[0], 702U, 400U, 0, 8U, &whole);
	check(error == EINVAL, "ends: an end before a kept fragment dropped");
	settle("ends before");
}

/* Fragments with destination options before the fragment header: the offset-zero one's are kept, renamed to UDP. */
static void
test_unfragmentable(void)
{
	struct packet_buf *packet;
	const struct ipv6_wire *header;
	uint8_t next_header;
	size_t offset;
	size_t field;
	int error;

	/* The second piece first, then the offset-zero one. */
	packet = fragment_make(&test_devices[0], 800U, 600U, 0, TEST_UDP, pattern + 600U, 300U, TEST_DSTOPTS);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == EINPROGRESS, "unfragmentable: last kept");
	packet = fragment_make(&test_devices[0], 800U, 0U, 1, TEST_UDP, pattern, 600U, TEST_DSTOPTS);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == 0, "unfragmentable: whole");

	/* The options kept, naming UDP now; the walk goes on after them. */
	check(next_header == TEST_UDP, "unfragmentable: next header");
	check(offset == IPV6_HEADER_LENGTH + 8U, "unfragmentable: offset after the options");
	check(field == IPV6_HEADER_LENGTH, "unfragmentable: field is the options' first byte");
	header = (const struct ipv6_wire *)packet->data;
	check(header->next_header == IPV6_NEXT_DSTOPTS, "unfragmentable: IPv6 header names the options");
	check(packet->data[IPV6_HEADER_LENGTH] == TEST_UDP, "unfragmentable: options name UDP");
	check(packet->data[IPV6_HEADER_LENGTH + 2U] == 1U, "unfragmentable: options' PadN kept");
	check(whole_check(packet, 900U, IPV6_HEADER_LENGTH + 8U), "unfragmentable: bytes and length");
	settle("unfragmentable");
}

/* A device going away takes its datagrams' fragments and their references. */
static void
test_purge(void)
{
	struct packet_buf *whole;
	int error;

	/* One datagram on each device. */
	error = fragment_send(&test_devices[0], 900U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "purge: device 0 kept");
	error = fragment_send(&test_devices[1], 901U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "purge: device 1 kept");
	check(device_references == 2, "purge: two references held");

	/* Device 0 goes: its fragment and reference go, device 1's stay. */
	ipv6_fragment_purge_device(&test_devices[0]);
	check(packet_buf_in_use() == 1U, "purge: one left");
	check(device_references == 1, "purge: one reference left");
	error = fragment_send(&test_devices[1], 901U, 600U, 0, 100U, &whole);
	check(whole_check(whole, 700U, IPV6_HEADER_LENGTH), "purge: device 1 completes");
	settle("purge");
}

/* The same key on two devices is two datagrams. */
static void
test_devices_key(void)
{
	struct packet_buf *whole;
	int error;

	/* The first half on device 0, the second on device 1: neither whole. */
	error = fragment_send(&test_devices[0], 950U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "device key: first on 0");
	error = fragment_send(&test_devices[1], 950U, 600U, 0, 100U, &whole);
	check(error == EINPROGRESS, "device key: last on 1 kept apart");

	/* The second half on device 0 too. */
	error = fragment_send(&test_devices[0], 950U, 600U, 0, 100U, &whole);
	check(error == 0, "device key: whole on 0");
	check(whole_check(whole, 700U, IPV6_HEADER_LENGTH), "device key: bytes");
	settle("device key");
}

/* Jumbo fragments: no more are kept than the bound, so the drivers' receive keeps its reserve. */
static void
test_jumbo(void)
{
#if PACKET_BUF_JUMBO_COUNT != 0
	struct packet_buf_pool_usage usage;
	struct packet_buf *whole;
	unsigned identification;
	unsigned piece;
	unsigned most;
	int error;

	/* Datagrams of three 2400-byte pieces with the last missing, a second apart. */
	most = 0U;
	error = 0;
	for (identification = 1000U; identification < 1006U; identification++) {
		for (piece = 0U; piece < 3U; piece++) {
			error = fragment_send(&test_devices[0], identification, 2400U * piece, 1, 2400U, &whole);
			packet_buf_pool_usage(&usage);
			if (usage.jumbo_used > most)
				most = usage.jumbo_used;
		}

	/* A second on, so that the next datagram is the newer. */
		test_now_ms += 1100U;
	}

	/* The newest datagram is kept, the jumbo packets within their bound. */
	check(error == EINPROGRESS, "jumbo: newest kept");
	check(most == TEST_HELD_JUMBO, "jumbo: kept up to the bound and no further");
	check(PACKET_BUF_JUMBO_COUNT - most >= PACKET_BUF_JUMBO_RESERVE, "jumbo: the receive reserve left");
	settle("jumbo");
#endif
}

/* A TCP segment is reassembled only as large as a standard packet holds. */
static void
test_tcp_size(void)
{
	struct packet_buf *packet;
	uint8_t next_header;
	size_t offset;
	size_t field;
	int error;

	/* 1600 bytes: whole. */
	packet = fragment_make(&test_devices[0], 1100U, 0U, 1, TEST_TCP, pattern, 1000U, TEST_PLAIN);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == EINPROGRESS, "tcp: first kept");
	packet = fragment_make(&test_devices[0], 1100U, 1000U, 0, TEST_TCP, pattern + 1000U, 600U, TEST_PLAIN);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == 0 && next_header == TEST_TCP, "tcp: 1600 bytes whole");
	if (error == 0)
		packet_buf_free(packet);
	settle("tcp small");

	/* Past a standard packet: dropped, the datagram dead. */
	packet = fragment_make(&test_devices[0], 1101U, 0U, 1, TEST_TCP, pattern, 1232U, TEST_PLAIN);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == EINPROGRESS, "tcp: large first kept");
	packet = fragment_make(&test_devices[0], 1101U, 1232U, 0, TEST_TCP, pattern + 1232U, 800U, TEST_PLAIN);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == EINVAL, "tcp: past a standard packet dropped");
	check(packet_buf_in_use() == 0U, "tcp: datagram thrown away");
	settle("tcp large");

	/* The same size for UDP is reassembled where a jumbo packet holds it. */
	if (TEST_LARGEST >= IPV6_HEADER_LENGTH + 2032U) {
		packet = fragment_make(&test_devices[0], 1102U, 0U, 1, TEST_UDP, pattern, 1232U, TEST_PLAIN);
		error = fragment_feed(&packet, &next_header, &offset, &field);
		packet = fragment_make(&test_devices[0], 1102U, 1232U, 0, TEST_UDP, pattern + 1232U, 800U, TEST_PLAIN);
		error = fragment_feed(&packet, &next_header, &offset, &field);
		check(error == 0, "tcp: the same size of UDP whole");
		if (error == 0)
			packet_buf_free(packet);
	}

	/* Every record run out, nothing left. */
	settle("tcp udp");
}

/* ECN: a CE piece makes the datagram CE; a Not-ECT piece among ECT ones drops it. */
static void
test_ecn_merge(void)
{
	struct packet_buf *whole;
	const struct ipv6_wire *header;
	int error;

	/* ECT(0), then CE. */
	test_ecn = 2U;
	error = fragment_send(&test_devices[0], 1200U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "ecn: ECT(0) kept");
	test_ecn = 3U;
	error = fragment_send(&test_devices[0], 1200U, 600U, 0, 100U, &whole);
	check(error == 0, "ecn: CE makes it whole");
	if (whole != NULL) {
		header = (const struct ipv6_wire *)whole->data;
		check(((header->version_class_flow[1] >> 4) & 3U) == 3U, "ecn: whole is CE");
		packet_buf_free(whole);
	}

	/* Every record run out, nothing left. */
	settle("ecn ce");

	/* Not-ECT, then ECT(1). */
	test_ecn = 0U;
	error = fragment_send(&test_devices[0], 1201U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "ecn: Not-ECT kept");
	test_ecn = 1U;
	error = fragment_send(&test_devices[0], 1201U, 600U, 0, 100U, &whole);
	check(error == ENOBUFS && whole == NULL, "ecn: Not-ECT with ECT dropped");
	test_ecn = 0U;
	settle("ecn mixed");
}

/* The pieces' unfragmentable parts differ: the offset-zero one's is kept, whichever piece comes last. */
static void
test_mixed_unfragmentable(void)
{
	struct packet_buf *packet;
	struct packet_buf *whole;
	uint8_t next_header;
	size_t offset;
	size_t field;
	int error;

	/* The offset-zero piece without options, then the last with destination options before its fragment header. */
	error = fragment_send(&test_devices[0], 1300U, 0U, 1, 600U, &whole);
	check(error == EINPROGRESS, "mixed: first kept");
	packet = fragment_make(&test_devices[0], 1300U, 600U, 0, TEST_UDP, pattern + 600U, 300U, TEST_DSTOPTS);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == 0, "mixed: whole");
	check(next_header == TEST_UDP && offset == IPV6_HEADER_LENGTH && field == 6U, "mixed: walked on from the offset-zero piece's part");
	if (error == 0)
		check(whole_check(packet, 900U, IPV6_HEADER_LENGTH), "mixed: bytes and length");
	settle("mixed");
}

/* No packet free for the whole datagram: it is lost, its pieces freed. */
static void
test_allocation(void)
{
#if PACKET_BUF_JUMBO_COUNT != 0
	struct packet_buf *taken[PACKET_BUF_JUMBO_COUNT];
	struct packet_buf *whole;
	unsigned count;
	int error;

	/* Every jumbo packet taken. */
	count = 0U;
	while (count < PACKET_BUF_JUMBO_COUNT) {
		taken[count] = packet_buf_alloc_for(0U, 4000U, PACKET_BUF_ALLOC_RECEIVE, &error);
		if (taken[count] == NULL)
			break;
		count++;
	}

	/* A 3000-byte datagram in standard fragments: kept, then lost at the end. */
	error = fragment_send(&test_devices[0], 1400U, 0U, 1, 1232U, &whole);
	check(error == EINPROGRESS, "allocation: first kept");
	error = fragment_send(&test_devices[0], 1400U, 1232U, 1, 1232U, &whole);
	error = fragment_send(&test_devices[0], 1400U, 2464U, 0, 536U, &whole);
	check(error == ENOBUFS && whole == NULL, "allocation: no packet for the whole");
	check(packet_buf_in_use() == count, "allocation: the pieces freed");

	/* The jumbo packets given back. */
	while (count > 0U) {
		count--;
		packet_buf_free(taken[count]);
	}

	/* Every record run out, nothing left. */
	settle("allocation");
#endif
}

/* An ICMPv6 error message behind destination options in a first fragment is not answered either. */
static void
test_quoted_error(void)
{
	struct packet_buf *packet;
	uint8_t data[600];
	uint8_t next_header;
	size_t offset;
	size_t field;
	int error;

	/* Destination options naming ICMPv6, then a Destination Unreachable. */
	memcpy(data, pattern, sizeof(data));
	data[0] = IPV6_NEXT_ICMPV6;
	data[1] = 0U;
	data[2] = 1U;
	data[3] = 4U;
	memset(data + 4, 0, 4U);
	data[8] = 1U;
	packet = fragment_make(&test_devices[0], 1500U, 0U, 1, IPV6_NEXT_DSTOPTS, data, sizeof(data), TEST_PLAIN);
	error = fragment_feed(&packet, &next_header, &offset, &field);
	check(error == EINPROGRESS, "quoted error: kept");
	test_now_ms += 60000U;
	ipv6_fragment_timer_run(test_now_ms);
	check(error_count == 0U, "quoted error: no Time Exceeded about an error message");
	settle("quoted error");
}

/*
 * The kernel's lock-word claim and spin hint, which the kernel core defines
 * with the HAL.  The host's exchange stands in for the port's sequence.
 */
int
atomic_raw_try_acquire_zero(
	volatile unsigned *value)
{
	return __atomic_exchange_n(value, 1U, __ATOMIC_ACQUIRE) == 0U;
}

/* The host has no spin instruction to give. */
void
atomic_spin_hint(
	void)
{
}
