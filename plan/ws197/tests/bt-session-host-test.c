/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the session's receiving and sending changed for the
 * phone link (ws197-p002, plan/ws197/phase002/phase.md sections 3, 4 and
 * 12.1), built with the host's compiler under ASan and UBSan.  The
 * controller is a socket pair: each part writes its packets into a script,
 * and a thread writes the script to the controller's end while the session
 * waits for a command (a flood larger than the socket's buffer goes in as
 * the session reads it, queueing what is not the command's answer).
 *
 *   receiving  (a) a flood of one link's ACL data during a command: the
 *              counted events, the events and an earlier link's data
 *              arrive, the flooded link alone is sealed, the scans'
 *              reports are dropped without a notice, the notice comes once
 *              after everything queued before the drop; (b) a second
 *              command inside the handler while the queue drains: the
 *              sealed link's data in it is dropped too; (c) a continuing
 *              packet after the notice is passed over until a first one;
 *              (d) pending says yes while a notice is due and the queue
 *              is empty; (e) the last
 *              channel follows the order of arrival, not of handling; (f)
 *              a hardware error with the queue full is handled; (g) a
 *              connection event that does not fit is noticed as counted
 *   sending    a link limited to 8 waiting frames and 1 packet in the
 *              controller: its frame past those refused, another link's
 *              frames still taken and sent past it; the room left; the
 *              limits reset with a new connection of the handle
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/acl.h"
#include "userland/base/bluetoothd/hci.h"
#include "userland/base/bluetoothd/session.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The connections: the flooded one (a phone's), the keyboard's, and a third that ends. */
#define TEST_PHONE		0x0040U
#define TEST_KEYBOARD		0x0041U
#define TEST_THIRD		0x0042U

/* The channels of the test's frames. */
#define TEST_CID_DATA		0x0040U
#define TEST_CID_SIGNALLING	0x0001U

/* What the handler keeps of what it is handed. */
#define TEST_HANDED_MAX		512U

/* The bytes of a part's script of packets (each a 2-byte length and the packet). */
#define TEST_SCRIPT_BYTES	(256U * 1024U)

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/*
 * The run: the controller's end of the socket pair, the script of packets
 * the writer thread writes to it (and whether the thread runs), what the
 * handler was handed in order (the packet's type, the handle, the first
 * byte after the ACL header, a notice's flags, channel and count), and a
 * command to run inside the handler when the keyboard's data comes.
 */
struct run {
	int controller;
	struct btd_session *session;
	uint8_t *script;
	size_t script_used;
	pthread_t writer;
	int writing;
	unsigned count;
	uint8_t types[TEST_HANDED_MAX];
	uint16_t handles[TEST_HANDED_MAX];
	uint8_t marks[TEST_HANDED_MAX];
	uint8_t notice_flags[TEST_HANDED_MAX];
	uint16_t notice_cid[TEST_HANDED_MAX];
	int command_inside;
	int inside_error;
};

static void check(int condition, const char *what);
static void handler(void *context, struct btd_session *session, const uint8_t *packet, size_t length);
static void open_run(struct run *run, struct btd_session *session, unsigned pool);
static void close_run(struct run *run);
static void put(struct run *run, const uint8_t *packet, size_t length);
static void start_writer(struct run *run);
static void wait_writer(struct run *run);
static void *writer_run(void *argument);
static void put_acl(struct run *run, uint16_t handle, int continuing, uint16_t cid, uint8_t mark, size_t length);
static void put_connected(struct run *run, uint16_t handle);
static void put_disconnected(struct run *run, uint16_t handle);
static void put_encryption(struct run *run, uint16_t handle);
static void put_report(struct run *run);
static void put_answer(struct run *run, uint16_t opcode);
static void drain(struct run *run);
static void drain_controller(struct run *run);
static unsigned find_notice(const struct run *run, uint16_t handle);
static void test_flood(void);
static void test_inside(void);
static void test_arrival(void);
static void test_counted(void);
static void test_sending(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_flood();
	test_inside();
	test_arrival();
	test_counted();
	test_sending();

	/* The count of what failed. */
	printf("bt-session-host-test: %u checks, %u failed\n", checks, failures);
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

/* Keeps what the session hands, and runs the command asked for when the keyboard's data comes. */
static void
handler(
	void *context,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	struct run *run;
	unsigned index;

	/* Room in the record. */
	run = context;
	if (run->count >= TEST_HANDED_MAX || length < 1U)
		return;
	index = run->count;
	run->count++;
	run->types[index] = packet[0];
	run->handles[index] = 0U;
	run->marks[index] = 0U;

	/* ACL data: its handle and the mark after its header (the first L2CAP byte for a continuing one). */
	if (packet[0] == BT_PACKET_ACL && length >= 6U) {
		run->handles[index] = (uint16_t)((packet[1] | (packet[2] << 8)) & 0x0fffU);
		run->marks[index] = packet[length - 1U];
	}

	/* A notice: its handle, flags and channel. */
	if (packet[0] == BTD_PACKET_DROP && length == BTD_DROP_LENGTH) {
		run->handles[index] = (uint16_t)(packet[1] | (packet[2] << 8));
		run->notice_flags[index] = packet[3];
		run->notice_cid[index] = (uint16_t)(packet[4] | (packet[5] << 8));
	}

	/* The command asked for inside, once, at the keyboard's data. */
	if (run->command_inside && packet[0] == BT_PACKET_ACL && run->handles[index] == TEST_KEYBOARD) {
		run->command_inside = 0;
		run->inside_error = btd_session_command(session, 0xfc02U, NULL, 0U);
	}
}

/* Opens a session on a socket pair, ready, with a BR/EDR pool of a size and the two connections made. */
static void
open_run(
	struct run *run,
	struct btd_session *session,
	unsigned pool)
{
	int ends[2];
	int status;

	/* A socket pair that keeps packets apart, as the node does; the writer blocks while the session's end is full. */
	status = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ends);
	if (status != 0) {
		perror("socketpair");
		exit(2);
	}

	/* The run, with room for its script. */
	memset(run, 0, sizeof(*run));
	run->controller = ends[1];
	run->session = session;
	run->script = malloc(TEST_SCRIPT_BYTES);
	if (run->script == NULL) {
		perror("malloc");
		exit(2);
	}

	/* The session, ready, with its pools and the run as its handler. */
	btd_session_init(session, ends[0], NULL, NULL, "fake", "/nonexistent");
	session->timing.command_ms = 300U;
	session->state = BTD_STATE_READY;
	session->acl_pool.length = 1021U;
	session->acl_pool.total = pool;
	session->acl_pool.free = pool;
	session->le_shared = 1;
	session->handler = handler;
	session->handler_context = run;

	/* The two connections. */
	put_connected(run, TEST_PHONE);
	put_connected(run, TEST_KEYBOARD);
	drain(run);
	run->count = 0U;
}

/* Closes both ends once the writer is done, and frees the script. */
static void
close_run(
	struct run *run)
{
	/* The writer first: it writes to the controller's end. */
	wait_writer(run);

	/* Both ends, and the script. */
	(void)close(run->session->descriptor);
	(void)close(run->controller);
	free(run->script);
	run->script = NULL;
}

/* Adds one packet the controller sends to the script. */
static void
put(
	struct run *run,
	const uint8_t *packet,
	size_t length)
{
	/* Room for its length and itself; the parts are written to fit. */
	if (run->writing || length > 0xffffU || run->script_used + 2U + length > TEST_SCRIPT_BYTES) {
		fprintf(stderr, "bt-session-host-test: the script is full or being written\n");
		exit(2);
	}

	/* Its length, least significant first, then the packet. */
	run->script[run->script_used] = (uint8_t)(length & 0xffU);
	run->script[run->script_used + 1U] = (uint8_t)(length >> 8);
	memcpy(run->script + run->script_used + 2U, packet, length);
	run->script_used += 2U + length;
}

/* Starts the thread that writes the script to the controller's end. */
static void
start_writer(
	struct run *run)
{
	int status;

	/* One writer at a time. */
	if (run->writing)
		return;

	/* The thread. */
	status = pthread_create(&run->writer, NULL, writer_run, run);
	if (status != 0) {
		perror("pthread_create");
		exit(2);
	}

	/* The thread runs until the script is written. */
	run->writing = 1;
}

/* Waits until the whole script is written, and empties it for the next part. */
static void
wait_writer(
	struct run *run)
{
	/* Nothing runs. */
	if (!run->writing)
		return;

	/* The thread's end, then an empty script. */
	(void)pthread_join(run->writer, NULL);
	run->writing = 0;
	run->script_used = 0U;
}

/* Writes each packet of the script in order, waiting while the session's end is full. */
static void *
writer_run(
	void *argument)
{
	struct run *run;
	size_t offset;
	size_t length;
	ssize_t written;

	/* Each packet. */
	run = argument;
	offset = 0U;
	while (offset < run->script_used) {
		length = (size_t)run->script[offset] | ((size_t)run->script[offset + 1U] << 8);
		written = write(run->controller, run->script + offset + 2U, length);
		if (written != (ssize_t)length) {
			perror("write");
			exit(2);
		}

		/* The next packet. */
		offset += 2U + length;
	}

	/* Succeeded: all written. */
	return NULL;
}

/*
 * Writes an ACL packet of a length: a first one with an L2CAP header of a
 * channel, or a continuing one; its last byte is the mark the handler
 * keeps.
 */
static void
put_acl(
	struct run *run,
	uint16_t handle,
	int continuing,
	uint16_t cid,
	uint8_t mark,
	size_t length)
{
	uint8_t packet[1100];
	unsigned field;

	/* The header: the handle with its boundary flag, the data's length. */
	memset(packet, 0, sizeof(packet));
	packet[0] = BT_PACKET_ACL;
	field = handle;
	if (continuing)
		field |= 0x1000U;
	else
		field |= 0x2000U;
	packet[1] = (uint8_t)(field & 0xffU);
	packet[2] = (uint8_t)(field >> 8);
	packet[3] = (uint8_t)(length & 0xffU);
	packet[4] = (uint8_t)(length >> 8);

	/* A first packet's L2CAP header (a frame longer than this packet), then the mark last. */
	if (!continuing) {
		packet[5] = 0xffU;
		packet[6] = 0x03U;
		packet[7] = (uint8_t)(cid & 0xffU);
		packet[8] = (uint8_t)(cid >> 8);
	}

	/* The mark, the last byte. */
	packet[5U + length - 1U] = mark;
	put(run, packet, 5U + length);
}

/* Writes a Connection Complete of a BR/EDR ACL connection. */
static void
put_connected(
	struct run *run,
	uint16_t handle)
{
	uint8_t packet[14];

	/* Status 0, the handle, an address, ACL, no encryption. */
	memset(packet, 0, sizeof(packet));
	packet[0] = BT_PACKET_EVENT;
	packet[1] = 0x03U;
	packet[2] = 11U;
	packet[4] = (uint8_t)(handle & 0xffU);
	packet[5] = (uint8_t)(handle >> 8);
	packet[6] = (uint8_t)handle;
	packet[12] = 0x01U;
	put(run, packet, sizeof(packet));
}

/* Writes a Disconnection Complete. */
static void
put_disconnected(
	struct run *run,
	uint16_t handle)
{
	uint8_t packet[7];

	/* Status 0, the handle, the reason. */
	packet[0] = BT_PACKET_EVENT;
	packet[1] = 0x05U;
	packet[2] = 4U;
	packet[3] = 0x00U;
	packet[4] = (uint8_t)(handle & 0xffU);
	packet[5] = (uint8_t)(handle >> 8);
	packet[6] = 0x13U;
	put(run, packet, sizeof(packet));
}

/* Writes an Encryption Change of a connection. */
static void
put_encryption(
	struct run *run,
	uint16_t handle)
{
	uint8_t packet[7];

	/* Status 0, the handle, on. */
	packet[0] = BT_PACKET_EVENT;
	packet[1] = 0x08U;
	packet[2] = 4U;
	packet[3] = 0x00U;
	packet[4] = (uint8_t)(handle & 0xffU);
	packet[5] = (uint8_t)(handle >> 8);
	packet[6] = 0x01U;
	put(run, packet, sizeof(packet));
}

/* Writes an LE advertising report. */
static void
put_report(
	struct run *run)
{
	uint8_t packet[15];

	/* Subevent 2, one report, no data. */
	memset(packet, 0, sizeof(packet));
	packet[0] = BT_PACKET_EVENT;
	packet[1] = 0x3eU;
	packet[2] = 12U;
	packet[3] = 0x02U;
	packet[4] = 0x01U;
	put(run, packet, sizeof(packet));
}

/* Writes the Command Complete of an opcode with status 0. */
static void
put_answer(
	struct run *run,
	uint16_t opcode)
{
	uint8_t packet[7];

	/* One command allowed, the opcode, status 0. */
	packet[0] = BT_PACKET_EVENT;
	packet[1] = 0x0eU;
	packet[2] = 4U;
	packet[3] = 0x01U;
	packet[4] = (uint8_t)(opcode & 0xffU);
	packet[5] = (uint8_t)(opcode >> 8);
	packet[6] = 0x00U;
	put(run, packet, sizeof(packet));
}

/* Writes what the script holds, then hands everything the session holds and the node has to the handler. */
static void
drain(
	struct run *run)
{
	unsigned rounds;
	int error;

	/* The script written whole (a part's packets after a command fit the socket's buffer). */
	start_writer(run);
	wait_writer(run);

	/* Until the node has nothing (bounded). */
	for (rounds = 0U; rounds < 10000U; rounds++) {
		error = btd_session_input(run->session);
		if (error != 0)
			break;
	}
}

/* Reads and forgets what the session wrote (commands and ACL packets). */
static void
drain_controller(
	struct run *run)
{
	uint8_t packet[1100];
	ssize_t got;

	/* Until nothing is left. */
	for (;;) {
		got = recv(run->controller, packet, sizeof(packet), MSG_DONTWAIT);
		if (got <= 0)
			break;
	}
}

/* Finds the notice of a handle among what was handed (TEST_HANDED_MAX: none). */
static unsigned
find_notice(
	const struct run *run,
	uint16_t handle)
{
	unsigned index;

	/* The first one. */
	for (index = 0U; index < run->count; index++) {
		if (run->types[index] == BTD_PACKET_DROP && run->handles[index] == handle)
			return index;
	}

	/* None. */
	return TEST_HANDED_MAX;
}

/*
 * (a): during a command, the keyboard's data, then 40 KB of the phone's
 * data (past the queue), then an Encryption Change, a third connection's
 * Connection and Disconnection Complete, and 40 LE reports (more than the
 * room above the events' reserve that the phone's flood leaves), then the
 * answer.
 */
static void
test_flood(void)
{
	static struct btd_session session;
	struct run run;
	unsigned index;
	unsigned notice;
	unsigned last_phone;
	unsigned phone_after;
	int keyboard_seen;
	int encryption_seen;
	int third_seen;
	int error;

	/* The packets, then the command. */
	open_run(&run, &session, 4U);
	put_acl(&run, TEST_KEYBOARD, 0, TEST_CID_DATA, 0x11U, 20U);
	for (index = 0U; index < 40U; index++)
		put_acl(&run, TEST_PHONE, 0, TEST_CID_DATA, (uint8_t)(0x40U + index), 1000U);
	put_encryption(&run, TEST_KEYBOARD);
	put_connected(&run, TEST_THIRD);
	put_disconnected(&run, TEST_THIRD);
	for (index = 0U; index < 40U; index++)
		put_report(&run);
	put_answer(&run, 0xfc01U);
	start_writer(&run);
	error = btd_session_command(&session, 0xfc01U, NULL, 0U);
	wait_writer(&run);
	check(error == 0, "flood: the command answered");
	check(session.queue_dropped != 0U, "flood: the queue was full");
	check(session.scan_dropped != 0U && session.scan_dropped < 40U, "flood: the reports past the room dropped");
	drain(&run);

	/* What was handed: the keyboard's data, the events, the phone's data before the drop, one notice after it. */
	keyboard_seen = 0;
	encryption_seen = 0;
	third_seen = 0;
	last_phone = 0U;
	phone_after = 0U;
	notice = find_notice(&run, TEST_PHONE);
	for (index = 0U; index < run.count; index++) {
		if (run.types[index] == BT_PACKET_ACL && run.handles[index] == TEST_KEYBOARD)
			keyboard_seen = 1;
		if (run.types[index] == BT_PACKET_EVENT)
			encryption_seen++;
		if (run.types[index] == BT_PACKET_ACL && run.handles[index] == TEST_PHONE) {
			last_phone = index;
			if (index > notice)
				phone_after++;
		}
	}

	/* The third connection's two events are among the events counted. */
	third_seen = encryption_seen;
	check(keyboard_seen, "flood: the keyboard's data came");
	check(third_seen >= 3, "flood: the encryption change and the third connection's events came");
	check(notice != TEST_HANDED_MAX, "flood: the phone's notice");
	check(notice != TEST_HANDED_MAX && last_phone < notice && phone_after == 0U, "flood: the notice after the phone's data");
	check(notice != TEST_HANDED_MAX && run.notice_flags[notice] == BTD_DROP_DATA && run.notice_cid[notice] == TEST_CID_DATA, "flood: data on channel 0x40");
	check(find_notice(&run, TEST_KEYBOARD) == TEST_HANDED_MAX, "flood: the keyboard not sealed");
	check(find_notice(&run, BTD_DROP_ALL) == TEST_HANDED_MAX, "flood: no events' notice for the reports");
	check(!btd_session_pending(&session), "flood: nothing pending at the end");
	close_run(&run);
}

/*
 * (b) and (c): the phone flooded during a first command; while the queue
 * drains, the handler runs a second command (at the keyboard's data, which
 * was queued after the phone's first packets), during which the phone's
 * continuing packet comes: it is dropped, not handed before the notice.
 * After the notice a continuing packet is passed over, a first one taken.
 */
static void
test_inside(void)
{
	static struct btd_session session;
	struct run run;
	unsigned index;
	unsigned notice;
	int bad;
	int error;

	/* The phone's first packets, then the keyboard's (where the handler's command runs), then the flood. */
	open_run(&run, &session, 4U);
	put_acl(&run, TEST_PHONE, 0, TEST_CID_DATA, 0x01U, 1000U);
	put_acl(&run, TEST_KEYBOARD, 0, TEST_CID_DATA, 0x11U, 20U);
	for (index = 0U; index < 40U; index++)
		put_acl(&run, TEST_PHONE, 0, TEST_CID_DATA, 0x02U, 1000U);
	put_answer(&run, 0xfc01U);

	/* The second command's packets: the phone's continuing packet, then its answer. */
	put_acl(&run, TEST_PHONE, 1, 0U, 0xc2U, 100U);
	put_answer(&run, 0xfc02U);
	start_writer(&run);
	error = btd_session_command(&session, 0xfc01U, NULL, 0U);
	wait_writer(&run);
	check(error == 0, "inside: the first command answered");
	run.command_inside = 1;
	drain(&run);
	check(run.inside_error == 0, "inside: the second command answered");

	/* The continuing packet never handed: before the notice it would have been joined to another frame. */
	bad = 0;
	for (index = 0U; index < run.count; index++) {
		if (run.types[index] == BT_PACKET_ACL && run.marks[index] == 0xc2U)
			bad = 1;
	}

	/* The phone's notice. */
	notice = find_notice(&run, TEST_PHONE);
	check(notice != TEST_HANDED_MAX, "inside: the notice");
	check(!bad, "inside: the continuing packet dropped");

	/* After the notice: a continuing packet passed over, then a first one taken. */
	run.count = 0U;
	put_acl(&run, TEST_PHONE, 1, 0U, 0xc3U, 100U);
	put_acl(&run, TEST_PHONE, 0, TEST_CID_DATA, 0xf1U, 100U);
	drain(&run);
	check(run.count == 1U && run.marks[0] == 0xf1U, "inside: continuing passed over, the first taken");
	check(session.continuing_skipped == 1U, "inside: one continuing passed over");
	close_run(&run);
}

/*
 * (d), (e) and (f): the phone's signalling packets are queued, the queue
 * fills, then its data on channel 0x40 is dropped: the notice says data on
 * 0x40, and handling the old signalling packets does not change the last
 * channel back; a hardware error with the queue full is still handled.
 * Nothing is queued after the drop, so once the queue is empty the notice
 * is due and not yet handed: pending says yes then.
 */
static void
test_arrival(void)
{
	static struct btd_session session;
	struct run run;
	unsigned index;
	unsigned notice;
	uint8_t hardware[4];
	int pending;
	int error;

	/* Signalling first (queued), the flood on 0x40, a hardware error, the answer. */
	open_run(&run, &session, 4U);
	for (index = 0U; index < 3U; index++)
		put_acl(&run, TEST_PHONE, 0, TEST_CID_SIGNALLING, 0x21U, 100U);
	for (index = 0U; index < 40U; index++)
		put_acl(&run, TEST_PHONE, 0, TEST_CID_DATA, 0x22U, 1000U);
	hardware[0] = BT_PACKET_EVENT;
	hardware[1] = 0x10U;
	hardware[2] = 1U;
	hardware[3] = 0x05U;
	put(&run, hardware, sizeof(hardware));
	put_answer(&run, 0xfc01U);
	start_writer(&run);
	error = btd_session_command(&session, 0xfc01U, NULL, 0U);
	wait_writer(&run);
	check(error == 0, "arrival: the command answered");
	check(session.hardware_errors == 1U && session.state == BTD_STATE_ERROR, "arrival: the hardware error handled");

	/* Handed until the queue is empty: the notice is due and still waits, and pending says so. */
	while (session.queue_used != 0U)
		(void)btd_session_input(&session);
	pending = btd_session_pending(&session);
	check(find_notice(&run, TEST_PHONE) == TEST_HANDED_MAX, "arrival: the notice not handed yet");
	check(pending, "arrival: pending while the notice is due");
	drain(&run);
	check(!btd_session_pending(&session), "arrival: nothing pending after the notice");

	/* The notice says data on 0x40 (the last channel as it arrived). */
	notice = find_notice(&run, TEST_PHONE);
	check(notice != TEST_HANDED_MAX && run.notice_flags[notice] == BTD_DROP_DATA && run.notice_cid[notice] == TEST_CID_DATA, "arrival: data on 0x40");
	check(session.links[0].last_cid == TEST_CID_DATA, "arrival: the last channel kept the order of arrival");
	close_run(&run);
}

/*
 * (g): 31 KB of the phone's data and then 150 Disconnection Completes of
 * unknown handles during a command: the last ones do not fit, and the
 * events' notice says counted.
 */
static void
test_counted(void)
{
	static struct btd_session session;
	struct run run;
	unsigned index;
	unsigned notice;
	int error;

	/* The data, the events, the answer. */
	open_run(&run, &session, 4U);
	for (index = 0U; index < 40U; index++)
		put_acl(&run, TEST_PHONE, 0, TEST_CID_DATA, 0x31U, 1000U);
	for (index = 0U; index < 600U; index++)
		put_disconnected(&run, (uint16_t)(0x0100U + index));
	put_answer(&run, 0xfc01U);
	start_writer(&run);
	error = btd_session_command(&session, 0xfc01U, NULL, 0U);
	wait_writer(&run);
	check(error == 0, "counted: the command answered");
	drain(&run);

	/* The events' notice, counted. */
	notice = find_notice(&run, BTD_DROP_ALL);
	check(notice != TEST_HANDED_MAX && (run.notice_flags[notice] & BTD_DROP_COUNTED) != 0U, "counted: noticed as counted");
	close_run(&run);
}

/*
 * Sending: the phone limited to 8 waiting frames and 1 packet in the
 * controller, a pool of 4: its first frame goes at once, 8 more wait and
 * the tenth is refused; the keyboard's 6 are taken and 3 of them go past
 * the phone's waiting frames; the room; a new connection of the handle has
 * no limit.
 */
static void
test_sending(void)
{
	static struct btd_session session;
	struct run run;
	uint8_t payload[16];
	uint8_t packet[1100];
	uint16_t order[8];
	unsigned count;
	unsigned index;
	unsigned room;
	ssize_t got;
	int error;

	/* The phone's limits. */
	open_run(&run, &session, 4U);
	error = btd_session_set_link_limits(&session, TEST_PHONE, BTD_SEND_PHONE_FRAMES, 1U);
	check(error == 0, "sending: limits set");
	error = btd_session_set_link_limits(&session, 0x0099U, 1U, 1U);
	check(error == ENOTCONN, "sending: an unknown connection");

	/* Nine frames of the phone taken (the first in the controller, 8 waiting), the tenth refused. */
	memset(payload, 0x5aU, sizeof(payload));
	for (index = 0U; index < 9U; index++) {
		error = btd_session_send(&session, TEST_PHONE, 0x0040U, payload, sizeof(payload));
		check(error == 0, "sending: a phone frame");
	}

	/* The tenth. */
	error = btd_session_send(&session, TEST_PHONE, 0x0040U, payload, sizeof(payload));
	check(error == ENOBUFS, "sending: the tenth refused");
	room = btd_session_link_room(&session, TEST_PHONE);
	check(room == 0U, "sending: no room left for the phone");

	/* The keyboard's six still taken. */
	for (index = 0U; index < 6U; index++) {
		error = btd_session_send(&session, TEST_KEYBOARD, 0x0001U, payload, sizeof(payload));
		check(error == 0, "sending: a keyboard frame");
	}

	/* The keyboard's room: its own share and the table's. */
	room = btd_session_link_room(&session, TEST_KEYBOARD);
	check(session.frame_count == 11U, "sending: 8 of the phone's and 3 of the keyboard's wait");
	check(room == BTD_SEND_FRAMES - 11U, "sending: the table's room for the keyboard");

	/* What went: one phone packet (its limit) and three of the keyboard's. */
	count = 0U;
	for (;;) {
		got = recv(run.controller, packet, sizeof(packet), MSG_DONTWAIT);
		if (got <= 0)
			break;
		if (packet[0] == BT_PACKET_ACL && count < 8U) {
			order[count] = (uint16_t)((packet[1] | (packet[2] << 8)) & 0x0fffU);
			count++;
		}
	}

	/* The packets the pool took. */
	check(count == 4U, "sending: the pool's four packets");
	check(count == 4U && order[0] == TEST_PHONE && order[1] == TEST_KEYBOARD, "sending: in turn");
	check(session.links[0].outstanding == 1U, "sending: the phone at its limit in the controller");

	/* A new connection of the phone's handle (after its end) has no limit but the session's. */
	put_disconnected(&run, TEST_PHONE);
	put_connected(&run, TEST_PHONE);
	drain(&run);
	drain_controller(&run);
	room = btd_session_link_room(&session, TEST_PHONE);
	check(room == BTD_SEND_FRAMES - session.frame_count, "sending: the limits reset with a new connection");
	close_run(&run);
}
