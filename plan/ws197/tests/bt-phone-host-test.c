/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of bluetoothd's phone link parts (ws197-p002,
 * plan/ws197/phase002/phase.md section 12.1), built with the host's
 * compiler under ASan and UBSan.
 *
 *   rfcomm   the FCS against TS 27.010 annex B and the values computed from
 *            its section 5.2.1.6 bit by bit; the peer's frames are written
 *            out here byte by byte from TS 27.010 and RFCOMM 1.2 (the
 *            address with the C/R bit of table 1, the DLCI with the
 *            direction bit of RFCOMM section 5.4), never with rfcomm.c's
 *            own builder, so that a symmetric mistake is not hidden; both
 *            roles of the session, PN, SABM, MSC, credits, DISC, the idle
 *            close, the refusals, a peer past its credits, frames the send
 *            hook had no room for, malformed frames, timers, and a fuzz
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/rfcomm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fuzz's rounds. */
#define TEST_FUZZ_ROUNDS	200000U

/* The frames the test keeps of what the session sent, and the longest. */
#define TEST_SENT_MAX		64U
#define TEST_FRAME_MAX		1100U

/* The L2CAP MTU the test's sessions run on (bluetoothd's own, 672). */
#define TEST_MTU		672U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/*
 * What the owner of the session under test saw: the frames sent (oldest
 * first), whether the send hook refuses for room, the server channels it
 * accepts, and the events with their DLCIs and reasons.  One instance
 * lives for each test.
 */
struct owner {
	unsigned sent_count;
	size_t sent_length[TEST_SENT_MAX];
	uint8_t sent[TEST_SENT_MAX][TEST_FRAME_MAX];
	int full;
	unsigned accepted_channel;
	unsigned opened_count;
	unsigned opened_dlci;
	unsigned data_count;
	size_t data_length;
	uint8_t data[TEST_FRAME_MAX];
	unsigned writable_count;
	unsigned closed_count;
	unsigned closed_dlci;
	int closed_reason;
	unsigned ended_count;
	int ended_reason;
};

static void check(int condition, const char *what);
static int owner_send(void *context, const uint8_t *payload, size_t length);
static int owner_accept(void *context, unsigned server_channel);
static void owner_opened(void *context, unsigned dlci);
static void owner_data(void *context, unsigned dlci, const uint8_t *data, size_t length);
static void owner_writable(void *context, unsigned dlci);
static void owner_closed(void *context, unsigned dlci, int reason);
static void owner_ended(void *context, int reason);
static void owner_init(struct owner *owner, struct btd_rfcomm *rf, int responder);
static size_t peer_frame(uint8_t *out, uint8_t address, uint8_t control, int with_credit, uint8_t credit, const uint8_t *data, size_t length);
static void peer_send(struct btd_rfcomm *rf, uint8_t address, uint8_t control, const uint8_t *data, size_t length, uint64_t now);
static void peer_mux(struct btd_rfcomm *rf, uint8_t address, uint8_t type, const uint8_t *value, size_t length, uint64_t now);
static int sent_is(struct owner *owner, unsigned index, uint8_t address, uint8_t control, const uint8_t *data, size_t length);
static int sent_mux_is(struct owner *owner, unsigned index, uint8_t address, uint8_t type, const uint8_t *value, size_t length);
static void test_fcs(void);
static void test_responder(void);
static void test_initiator(void);
static void test_refusals(void);
static void test_room(void);
static void test_broken(void);
static void test_fuzz(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_fcs();
	test_responder();
	test_initiator();
	test_refusals();
	test_room();
	test_broken();
	test_fuzz();

	/* The count of what failed. */
	printf("bt-phone-host-test: %u checks, %u failed\n", checks, failures);
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

/* Keeps a frame the session sends (or refuses it while the owner says it has no room). */
static int
owner_send(
	void *context,
	const uint8_t *payload,
	size_t length)
{
	struct owner *owner;

	/* No room now. */
	owner = context;
	if (owner->full)
		return ENOBUFS;

	/* Succeeded: kept, when the test has room for it. */
	if (owner->sent_count < TEST_SENT_MAX && length <= TEST_FRAME_MAX) {
		memcpy(owner->sent[owner->sent_count], payload, length);
		owner->sent_length[owner->sent_count] = length;
		owner->sent_count++;
	}

	/* Taken. */
	return 0;
}

/* Accepts the one server channel the test offers. */
static int
owner_accept(
	void *context,
	unsigned server_channel)
{
	struct owner *owner;

	/* The test's channel only. */
	owner = context;
	if (server_channel == owner->accepted_channel)
		return 1;

	/* Any other refused. */
	return 0;
}

/* Records a DLC that opened. */
static void
owner_opened(
	void *context,
	unsigned dlci)
{
	struct owner *owner;

	/* The count and the DLCI. */
	owner = context;
	owner->opened_count++;
	owner->opened_dlci = dlci;
}

/* Records data that came. */
static void
owner_data(
	void *context,
	unsigned dlci,
	const uint8_t *data,
	size_t length)
{
	struct owner *owner;

	/* The last data kept. */
	(void)dlci;
	owner = context;
	owner->data_count++;
	owner->data_length = length;
	if (length <= TEST_FRAME_MAX)
		memcpy(owner->data, data, length);
}

/* Counts a DLC told it may write. */
static void
owner_writable(
	void *context,
	unsigned dlci)
{
	struct owner *owner;

	/* Counted. */
	(void)dlci;
	owner = context;
	owner->writable_count++;
}

/* Records a DLC that closed. */
static void
owner_closed(
	void *context,
	unsigned dlci,
	int reason)
{
	struct owner *owner;

	/* The count, the DLCI and why. */
	owner = context;
	owner->closed_count++;
	owner->closed_dlci = dlci;
	owner->closed_reason = reason;
}

/* Records the session's end. */
static void
owner_ended(
	void *context,
	int reason)
{
	struct owner *owner;

	/* The count and why. */
	owner = context;
	owner->ended_count++;
	owner->ended_reason = reason;
}

/* Empties the owner and gives the session its hooks; a responder offers server channel 16. */
static void
owner_init(
	struct owner *owner,
	struct btd_rfcomm *rf,
	int responder)
{
	struct btd_rfcomm_events events;
	int error;

	/* Nothing seen yet. */
	memset(owner, 0, sizeof(*owner));
	owner->accepted_channel = 16U;

	/* The hooks. */
	events.context = owner;
	events.send = owner_send;
	events.accept = owner_accept;
	events.opened = owner_opened;
	events.data = owner_data;
	events.writable = owner_writable;
	events.closed = owner_closed;
	events.ended = owner_ended;
	btd_rfcomm_init(rf, &events, TEST_MTU);

	/* Server channel 16 listened on (MAP's MNS in bluetoothd). */
	error = btd_rfcomm_listen(rf, 16U);
	check(error == 0, "listen on 16");
	(void)responder;
}

/*
 * Writes a frame as the peer would, from TS 27.010 section 5.2 written out
 * here: address, control, length in one octet (EA 1) or two, a credit
 * octet, the data, and the FCS over the address and control of a UIH, and
 * over the length too of the others.
 */
static size_t
peer_frame(
	uint8_t *out,
	uint8_t address,
	uint8_t control,
	int with_credit,
	uint8_t credit,
	const uint8_t *data,
	size_t length)
{
	size_t used;
	size_t covered;

	/* Address and control. */
	out[0] = address;
	out[1] = control;

	/* The length. */
	if (length <= 127U) {
		out[2] = (uint8_t)((length << 1) | 1U);
		used = 3U;
	} else {
		out[2] = (uint8_t)((length << 1) & 0xfeU);
		out[3] = (uint8_t)(length >> 7);
		used = 4U;
	}

	/* The FCS's coverage starts as the header so far. */
	covered = used;

	/* The credit octet. */
	if (with_credit) {
		out[used] = credit;
		used++;
	}

	/* The data. */
	if (length != 0U)
		memcpy(out + used, data, length);
	used += length;

	/* The FCS (its function is checked against the standard's values first). */
	if ((control & 0xefU) == 0xefU)
		covered = 2U;
	out[used] = btd_rfcomm_fcs(out, covered);
	used++;

	/* Succeeded: the frame's length. */
	return used;
}

/* Hands the session one frame of the peer without a credit octet. */
static void
peer_send(
	struct btd_rfcomm *rf,
	uint8_t address,
	uint8_t control,
	const uint8_t *data,
	size_t length,
	uint64_t now)
{
	uint8_t frame[TEST_FRAME_MAX];
	size_t used;

	/* Written, then taken. */
	used = peer_frame(frame, address, control, 0, 0U, data, length);
	btd_rfcomm_input(rf, frame, used, now);
}

/* Hands the session one multiplexer message of the peer in a UIH on DLCI 0 (the address given carries the UIH's C/R). */
static void
peer_mux(
	struct btd_rfcomm *rf,
	uint8_t address,
	uint8_t type,
	const uint8_t *value,
	size_t length,
	uint64_t now)
{
	uint8_t message[64];

	/* The type, the length octet, the value. */
	message[0] = type;
	message[1] = (uint8_t)((length << 1) | 1U);
	if (length != 0U)
		memcpy(message + 2, value, length);
	peer_send(rf, address, 0xefU, message, length + 2U, now);
}

/* Tells whether the session's frame at an index is the frame written out here. */
static int
sent_is(
	struct owner *owner,
	unsigned index,
	uint8_t address,
	uint8_t control,
	const uint8_t *data,
	size_t length)
{
	uint8_t wanted[TEST_FRAME_MAX];
	size_t used;
	int differs;

	/* A frame there. */
	if (index >= owner->sent_count)
		return 0;

	/* The same bytes. */
	used = peer_frame(wanted, address, control, 0, 0U, data, length);
	if (owner->sent_length[index] != used)
		return 0;
	differs = memcmp(owner->sent[index], wanted, used);
	if (differs != 0)
		return 0;

	/* Succeeded: the same. */
	return 1;
}

/* Tells whether the session's frame at an index is a multiplexer message written out here. */
static int
sent_mux_is(
	struct owner *owner,
	unsigned index,
	uint8_t address,
	uint8_t type,
	const uint8_t *value,
	size_t length)
{
	uint8_t message[64];
	int same;

	/* The message, then the frame around it. */
	message[0] = type;
	message[1] = (uint8_t)((length << 1) | 1U);
	if (length != 0U)
		memcpy(message + 2, value, length);
	same = sent_is(owner, index, address, 0xefU, message, length + 2U);

	/* Succeeded: whether it is the same. */
	return same;
}

/*
 * The FCS: TS 27.010 annex B.3.1 (a SABM on DLCI 1: 07 3f gives 89) and,
 * computed from section 5.2.1.6's division bit by bit for this test, the
 * SABM and the UA of DLCI 0 (03 3f 01 gives 1c, 03 73 01 gives d7).
 */
static void
test_fcs(void)
{
	static const uint8_t annex[] = { 0x07U, 0x3fU };
	static const uint8_t sabm[] = { 0x03U, 0x3fU, 0x01U };
	static const uint8_t ua[] = { 0x03U, 0x73U, 0x01U };
	uint8_t fcs;

	/* Annex B's example. */
	fcs = btd_rfcomm_fcs(annex, sizeof(annex));
	check(fcs == 0x89U, "fcs of annex B");

	/* DLCI 0's SABM and UA. */
	fcs = btd_rfcomm_fcs(sabm, sizeof(sabm));
	check(fcs == 0x1cU, "fcs of SABM on DLCI 0");
	fcs = btd_rfcomm_fcs(ua, sizeof(ua));
	check(fcs == 0xd7U, "fcs of UA on DLCI 0");
}

/*
 * bluetoothd as the responder (the phone opens the session and a DLC to
 * bluetoothd's server channel 16, DLCI 32: the responder's direction bit
 * is 0): SABM and UA on DLCI 0, PN, SABM and UA, the modem status both
 * ways, data and credits both ways, a frame with a two-octet length, the
 * peer's DISC, the idle close, and the end.
 */
static void
test_responder(void)
{
	static struct btd_rfcomm rf;
	static struct owner owner;
	uint8_t pn[8];
	uint8_t pn_answer[8];
	uint8_t msc[2];
	uint8_t data[300];
	uint8_t credit_frame[16];
	size_t length;
	size_t written;
	const struct btd_rfcomm_dlc *dlc;
	unsigned index;
	int error;

	/* A fresh owner and session. */
	owner_init(&owner, &rf, 1);

	/* The peer's SABM on DLCI 0 (initiator, C/R 1): UA 03 73 01 d7 (a response of the responder, C/R 1). */
	peer_send(&rf, 0x03U, 0x3fU, NULL, 0U, 1000U);
	check(owner.sent_count == 1U, "responder: one answer to SABM");
	check(sent_is(&owner, 0U, 0x03U, 0x73U, NULL, 0U), "responder: UA on DLCI 0");
	check(owner.sent[0][3] == 0xd7U, "responder: UA's FCS is d7");
	check(rf.state == BTD_RFCOMM_SESSION_OPEN, "responder: session open");

	/* The peer's PN for DLCI 32: CL 0xf, N1 639, K 7. */
	pn[0] = 32U;
	pn[1] = 0xf0U;
	pn[2] = 0x00U;
	pn[3] = 0x00U;
	pn[4] = 0x7fU;
	pn[5] = 0x02U;
	pn[6] = 0x00U;
	pn[7] = 0x07U;
	peer_mux(&rf, 0x03U, 0x83U, pn, sizeof(pn), 1010U);

	/* The answer: a UIH of the responder (C/R 0, address 01), PN response 81, CL 0xe, N1 639, K 7. */
	memcpy(pn_answer, pn, sizeof(pn_answer));
	pn_answer[1] = 0xe0U;
	pn_answer[7] = 0x07U;
	check(sent_mux_is(&owner, 1U, 0x01U, 0x81U, pn_answer, sizeof(pn_answer)), "responder: PN response");

	/* The peer's SABM on DLCI 32 (address 03 | 32 << 2 = 83): UA, then bluetoothd's MSC command. */
	peer_send(&rf, 0x83U, 0x3fU, NULL, 0U, 1020U);
	check(sent_is(&owner, 2U, 0x83U, 0x73U, NULL, 0U), "responder: UA on DLCI 32");
	msc[0] = 0x83U;
	msc[1] = 0x8dU;
	check(sent_mux_is(&owner, 3U, 0x01U, 0xe3U, msc, sizeof(msc)), "responder: MSC command");
	check(owner.opened_count == 0U, "responder: not open before the MSCs");

	/* The peer's MSC command: answered with its own signals; then its answer to ours: connected. */
	msc[1] = 0x8dU;
	peer_mux(&rf, 0x03U, 0xe3U, msc, sizeof(msc), 1030U);
	check(sent_mux_is(&owner, 4U, 0x01U, 0xe1U, msc, sizeof(msc)), "responder: MSC response");
	check(owner.opened_count == 0U, "responder: not open before the MSC answer");
	peer_mux(&rf, 0x03U, 0xe1U, msc, sizeof(msc), 1040U);
	check(owner.opened_count == 1U && owner.opened_dlci == 32U, "responder: DLC 32 opened");

	/*
	 * Data from the peer: given to the owner, a credit spent; 6 left is half
	 * the share (12 frames of 639 in 8 KB), so 6 go back in a UIH of the
	 * responder on DLCI 32 (81) with P/F, length 0, credit 6.
	 */
	memcpy(data, "hello", 5U);
	index = owner.sent_count;
	peer_send(&rf, 0x83U, 0xefU, data, 5U, 1050U);
	check(owner.data_count == 1U && owner.data_length == 5U, "responder: data came");
	check(memcmp(owner.data, "hello", 5U) == 0, "responder: the data's bytes");
	dlc = btd_rfcomm_dlc(&rf, 32U);
	length = peer_frame(credit_frame, 0x81U, 0xffU, 1, 6U, NULL, 0U);
	check(owner.sent_count == index + 1U, "responder: one credit frame");
	check(owner.sent_length[index] == length && memcmp(owner.sent[index], credit_frame, length) == 0, "responder: credit frame's bytes");
	check(dlc != NULL && dlc->rx_credits == 12U, "responder: share given back");

	/* One more frame: 11 left, more than half, nothing given. */
	index = owner.sent_count;
	peer_send(&rf, 0x83U, 0xefU, data, 1U, 1060U);
	check(owner.sent_count == index && dlc->rx_credits == 11U, "responder: no credits while more than half are out");

	/* A frame of 300 bytes: a two-octet length. */
	memset(data, 0x5aU, sizeof(data));
	peer_send(&rf, 0x83U, 0xefU, data, 300U, 1070U);
	check(owner.data_length == 300U, "responder: two-octet length");

	/* bluetoothd writes: UIH of the responder on DLCI 32 (address 81), the peer's 7 credits. */
	index = owner.sent_count;
	error = btd_rfcomm_write(&rf, 32U, (const uint8_t *)"world", 5U, &written);
	check(error == 0 && written == 5U, "responder: write");
	check(sent_is(&owner, index, 0x81U, 0xefU, (const uint8_t *)"world", 5U), "responder: written frame");
	check(dlc->tx_credits == 6U, "responder: a credit of the peer spent");

	/* Out of credits: the rest waits. */
	memset(data, 0x11U, sizeof(data));
	error = btd_rfcomm_write(&rf, 32U, data, sizeof(data), &written);
	check(error == 0 && written == sizeof(data), "responder: write within credits");
	for (index = 0U; index < 10U; index++)
		(void)btd_rfcomm_write(&rf, 32U, data, 1U, &written);
	check(dlc->tx_credits == 0U, "responder: credits spent");
	error = btd_rfcomm_write(&rf, 32U, data, 1U, &written);
	check(error == 0 && written == 0U, "responder: nothing without credits");

	/* The peer's credits (a UIH of 83 with P/F and credit 5): the writer is told. */
	owner.writable_count = 0U;
	length = peer_frame(credit_frame, 0x83U, 0xffU, 1, 5U, NULL, 0U);
	btd_rfcomm_input(&rf, credit_frame, length, 1080U);
	check(dlc->tx_credits == 5U, "responder: credits came");
	check(owner.writable_count == 1U, "responder: writer told");

	/* The peer's PN on the open DLC: the present values back, CL 0xe for a request of 0xf, no credits. */
	index = owner.sent_count;
	peer_mux(&rf, 0x03U, 0x83U, pn, sizeof(pn), 1090U);
	pn_answer[7] = 0x00U;
	check(sent_mux_is(&owner, index, 0x01U, 0x81U, pn_answer, sizeof(pn_answer)), "responder: PN on an open DLC");
	check(dlc->state == BTD_RFCOMM_DLC_CONNECTED, "responder: still connected");

	/* The peer's DISC on DLCI 32: UA and closed by the peer. */
	index = owner.sent_count;
	peer_send(&rf, 0x83U, 0x53U, NULL, 0U, 2000U);
	check(sent_is(&owner, index, 0x83U, 0x73U, NULL, 0U), "responder: UA to DISC");
	check(owner.closed_count == 1U && owner.closed_dlci == 32U && owner.closed_reason == BTD_RFCOMM_CLOSED_REMOTE, "responder: closed by the peer");

	/* Idle for 2 s: bluetoothd's DISC on DLCI 0 (a command of the responder, C/R 0, address 01). */
	check(btd_rfcomm_deadline(&rf) == 2000U + BTD_RFCOMM_IDLE_MS, "responder: idle deadline");
	index = owner.sent_count;
	btd_rfcomm_tick(&rf, 3999U);
	check(owner.sent_count == index, "responder: not before the idle time");
	btd_rfcomm_tick(&rf, 4000U);
	check(sent_is(&owner, index, 0x01U, 0x53U, NULL, 0U), "responder: DISC on DLCI 0");
	check(rf.state == BTD_RFCOMM_SESSION_CLOSING, "responder: closing");

	/* The peer's UA (a response of the initiator, C/R 0): ended. */
	peer_send(&rf, 0x01U, 0x73U, NULL, 0U, 4010U);
	check(owner.ended_count == 1U && owner.ended_reason == BTD_RFCOMM_CLOSED_LOCAL, "responder: ended");
	check(rf.state == BTD_RFCOMM_SESSION_CLOSED, "responder: closed");
}

/*
 * bluetoothd as the initiator (it opens the session and a DLC to the
 * phone's server channel 5, DLCI 10: the inverse of its direction bit 1);
 * then the phone opens bluetoothd's server channel 16 (DLCI 33) without a
 * PN, which the session takes now that credits were agreed.
 */
static void
test_initiator(void)
{
	static struct btd_rfcomm rf;
	static struct owner owner;
	uint8_t pn[8];
	uint8_t msc[2];
	uint8_t credit_frame[16];
	size_t length;
	unsigned dlci;
	const struct btd_rfcomm_dlc *dlc;
	unsigned index;
	int error;

	/* A fresh owner and session. */
	owner_init(&owner, &rf, 0);

	/* SABM on DLCI 0: 03 3f 01 1c. */
	error = btd_rfcomm_start(&rf, 1000U);
	check(error == 0, "initiator: start");
	check(sent_is(&owner, 0U, 0x03U, 0x3fU, NULL, 0U), "initiator: SABM on DLCI 0");
	check(owner.sent[0][3] == 0x1cU, "initiator: SABM's FCS is 1c");

	/* A DLC asked for before the UA: it waits. */
	error = btd_rfcomm_connect(&rf, 5U, 1001U, &dlci);
	check(error == 0 && dlci == 10U, "initiator: DLCI 10 for channel 5");
	check(owner.sent_count == 1U, "initiator: nothing before the UA");

	/* The peer's UA (a response of the responder, C/R 1): the PN (UIH of the initiator, address 03, command 83, N1 667, K 7). */
	peer_send(&rf, 0x03U, 0x73U, NULL, 0U, 1010U);
	check(rf.state == BTD_RFCOMM_SESSION_OPEN, "initiator: open");
	pn[0] = 10U;
	pn[1] = 0xf0U;
	pn[2] = 0x00U;
	pn[3] = 0x00U;
	pn[4] = (uint8_t)(667U & 0xffU);
	pn[5] = (uint8_t)(667U >> 8);
	pn[6] = 0x00U;
	pn[7] = 0x07U;
	check(sent_mux_is(&owner, 1U, 0x03U, 0x83U, pn, sizeof(pn)), "initiator: PN command");

	/* The peer's PN response (UIH of the responder, address 01): N1 256, K 3; then SABM on DLCI 10 (address 03 | 10 << 2 = 2b). */
	pn[1] = 0xe0U;
	pn[4] = 0x00U;
	pn[5] = 0x01U;
	pn[7] = 0x03U;
	peer_mux(&rf, 0x01U, 0x81U, pn, sizeof(pn), 1020U);
	check(sent_is(&owner, 2U, 0x2bU, 0x3fU, NULL, 0U), "initiator: SABM on DLCI 10");
	dlc = btd_rfcomm_dlc(&rf, 10U);
	check(dlc != NULL && dlc->n1 == 256U && dlc->tx_credits == 3U, "initiator: the PN's values");

	/* The peer's UA (C/R 1 from the responder): bluetoothd's MSC command (UIH of the initiator, address 03). */
	peer_send(&rf, 0x2bU, 0x73U, NULL, 0U, 1030U);
	msc[0] = 0x2bU;
	msc[1] = 0x8dU;
	check(sent_mux_is(&owner, 3U, 0x03U, 0xe3U, msc, sizeof(msc)), "initiator: MSC command");

	/* The MSCs both ways: opened. */
	peer_mux(&rf, 0x01U, 0xe3U, msc, sizeof(msc), 1040U);
	check(sent_mux_is(&owner, 4U, 0x03U, 0xe1U, msc, sizeof(msc)), "initiator: MSC response");
	peer_mux(&rf, 0x01U, 0xe1U, msc, sizeof(msc), 1050U);
	check(owner.opened_count == 1U && owner.opened_dlci == 10U, "initiator: DLC 10 opened");

	/* The phone opens server channel 16 without a PN: DLCI 33 (16 << 1 | 1, the initiator's servers), address 01 | 33 << 2 = 85. */
	index = owner.sent_count;
	peer_send(&rf, 0x85U, 0x3fU, NULL, 0U, 1060U);
	check(sent_is(&owner, index, 0x85U, 0x73U, NULL, 0U), "initiator: UA on DLCI 33 (C/R 0 of the initiator's response)");
	msc[0] = 0x87U;
	check(sent_mux_is(&owner, index + 1U, 0x03U, 0xe3U, msc, sizeof(msc)), "initiator: MSC for DLCI 33");
	dlc = btd_rfcomm_dlc(&rf, 33U);
	check(dlc != NULL && dlc->n1 == BTD_RFCOMM_N1_DEFAULT && dlc->rx_credits == 0U, "initiator: DLC without PN, default size, no credits");

	/* The MSCs: connected, and credits given at once (a UIH of the initiator on 33, address 87, P/F, credit 32: 8192 / 127 / 2 DLCs). */
	peer_mux(&rf, 0x01U, 0xe3U, msc, sizeof(msc), 1070U);
	index = owner.sent_count;
	peer_mux(&rf, 0x01U, 0xe1U, msc, sizeof(msc), 1080U);
	check(owner.opened_count == 2U && owner.opened_dlci == 33U, "initiator: DLC 33 opened");
	length = peer_frame(credit_frame, 0x87U, 0xffU, 1, 32U, NULL, 0U);
	check(owner.sent_count == index + 1U && owner.sent_length[index] == length, "initiator: credit frame for DLC 33");
	check(memcmp(owner.sent[index], credit_frame, length) == 0, "initiator: credit frame's bytes");

	/* bluetoothd closes DLC 10: DISC (command of the initiator, C/R 1, address 2b); the peer's UA: closed as asked. */
	index = owner.sent_count;
	error = btd_rfcomm_close(&rf, 10U, 2000U);
	check(error == 0 && sent_is(&owner, index, 0x2bU, 0x53U, NULL, 0U), "initiator: DISC on DLCI 10");
	peer_send(&rf, 0x2bU, 0x73U, NULL, 0U, 2010U);
	check(owner.closed_count == 1U && owner.closed_dlci == 10U && owner.closed_reason == BTD_RFCOMM_CLOSED_LOCAL, "initiator: closed as asked");

	/* The peer closes the multiplexer: UA on DLCI 0 (response of the initiator, C/R 0), DLC 33 closed, ended. */
	index = owner.sent_count;
	peer_send(&rf, 0x01U, 0x53U, NULL, 0U, 3000U);
	check(sent_is(&owner, index, 0x01U, 0x73U, NULL, 0U), "initiator: UA to DISC on DLCI 0");
	check(owner.closed_count == 2U && owner.closed_dlci == 33U, "initiator: DLC 33 closed with the session");
	check(owner.ended_count == 1U && owner.ended_reason == BTD_RFCOMM_CLOSED_REMOTE, "initiator: ended by the peer");
}

/*
 * The refusals: a first DLC without a PN (no credits agreed), a channel
 * not offered (SABM and PN), a PN that does not ask for credits, a PN
 * response of a peer without credits, a SABM of DLCI 0 refused, and a
 * peer sending past its credits.
 */
static void
test_refusals(void)
{
	static struct btd_rfcomm rf;
	static struct owner owner;
	uint8_t pn[8];
	uint8_t data[4];
	const struct btd_rfcomm_dlc *dlc;
	unsigned given;
	unsigned dlci;
	unsigned index;
	int error;

	/* A responder session, then a SABM on DLCI 32 without a PN: DM (C/R 1 of the responder's response, address 83, 1f). */
	owner_init(&owner, &rf, 1);
	peer_send(&rf, 0x03U, 0x3fU, NULL, 0U, 1000U);
	peer_send(&rf, 0x83U, 0x3fU, NULL, 0U, 1001U);
	check(sent_is(&owner, 1U, 0x83U, 0x1fU, NULL, 0U), "refusals: first DLC without PN refused");

	/* A server channel not offered (17, DLCI 34): its PN and its SABM get DM. */
	pn[0] = 34U;
	pn[1] = 0xf0U;
	pn[2] = 0x00U;
	pn[3] = 0x00U;
	pn[4] = 0x7fU;
	pn[5] = 0x00U;
	pn[6] = 0x00U;
	pn[7] = 0x07U;
	peer_mux(&rf, 0x03U, 0x83U, pn, sizeof(pn), 1002U);
	check(sent_is(&owner, 2U, 0x8bU, 0x1fU, NULL, 0U), "refusals: PN for a channel not offered");
	peer_send(&rf, 0x8bU, 0x3fU, NULL, 0U, 1003U);
	check(sent_is(&owner, 3U, 0x8bU, 0x1fU, NULL, 0U), "refusals: SABM for a channel not offered");

	/* A DLCI of the wrong direction (33: an initiator's server, not this responder's): DM. */
	peer_send(&rf, 0x87U, 0x3fU, NULL, 0U, 1004U);
	check(sent_is(&owner, 4U, 0x87U, 0x1fU, NULL, 0U), "refusals: DLCI of the wrong direction");

	/* A PN without credits (CL 0) for an offered channel: DM. */
	pn[0] = 32U;
	pn[1] = 0x00U;
	peer_mux(&rf, 0x03U, 0x83U, pn, sizeof(pn), 1005U);
	check(sent_is(&owner, 5U, 0x83U, 0x1fU, NULL, 0U), "refusals: PN without credits");

	/* A peer past its credits: the DLC opened (its credits topped up to the share), then one frame more than given ends the session. */
	pn[1] = 0xf0U;
	peer_mux(&rf, 0x03U, 0x83U, pn, sizeof(pn), 1006U);
	peer_send(&rf, 0x83U, 0x3fU, NULL, 0U, 1007U);
	pn[0] = 0x83U;
	pn[1] = 0x8dU;
	peer_mux(&rf, 0x03U, 0xe3U, pn, 2U, 1008U);
	peer_mux(&rf, 0x03U, 0xe1U, pn, 2U, 1009U);
	check(owner.opened_count == 1U, "refusals: DLC open");
	memset(data, 0x22U, sizeof(data));
	owner.full = 1;
	dlc = btd_rfcomm_dlc(&rf, 32U);
	given = 0U;
	if (dlc != NULL)
		given = dlc->rx_credits;
	check(given == 64U, "refusals: the share of 127-byte frames given");
	for (index = 0U; index < given; index++)
		peer_send(&rf, 0x83U, 0xefU, data, sizeof(data), 1010U);
	check(owner.ended_count == 0U, "refusals: within the credits");
	peer_send(&rf, 0x83U, 0xefU, data, sizeof(data), 1011U);
	check(owner.ended_count == 1U && owner.ended_reason == BTD_RFCOMM_CLOSED_ERROR, "refusals: past the credits ends the session");
	check(owner.closed_count == 1U, "refusals: the DLC told");
	owner.full = 0;

	/* An initiator whose peer answers the PN without credits: DM and the DLC refused. */
	owner_init(&owner, &rf, 0);
	(void)btd_rfcomm_start(&rf, 1000U);
	peer_send(&rf, 0x03U, 0x73U, NULL, 0U, 1001U);
	error = btd_rfcomm_connect(&rf, 5U, 1002U, &dlci);
	check(error == 0, "refusals: connect");
	pn[0] = 10U;
	pn[1] = 0x00U;
	pn[2] = 0x00U;
	pn[3] = 0x00U;
	pn[4] = 0x7fU;
	pn[5] = 0x00U;
	pn[6] = 0x00U;
	pn[7] = 0x00U;
	index = owner.sent_count;
	peer_mux(&rf, 0x01U, 0x81U, pn, sizeof(pn), 1003U);
	check(sent_is(&owner, index, 0x29U, 0x1fU, NULL, 0U), "refusals: DM to a PN response without credits (a response of the initiator, C/R 0)");
	check(owner.closed_count == 1U && owner.closed_reason == BTD_RFCOMM_CLOSED_REFUSED, "refusals: DLC refused");

	/* An initiator whose SABM on DLCI 0 is answered with DM: ended, refused. */
	owner_init(&owner, &rf, 0);
	(void)btd_rfcomm_start(&rf, 1000U);
	peer_send(&rf, 0x03U, 0x1fU, NULL, 0U, 1001U);
	check(owner.ended_count == 1U && owner.ended_reason == BTD_RFCOMM_CLOSED_REFUSED, "refusals: session refused");
}

/*
 * The send hook without room: the UA and the MSC wait in order, data does
 * not pass them, and a credit frame refused is sent by the next pump.
 */
static void
test_room(void)
{
	static struct btd_rfcomm rf;
	static struct owner owner;
	uint8_t pn[8];
	uint8_t msc[2];
	uint8_t data[4];
	size_t written;
	unsigned index;
	const struct btd_rfcomm_dlc *dlc;
	int error;

	/* A responder session open, the DLC negotiated. */
	owner_init(&owner, &rf, 1);
	peer_send(&rf, 0x03U, 0x3fU, NULL, 0U, 1000U);
	pn[0] = 32U;
	pn[1] = 0xf0U;
	pn[2] = 0x00U;
	pn[3] = 0x00U;
	pn[4] = 0x7fU;
	pn[5] = 0x02U;
	pn[6] = 0x00U;
	pn[7] = 0x07U;
	peer_mux(&rf, 0x03U, 0x83U, pn, sizeof(pn), 1001U);

	/* No room: the SABM's UA and the MSC wait. */
	owner.full = 1;
	index = owner.sent_count;
	peer_send(&rf, 0x83U, 0x3fU, NULL, 0U, 1002U);
	check(owner.sent_count == index && rf.pending_count == 2U, "room: UA and MSC wait");

	/* Room again: both go in their order. */
	owner.full = 0;
	btd_rfcomm_pump(&rf);
	check(sent_is(&owner, index, 0x83U, 0x73U, NULL, 0U), "room: UA first");
	msc[0] = 0x83U;
	msc[1] = 0x8dU;
	check(sent_mux_is(&owner, index + 1U, 0x01U, 0xe3U, msc, sizeof(msc)), "room: MSC second");

	/* Connected. */
	peer_mux(&rf, 0x03U, 0xe3U, msc, sizeof(msc), 1003U);
	peer_mux(&rf, 0x03U, 0xe1U, msc, sizeof(msc), 1004U);
	check(owner.opened_count == 1U, "room: connected");

	/* Data written while a control frame waits does not pass it. */
	owner.full = 1;
	pn[0] = 0x03U;
	peer_mux(&rf, 0x03U, 0xa3U, NULL, 0U, 1005U);
	check(rf.pending_count == 1U, "room: FCon's answer waits");
	owner.full = 0;
	memset(data, 0x33U, sizeof(data));
	index = owner.sent_count;
	error = btd_rfcomm_write(&rf, 32U, data, sizeof(data), &written);
	check(error == 0 && written == sizeof(data), "room: write after the waiting answer");
	check(sent_mux_is(&owner, index, 0x01U, 0xa1U, NULL, 0U), "room: FCon's answer went first");
	check(sent_is(&owner, index + 1U, 0x81U, 0xefU, data, sizeof(data)), "room: then the data");

	/* A credit frame refused for room is not lost: the next pump gives the same credits. */
	dlc = btd_rfcomm_dlc(&rf, 32U);
	owner.full = 1;
	for (index = 0U; index < 4U; index++)
		peer_send(&rf, 0x83U, 0xefU, data, 1U, 1006U);
	check(dlc->rx_credits == 3U, "room: credits spent, none given");
	owner.full = 0;
	index = owner.sent_count;
	btd_rfcomm_pump(&rf);
	check(owner.sent_count == index + 1U && dlc->rx_credits == 12U, "room: credits given by the pump");
}

/*
 * Broken frames: a wrong FCS, a length past the frame, a frame larger than
 * N1, four malformed in a row end the session; an unanswered SABM on DLCI
 * 0 ends it on T1; an unknown multiplexer command gets NSC.
 */
static void
test_broken(void)
{
	static struct btd_rfcomm rf;
	static struct owner owner;
	uint8_t frame[TEST_FRAME_MAX];
	uint8_t value[1];
	size_t length;
	unsigned index;

	/* An open responder session. */
	owner_init(&owner, &rf, 1);
	peer_send(&rf, 0x03U, 0x3fU, NULL, 0U, 1000U);

	/* An unknown command (type 0x47: PSC, not supported): NSC with its type. */
	index = owner.sent_count;
	peer_mux(&rf, 0x03U, 0x43U, NULL, 0U, 1001U);
	value[0] = 0x43U;
	check(sent_mux_is(&owner, index, 0x01U, 0x11U, value, sizeof(value)), "broken: NSC for PSC");

	/* A wrong FCS, a length past the frame, an address without EA: passed over, counted. */
	length = peer_frame(frame, 0x03U, 0x3fU, 0, 0U, NULL, 0U);
	frame[length - 1U] ^= 0x01U;
	btd_rfcomm_input(&rf, frame, length, 1002U);
	check(rf.malformed == 1U && owner.ended_count == 0U, "broken: wrong FCS");
	length = peer_frame(frame, 0x03U, 0xefU, 0, 0U, (const uint8_t *)"ab", 2U);
	btd_rfcomm_input(&rf, frame, length - 1U, 1003U);
	check(rf.malformed == 2U, "broken: short frame");
	frame[0] = 0x02U;
	btd_rfcomm_input(&rf, frame, length, 1004U);
	check(rf.malformed == 3U, "broken: address without EA");

	/* A frame on DLCI 0 longer than the default size, the fourth in a row: the session ends. */
	memset(frame, 0, sizeof(frame));
	length = peer_frame(frame, 0x03U, 0xefU, 0, 0U, frame + 512, 200U);
	btd_rfcomm_input(&rf, frame, length, 1005U);
	check(owner.ended_count == 1U && owner.ended_reason == BTD_RFCOMM_CLOSED_ERROR, "broken: malformed run ends the session");

	/* An initiator never answered: T1 ends the session. */
	owner_init(&owner, &rf, 0);
	(void)btd_rfcomm_start(&rf, 1000U);
	check(btd_rfcomm_deadline(&rf) == 1000U + BTD_RFCOMM_T1_MS, "broken: T1 deadline");
	btd_rfcomm_tick(&rf, 1000U + BTD_RFCOMM_T1_MS);
	check(owner.ended_count == 1U && owner.ended_reason == BTD_RFCOMM_CLOSED_TIMEOUT, "broken: T1 ends the session");
}

/*
 * Random frames into sessions of both roles (a fixed seed): nothing
 * crashes under ASan and UBSan, and a DLC never holds more credits than a
 * frame octet carries.
 */
static void
test_fuzz(void)
{
	static struct btd_rfcomm rf;
	static struct owner owner;
	uint8_t frame[TEST_FRAME_MAX];
	unsigned round;
	unsigned seed;
	size_t length;
	size_t index;
	size_t covered;
	unsigned slot;
	int sane;

	/* A fixed seed. */
	seed = 0x197c0de0U;
	sane = 1;
	for (round = 0U; round < TEST_FUZZ_ROUNDS; round++) {
		/* A new session every 1000 frames, of either role, open. */
		if (round % 1000U == 0U) {
			owner_init(&owner, &rf, (int)(round / 1000U) & 1);
			if (((round / 1000U) & 1U) != 0U) {
				peer_send(&rf, 0x03U, 0x3fU, NULL, 0U, round);
			} else {
				(void)btd_rfcomm_start(&rf, round);
				peer_send(&rf, 0x03U, 0x73U, NULL, 0U, round);
			}
		}

		/* A frame: random bytes, often made well formed with a valid FCS. */
		seed = seed * 1103515245U + 12345U;
		length = (size_t)((seed >> 16) % 40U) + 1U;
		for (index = 0U; index < length; index++) {
			seed = seed * 1103515245U + 12345U;
			frame[index] = (uint8_t)(seed >> 16);
		}

		/* Often made well formed: the address's EA, a length that fits, a valid FCS. */
		if ((seed & 0x300U) != 0U && length >= 4U) {
			frame[0] |= 0x01U;
			frame[2] = (uint8_t)(((length - 4U) << 1) | 1U);
			covered = 3U;
			if ((frame[1] & 0xefU) == 0xefU)
				covered = 2U;
			frame[length - 1U] = btd_rfcomm_fcs(frame, covered);
		}

		/* Taken, then the waiting frames and the timers. */
		btd_rfcomm_input(&rf, frame, length, round);
		btd_rfcomm_pump(&rf);
		btd_rfcomm_tick(&rf, round);

		/* The credits within an octet. */
		for (slot = 0U; slot < BTD_RFCOMM_DLCS_MAX; slot++) {
			if (rf.dlcs[slot].tx_credits > 255U || rf.dlcs[slot].rx_credits > 255U)
				sane = 0;
		}
	}

	/* No credit count ever left its octet. */
	check(sane, "fuzz: credits within an octet");
}
