/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the L2CAP move at the phone link's handoff (ws197-p002,
 * plan/ws197/phase002/phase.md sections 7.3 and 12.1), built with the
 * host's compiler under ASan and UBSan: the phone's SDP and RFCOMM channels
 * answered Pending during the pairing move to the phone link's table in
 * their own slots (their local CIDs stay), are found there by CID and are
 * answered from it; a channel whose slot is taken stays and is refused one
 * command at a time (Connection Response "no resources" for a pending one,
 * Disconnection Request for an open one); another connection's channels
 * stay; the request identifiers go on.
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/l2cap.h"

#include <stdio.h>
#include <string.h>

/* The phone's connection, another one, and the PSMs answered Pending. */
#define TEST_HANDLE		0x0041U
#define TEST_OTHER		0x0042U
#define TEST_PSM_SDP		0x0001U
#define TEST_PSM_RFCOMM		0x0003U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

static void check(int condition, const char *what);
static int accept_pending(void *context, uint16_t handle, uint16_t psm, uint16_t *result, uint16_t *status);
static void request(struct btd_l2cap *l2cap, uint16_t handle, uint8_t identifier, uint16_t psm, uint16_t source);
static unsigned le16(const uint8_t *bytes);
static void test_move(void);
static void test_open_left(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_move();
	test_open_left();

	/* The count of what failed. */
	printf("bt-l2cap-move-host-test: %u checks, %u failed\n", checks, failures);
	if (failures != 0U)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* Counts a check and reports the one that failed. */
static void
check(
	int condition,
	const char *what)
{
	/* Counted. */
	checks++;
	if (condition)
		return;

	/* Reported. */
	failures++;
	printf("FAIL: %s\n", what);
}

/* The pairing's accept hook with a phone: SDP and RFCOMM pending, anything else not supported. */
static int
accept_pending(
	void *context,
	uint16_t handle,
	uint16_t psm,
	uint16_t *result,
	uint16_t *status)
{
	/* Pending for the two PSMs. */
	(void)context;
	(void)handle;
	*status = 0U;
	if (psm == TEST_PSM_SDP || psm == TEST_PSM_RFCOMM) {
		*result = BTD_L2CAP_PENDING;
		return 0;
	}

	/* Refused. */
	*result = BTD_L2CAP_PSM_NOT_SUPPORTED;
	return 0;
}

/* Hands a table the other side's Connection Request of a PSM from its CID. */
static void
request(
	struct btd_l2cap *l2cap,
	uint16_t handle,
	uint8_t identifier,
	uint16_t psm,
	uint16_t source)
{
	struct btd_signal_effect effect;
	uint8_t payload[8];
	uint8_t answer[64];
	size_t length;

	/* Code 0x02, the identifier, four bytes: the PSM and the source CID. */
	payload[0] = 0x02U;
	payload[1] = identifier;
	payload[2] = 4U;
	payload[3] = 0U;
	payload[4] = (uint8_t)(psm & 0xffU);
	payload[5] = (uint8_t)(psm >> 8);
	payload[6] = (uint8_t)(source & 0xffU);
	payload[7] = (uint8_t)(source >> 8);
	memset(&effect, 0, sizeof(effect));
	(void)btd_l2cap_signal(l2cap, handle, 0, payload, sizeof(payload), answer, sizeof(answer), &length, &effect);
}

/* Reads a little-endian 16-bit field. */
static unsigned
le16(
	const uint8_t *bytes)
{
	/* The two bytes. */
	return (unsigned)bytes[0] | ((unsigned)bytes[1] << 8);
}

/* Two pending channels, one moved and one left; another connection's stays. */
static void
test_move(void)
{
	static struct btd_l2cap from;
	static struct btd_l2cap to;
	struct btd_channel *channel;
	uint8_t answer[64];
	unsigned moved;
	unsigned left;
	size_t length;
	int error;

	/* The pairing's table: SDP (slot 0) and RFCOMM (slot 1) pending, another connection's SDP (slot 2). */
	btd_l2cap_init(&from);
	btd_l2cap_set_accept(&from, accept_pending, NULL);
	request(&from, TEST_HANDLE, 5U, TEST_PSM_SDP, 0x0070U);
	request(&from, TEST_HANDLE, 6U, TEST_PSM_RFCOMM, 0x0071U);
	request(&from, TEST_OTHER, 7U, TEST_PSM_SDP, 0x0072U);
	check(from.channels[0].state == BTD_CHANNEL_PENDING && from.channels[1].state == BTD_CHANNEL_PENDING, "move: two pending");
	from.next_identifier = 9U;

	/* The phone link's table with slot 1 taken. */
	btd_l2cap_init(&to);
	to.channels[1].state = BTD_CHANNEL_OPEN;
	to.channels[1].handle = TEST_OTHER;

	/* The move: one moved, one left, the other connection's stays. */
	btd_l2cap_move(&from, &to, TEST_HANDLE, &moved, &left);
	check(moved == 1U && left == 1U, "move: one moved, one left");
	check(from.channels[0].state == BTD_CHANNEL_FREE, "move: the moved one freed in the first table");
	check(from.channels[2].state == BTD_CHANNEL_PENDING && from.channels[2].handle == TEST_OTHER, "move: another connection's stays");
	check(to.next_identifier == 9U, "move: the identifiers go on");

	/* Found by its local CID in the new table, with its state and the other side's CID and identifier. */
	channel = btd_l2cap_channel(&to, 0x0040U);
	check(channel != NULL && channel->state == BTD_CHANNEL_PENDING && channel->remote_cid == 0x0070U && channel->identifier == 5U, "move: found by its CID");

	/* Answered from the new table: Connection Response success under their identifier, then our Configure Request. */
	error = btd_l2cap_answer_pending(&to, TEST_HANDLE, BTD_L2CAP_SUCCESS, answer, sizeof(answer), &length);
	check(error == 0 && length >= 12U, "move: answered from the new table");
	check(length >= 12U && answer[0] == 0x03U && answer[1] == 5U && le16(answer + 4) == 0x0040U && le16(answer + 6) == 0x0070U && le16(answer + 8) == 0U, "move: success under their identifier");
	check(length >= 13U && answer[12] == 0x04U, "move: our Configure Request follows");

	/* The left one: Connection Response "no resources" under its identifier, then nothing more. */
	error = btd_l2cap_refuse_left(&from, TEST_HANDLE, answer, sizeof(answer), &length);
	check(error == 0 && length == 12U, "move: one refusal");
	check(length == 12U && answer[0] == 0x03U && answer[1] == 6U && le16(answer + 4) == 0U && le16(answer + 6) == 0x0071U && le16(answer + 8) == BTD_L2CAP_NO_RESOURCES, "move: no resources under their identifier");
	check(from.channels[1].state == BTD_CHANNEL_FREE && !from.channels[1].left, "move: the refused one freed");
	error = btd_l2cap_refuse_left(&from, TEST_HANDLE, answer, sizeof(answer), &length);
	check(error == 0 && length == 0U, "move: nothing more");

	/* A refusal that does not fit keeps the channel. */
	btd_l2cap_init(&from);
	btd_l2cap_set_accept(&from, accept_pending, NULL);
	request(&from, TEST_HANDLE, 8U, TEST_PSM_SDP, 0x0073U);
	from.channels[0].left = 1;
	error = btd_l2cap_refuse_left(&from, TEST_HANDLE, answer, 8U, &length);
	check(error != 0 && from.channels[0].state == BTD_CHANNEL_PENDING, "move: kept when the refusal does not fit");
}

/* An open channel left by the move: a Disconnection Request, ours and theirs. */
static void
test_open_left(void)
{
	static struct btd_l2cap from;
	static struct btd_l2cap to;
	uint8_t answer[64];
	unsigned moved;
	unsigned left;
	size_t length;
	int error;

	/* An open channel in slot 3 of the pairing's table, slot 3 taken in the other. */
	btd_l2cap_init(&from);
	from.channels[3].state = BTD_CHANNEL_OPEN;
	from.channels[3].handle = TEST_HANDLE;
	from.channels[3].local_cid = 0x0043U;
	from.channels[3].remote_cid = 0x0080U;
	from.next_identifier = 20U;
	btd_l2cap_init(&to);
	to.channels[3].state = BTD_CHANNEL_CONFIGURING;
	to.channels[3].handle = TEST_OTHER;

	/* Left, then refused with a Disconnection Request under a new identifier. */
	btd_l2cap_move(&from, &to, TEST_HANDLE, &moved, &left);
	check(moved == 0U && left == 1U, "open: left");
	error = btd_l2cap_refuse_left(&from, TEST_HANDLE, answer, sizeof(answer), &length);
	check(error == 0 && length == 8U && answer[0] == 0x06U && answer[1] == 20U, "open: a Disconnection Request");
	check(length == 8U && le16(answer + 4) == 0x0080U && le16(answer + 6) == 0x0043U, "open: their CID, then ours");
	check(from.channels[3].state == BTD_CHANNEL_FREE, "open: freed");
	check(to.channels[3].state == BTD_CHANNEL_CONFIGURING && to.channels[3].handle == TEST_OTHER, "open: the other table's channel untouched");

	/* A dropped connection clears a mark of a move. */
	from.channels[4].state = BTD_CHANNEL_PENDING;
	from.channels[4].handle = TEST_HANDLE;
	from.channels[4].left = 1;
	btd_l2cap_drop(&from, TEST_HANDLE);
	check(from.channels[4].state == BTD_CHANNEL_FREE && !from.channels[4].left, "open: a drop clears the mark");
}
