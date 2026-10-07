/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the Bluetooth HCI class's pure part (ws143-p002,
 * src/drivers/generic/bt-hci-proto.c): the reassembly of events and ACL
 * packets fed a byte at a time, across transfers and several in one; an
 * ACL packet past the limit and the recovery at the next transfer; the
 * checks of written packets and of Secure Send; the ring's wrap, order,
 * room, the notices' reserve and a full ring.
 */

#include <drivers/generic/bt-hci.h>

#include <stdio.h>
#include <string.h>
#include <uapi/errno.h>

/* The most packets one test gathers. */
#define TEST_PACKETS	16U

/* The checks that failed. */
static unsigned failures;

/* The packets the assembler delivered in one test. */
static uint8_t delivered_types[TEST_PACKETS];
static size_t delivered_lengths[TEST_PACKETS];
static uint8_t delivered_bytes[TEST_PACKETS][8U + BT_ACL_DATA_MAX];
static unsigned delivered_count;

static void expect(int condition, const char *what);
static int keep(void *context, uint8_t type, const uint8_t *packet, size_t length);
static int refuse(void *context, uint8_t type, const uint8_t *packet, size_t length);
static void test_events(void);
static void test_acl(void);
static void test_write(void);
static void test_ring(void);

/*
 * Runs every test; reports how many checks failed.
 */
int
main(void)
{
	/* The reassembly, the write checks and the ring. */
	test_events();
	test_acl();
	test_write();
	test_ring();

	/* A failed check fails the whole. */
	if (failures != 0U) {
		printf("bt-hci-proto: %u check(s) failed\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("bt-hci-proto: all checks passed\n");
	return 0;
}

/* Counts and reports a check that does not hold. */
static void
expect(
	int condition,
	const char *what)
{
	/* A check that holds says nothing. */
	if (condition)
		return;

	/* A failed one is counted and printed. */
	failures++;
	printf("FAIL: %s\n", what);
}

/* Keeps one delivered packet. */
static int
keep(
	void *context,
	uint8_t type,
	const uint8_t *packet,
	size_t length)
{
	(void)context;
	if (delivered_count >= TEST_PACKETS)
		return ENOSPC;
	delivered_types[delivered_count] = type;
	delivered_lengths[delivered_count] = length;
	memcpy(delivered_bytes[delivered_count], packet, length);
	delivered_count++;
	return 0;
}

/* Refuses every packet (the class out of room). */
static int
refuse(
	void *context,
	uint8_t type,
	const uint8_t *packet,
	size_t length)
{
	(void)context;
	(void)type;
	(void)packet;
	(void)length;
	return ENOSPC;
}

/* Events fed a byte at a time, two in one transfer, one across three, and one with no parameters. */
static void
test_events(void)
{
	size_t consumed;
	static const uint8_t complete[] = { 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };
	static const uint8_t two[] = { 0x0f, 0x04, 0x00, 0x01, 0x05, 0x04, 0x13, 0x01, 0x07 };
	static const uint8_t empty[] = { 0x1a, 0x00 };
	uint8_t buffer[BT_HCI_EVENT_HEADER + 255U];
	uint8_t big[2U + 255U];
	struct bt_hci_assembler assembler;
	size_t index;
	int error;

	/* An assembler of events. */
	bt_hci_assembler_init(&assembler, buffer, sizeof(buffer), BT_HCI_ASSEMBLE_EVENT, BT_ACL_DATA_DEFAULT);

	/* A byte at a time. */
	delivered_count = 0U;
	for (index = 0U; index < sizeof(complete); index++) {
		error = bt_hci_assembler_feed(&assembler, complete + index, 1U, keep, NULL, &consumed);
		expect(error == 0, "event byte feed");
	}

	/* One whole event came of it. */
	expect(delivered_count == 1U, "event byte count");
	expect(delivered_types[0] == BT_PACKET_EVENT, "event type");
	expect(delivered_lengths[0] == sizeof(complete), "event length");
	expect(memcmp(delivered_bytes[0], complete, sizeof(complete)) == 0, "event bytes");

	/* Two events in one transfer. */
	delivered_count = 0U;
	error = bt_hci_assembler_feed(&assembler, two, sizeof(two), keep, NULL, &consumed);
	expect(error == 0, "two events feed");
	expect(delivered_count == 2U, "two events count");
	expect(delivered_lengths[0] == 6U && delivered_lengths[1] == 3U, "two events lengths");

	/* The longest event across three transfers of 100, 100 and 57 bytes. */
	big[0] = 0xff;
	big[1] = 0xff;
	for (index = 2U; index < sizeof(big); index++)
		big[index] = (uint8_t)index;
	delivered_count = 0U;
	error = bt_hci_assembler_feed(&assembler, big, 100U, keep, NULL, &consumed);
	expect(error == 0 && delivered_count == 0U, "long event part 1");
	error = bt_hci_assembler_feed(&assembler, big + 100, 100U, keep, NULL, &consumed);
	expect(error == 0 && delivered_count == 0U, "long event part 2");
	error = bt_hci_assembler_feed(&assembler, big + 200, sizeof(big) - 200U, keep, NULL, &consumed);
	expect(error == 0 && delivered_count == 1U, "long event part 3");
	expect(delivered_lengths[0] == sizeof(big), "long event length");
	expect(memcmp(delivered_bytes[0], big, sizeof(big)) == 0, "long event bytes");

	/* An event with no parameters at the end of a transfer. */
	delivered_count = 0U;
	error = bt_hci_assembler_feed(&assembler, empty, sizeof(empty), keep, NULL, &consumed);
	expect(error == 0 && delivered_count == 1U, "empty event at the end");
	expect(delivered_lengths[0] == 2U, "empty event length");

	/* An event with no parameters split after its first byte. */
	delivered_count = 0U;
	(void)bt_hci_assembler_feed(&assembler, empty, 1U, keep, NULL, &consumed);
	error = bt_hci_assembler_feed(&assembler, empty + 1, 1U, keep, NULL, &consumed);
	expect(error == 0 && delivered_count == 1U, "empty event split");

	/* A refused packet stays gathered; the rest is fed later, and the held packet goes first. */
	error = bt_hci_assembler_feed(&assembler, two, sizeof(two), refuse, NULL, &consumed);
	expect(error == ENOSPC, "refused delivery");
	expect(consumed == 6U && assembler.have == 6U, "refused packet held");
	delivered_count = 0U;
	error = bt_hci_assembler_feed(&assembler, NULL, 0U, keep, NULL, &consumed);
	expect(error == 0 && consumed == 0U && delivered_count == 1U && delivered_lengths[0] == 6U, "held packet delivered alone");
	error = bt_hci_assembler_feed(&assembler, two + 6, sizeof(two) - 6U, keep, NULL, &consumed);
	expect(error == 0 && consumed == 3U && delivered_count == 2U && delivered_lengths[1] == 3U, "rest fed later");
}

/* ACL packets across transfers, one past the limit, and the recovery at the next transfer. */
static void
test_acl(void)
{
	size_t consumed;
	static uint8_t buffer[BT_HCI_ACL_HEADER + BT_ACL_DATA_MAX];
	static uint8_t stream[2U * (BT_HCI_ACL_HEADER + 1021U)];
	static const uint8_t too_long[] = { 0x01, 0x20, 0xfe, 0x03, 0xaa, 0xbb };
	static const uint8_t small[] = { 0x01, 0x20, 0x02, 0x00, 0x11, 0x22 };
	struct bt_hci_assembler assembler;
	size_t index;
	size_t offset;
	int error;

	/* An assembler of ACL packets at the default limit. */
	bt_hci_assembler_init(&assembler, buffer, sizeof(buffer), BT_HCI_ASSEMBLE_ACL, BT_ACL_DATA_DEFAULT);

	/* Two packets of the default limit, fed in transfers of 64 bytes. */
	for (index = 0U; index < 2U; index++) {
		offset = index * (BT_HCI_ACL_HEADER + 1021U);
		stream[offset] = (uint8_t)(0x40U + index);
		stream[offset + 1U] = 0x20;
		stream[offset + 2U] = (uint8_t)(1021U & 0xffU);
		stream[offset + 3U] = (uint8_t)(1021U >> 8);
		memset(stream + offset + 4U, (int)(0x30U + index), 1021U);
	}

	/* Fed 64 bytes a transfer. */
	delivered_count = 0U;
	for (offset = 0U; offset < sizeof(stream); offset += 64U) {
		index = sizeof(stream) - offset;
		if (index > 64U)
			index = 64U;
		error = bt_hci_assembler_feed(&assembler, stream + offset, index, keep, NULL, &consumed);
		expect(error == 0, "acl feed");
	}

	/* Both packets came whole. */
	expect(delivered_count == 2U, "acl count");
	expect(delivered_types[0] == BT_PACKET_ACL, "acl type");
	expect(delivered_lengths[0] == BT_HCI_ACL_HEADER + 1021U, "acl length");
	expect(delivered_bytes[1][0] == 0x41 && delivered_bytes[1][4] == 0x31, "second acl bytes");

	/* A header past the limit: refused, nothing gathered. */
	delivered_count = 0U;
	error = bt_hci_assembler_feed(&assembler, too_long, sizeof(too_long), keep, NULL, &consumed);
	expect(error == EBADMSG, "too long acl refused");
	expect(consumed == sizeof(too_long), "too long acl drops the transfer");
	expect(assembler.have == 0U, "too long acl leaves nothing gathered");
	expect(delivered_count == 0U, "too long acl not delivered");

	/* The next transfer starts a new packet. */
	error = bt_hci_assembler_feed(&assembler, small, sizeof(small), keep, NULL, &consumed);
	expect(error == 0 && delivered_count == 1U, "recovery at the next transfer");

	/* A higher limit takes the longest packet. */
	bt_hci_assembler_restart(&assembler, BT_HCI_ASSEMBLE_ACL, BT_ACL_DATA_MAX);
	delivered_count = 0U;
	stream[0] = 0x01;
	stream[1] = 0x20;
	stream[2] = 0x00;
	stream[3] = 0x10;
	error = bt_hci_assembler_feed(&assembler, stream, 4U, keep, NULL, &consumed);
	expect(error == 0, "longest acl header");
	for (index = 0U; index < 4U; index++) {
		error = bt_hci_assembler_feed(&assembler, stream + 4, 1024U, keep, NULL, &consumed);
		expect(error == 0, "longest acl body");
	}

	/* It came whole. */
	expect(delivered_count == 1U && delivered_lengths[0] == 4U + 4096U, "longest acl");

	/* Restarting drops a half-gathered packet. */
	(void)bt_hci_assembler_feed(&assembler, small, 3U, keep, NULL, &consumed);
	bt_hci_assembler_restart(&assembler, BT_HCI_ASSEMBLE_EVENT, BT_ACL_DATA_DEFAULT);
	expect(assembler.have == 0U, "restart drops the gathered bytes");
}

/* The checks of written packets and of Secure Send. */
static void
test_write(void)
{
	static const uint8_t reset[] = { 0x01, 0x03, 0x0c, 0x00 };
	static const uint8_t short_command[] = { 0x01, 0x03, 0x0c };
	static const uint8_t long_command[] = { 0x01, 0x03, 0x0c, 0x00, 0x00 };
	static const uint8_t secure_send[] = { 0x01, 0x09, 0xfc, 0x01, 0x00 };
	static const uint8_t acl[] = { 0x02, 0x01, 0x20, 0x02, 0x00, 0xaa, 0xbb };
	static const uint8_t acl_short[] = { 0x02, 0x01, 0x20, 0x02, 0x00, 0xaa };
	static const uint8_t sco[] = { 0x03, 0x01, 0x00, 0x00 };
	static const uint8_t event[] = { 0x04, 0x0e, 0x00 };
	uint8_t acl_over[1U + 4U + 28U];
	int result;

	/* Commands, ACL packets and the types that are not written. */
	result = bt_hci_check_write(reset, sizeof(reset), BT_ACL_DATA_DEFAULT);
	expect(result == 0, "reset command accepted");
	result = bt_hci_check_write(short_command, sizeof(short_command), BT_ACL_DATA_DEFAULT);
	expect(result == EINVAL, "short command refused");
	result = bt_hci_check_write(long_command, sizeof(long_command), BT_ACL_DATA_DEFAULT);
	expect(result == EINVAL, "long command refused");
	result = bt_hci_check_write(acl, sizeof(acl), BT_ACL_DATA_DEFAULT);
	expect(result == 0, "acl accepted");
	result = bt_hci_check_write(acl_short, sizeof(acl_short), BT_ACL_DATA_DEFAULT);
	expect(result == EINVAL, "short acl refused");
	result = bt_hci_check_write(sco, sizeof(sco), BT_ACL_DATA_DEFAULT);
	expect(result == EINVAL, "sco refused");
	result = bt_hci_check_write(event, sizeof(event), BT_ACL_DATA_DEFAULT);
	expect(result == EINVAL, "event refused");
	result = bt_hci_check_write(reset, 0U, BT_ACL_DATA_DEFAULT);
	expect(result == EINVAL, "empty write refused");

	/* An ACL packet one byte past the limit. */
	memset(acl_over, 0, sizeof(acl_over));
	acl_over[0] = 0x02;
	acl_over[3] = 28U;
	result = bt_hci_check_write(acl_over, sizeof(acl_over), 27U);
	expect(result == EINVAL, "acl past the limit refused");
	result = bt_hci_check_write(acl_over, sizeof(acl_over), 28U);
	expect(result == 0, "acl at the limit accepted");

	/* Secure Send and not. */
	result = bt_hci_is_secure_send(secure_send, sizeof(secure_send));
	expect(result == 1, "secure send recognized");
	result = bt_hci_is_secure_send(reset, sizeof(reset));
	expect(result == 0, "reset is not secure send");
	result = bt_hci_is_secure_send(acl, sizeof(acl));
	expect(result == 0, "acl is not secure send");
}

/* The ring: order, wrap, room, the notices' reserve and a full ring. */
static void
test_ring(void)
{
	uint8_t storage[64];
	uint8_t body[40];
	uint8_t copy[64];
	struct bt_hci_ring ring;
	uint32_t sequence;
	size_t length;
	size_t room;
	uint8_t type;
	unsigned round;
	int error;

	/* A small ring with a reserve of 8 bytes. */
	bt_hci_ring_init(&ring, storage, sizeof(storage), 8U);
	memset(body, 0x5a, sizeof(body));

	/* Room beside the reserve. */
	room = bt_hci_ring_room(&ring);
	expect(room == 56U, "empty ring room");

	/* Empty: nothing to peek. */
	error = bt_hci_ring_peek(&ring, &type, &sequence, &length);
	expect(error == ENOENT, "empty ring peek");

	/* Many rounds of two records, so that they wrap. */
	for (round = 0U; round < 20U; round++) {
		body[0] = (uint8_t)round;
		error = bt_hci_ring_push(&ring, BT_PACKET_EVENT, round * 2U, body, 10U, 0);
		expect(error == 0, "ring push 1");
		error = bt_hci_ring_push(&ring, BT_PACKET_ACL, round * 2U + 1U, body, 20U, 0);
		expect(error == 0, "ring push 2");

		/* The oldest is the first pushed. */
		error = bt_hci_ring_peek(&ring, &type, &sequence, &length);
		expect(error == 0 && type == BT_PACKET_EVENT && sequence == round * 2U && length == 10U, "ring order 1");
		length = bt_hci_ring_copy(&ring, copy);
		expect(length == 11U && copy[0] == BT_PACKET_EVENT && copy[1] == (uint8_t)round, "ring copy 1");
		bt_hci_ring_pop(&ring);

		/* Then the second. */
		error = bt_hci_ring_peek(&ring, &type, &sequence, &length);
		expect(error == 0 && type == BT_PACKET_ACL && sequence == round * 2U + 1U && length == 20U, "ring order 2");
		length = bt_hci_ring_copy(&ring, copy);
		expect(length == 21U && copy[20] == 0x5a, "ring copy 2");

		/* Leave the second in place every other round, so that the head moves around the ring. */
		if ((round & 1U) == 0U)
			bt_hci_ring_pop(&ring);
		else
			bt_hci_ring_clear(&ring);
	}

	/* Nothing is left. */
	expect(ring.count == 0U && ring.used == 0U, "ring empty after the rounds");

	/* A packet that would eat into the reserve is refused; a notice is not. */
	error = bt_hci_ring_push(&ring, BT_PACKET_ACL, 1U, body, 40U, 0);
	expect(error == 0, "ring push 40");
	room = bt_hci_ring_room(&ring);
	expect(room == 9U, "ring room after 47");
	error = bt_hci_ring_push(&ring, BT_PACKET_EVENT, 2U, body, 3U, 0);
	expect(error == ENOSPC, "packet into the reserve refused");
	error = bt_hci_ring_push(&ring, BT_PACKET_NOTICE_RESET, 3U, NULL, 0U, 1);
	expect(error == 0, "notice uses the reserve");
	error = bt_hci_ring_push(&ring, BT_PACKET_NOTICE_RESET, 4U, NULL, 0U, 1);
	expect(error == 0, "second notice fits");
	error = bt_hci_ring_push(&ring, BT_PACKET_NOTICE_RESET, 5U, NULL, 0U, 1);
	expect(error == ENOSPC, "full ring refuses a notice");
	room = bt_hci_ring_room(&ring);
	expect(room == 0U, "full ring has no room");

	/* A body longer than a record can say. */
	error = bt_hci_ring_push(&ring, BT_PACKET_ACL, 6U, body, 0x10000U, 0);
	expect(error == EINVAL, "oversized record refused");
}
