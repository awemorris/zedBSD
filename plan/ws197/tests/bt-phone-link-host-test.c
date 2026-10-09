/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the phone link's transport (ws197-p002,
 * plan/ws197/phase002/phase.md sections 7.4, 7.5, 11 and 12.1), built with
 * the host's compiler under ASan and UBSan.  The controller is a thread on
 * a socket pair that answers each command with a Command Status, keeps
 * each ACL packet bluetoothd sends and gives its buffer back (Number Of
 * Completed Packets).  The phone is played by the test with bluetoothd's
 * own L2CAP, SDP server, RFCOMM and OBEX server parts on its side: its
 * frames are handed to the phone link as the router would.
 *
 *   handoff    each condition that refuses a pairing's link (LE, a key
 *              not authenticated, a short key, another Class of Device, a
 *              phone there already, the HID host's links all in use); the
 *              one taken: the route, the session's share, the HID host's
 *              limit, the channel held Pending answered and served by the
 *              SDP server
 *   probe      PHONE PROBE against the phone's MAS: SDP finds channel 5,
 *              RFCOMM opens the DLC, OBEX connects, gets (the phone's OBEX
 *              server does not implement Get: 0xD1) and disconnects; and
 *              its refusals (not the phone, another class, busy)
 *   notices    lost RFCOMM data closes its channel alone, lost events are
 *              counted, lost signalling ends the link; the link's end
 *              gives the HID host its links back
 *   snoop      a phone's ACL packet hidden: the headers kept, the rest zero
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/hid.h"
#include "userland/base/bluetoothd/l2cap.h"
#include "userland/base/bluetoothd/obex.h"
#include "userland/base/bluetoothd/pair.h"
#include "userland/base/bluetoothd/phone.h"
#include "userland/base/bluetoothd/rfcomm.h"
#include "userland/base/bluetoothd/router.h"
#include "userland/base/bluetoothd/sdps.h"
#include "userland/base/bluetoothd/session.h"
#include "userland/base/bluetoothd/snoop.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* The commands and ACL packets the controller keeps, and the bytes it keeps of an ACL packet. */
#define TEST_RECORDS_MAX	256U
#define TEST_ACL_BYTES		1100U

/* The opcode the test looks for: Disconnect. */
#define TEST_DISCONNECT		0x0406U

/* The phone's link, its device's last address byte, and the uid that paired it. */
#define TEST_HANDLE		0x002aU
#define TEST_PHONE		0x51U
#define TEST_UID		1001U

/* The phone's MAS server channel. */
#define TEST_MAS_CHANNEL	5U

/* The link key types: authenticated P-256, Just Works P-256. */
#define TEST_KEY_MITM		0x08U
#define TEST_KEY_JUST_WORKS	0x07U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/*
 * The controller: its end of the socket pair, and what it was sent (under
 * the lock: the thread writes, the test takes): the commands' opcodes, and
 * the ACL packets not taken yet (a ring).
 */
struct controller {
	int descriptor;
	pthread_mutex_t lock;
	unsigned command_count;
	uint16_t opcodes[TEST_RECORDS_MAX];
	unsigned acl_first;
	unsigned acl_count;
	unsigned acl_total;
	size_t acl_lengths[TEST_RECORDS_MAX];
	uint8_t acl[TEST_RECORDS_MAX][TEST_ACL_BYTES];
};

/*
 * The phone the test plays: its channels and frame, its SDP records and
 * server, its RFCOMM session and OBEX server, and what it saw: the SDP
 * answers it got, the Disconnection Requests and Connection Responses.
 */
struct fake {
	struct btd_l2cap l2cap;
	struct btd_reassembly reassembly;
	struct btd_sdps_db db;
	uint16_t sdps_cid;
	struct btd_sdps sdps;
	uint16_t rfcomm_cid;
	int rfcomm_active;
	struct btd_rfcomm rfcomm;
	unsigned dlci;
	struct btd_obex obex;
	uint16_t query_cid;
	unsigned sdp_answers;
	unsigned disconnect_requests;
	unsigned connections;
	uint16_t last_result;
};

/* The run's world: the controller and its thread, the session, the pairing, the router, the HID host, the phone link, the phone, and the probe's answer. */
struct world {
	struct controller controller;
	pthread_t thread;
	struct btd_session session;
	struct btd_pair pair;
	struct btd_router router;
	struct btd_hid hid;
	struct btd_phone phone;
	struct btd_sdps_db records;
	struct fake fake;
	char answer[BTD_PHONE_ANSWER_MAX];
};

/* The run's world, one at a time; static for its size. */
static struct world world;

static void check(int condition, const char *what);
static void open_world(void);
static void close_world(void);
static void *controller_run(void *argument);
static unsigned commands_of(uint16_t opcode);
static int take_acl(uint8_t *packet, size_t *length);
static void exchange(void);
static void fake_init(void);
static void fake_acl(const uint8_t *packet, size_t length);
static void fake_signal(const uint8_t *payload, size_t length);
static void fake_to_phone(uint16_t cid, const uint8_t *payload, size_t length);
static int fake_accept(void *context, uint16_t handle, uint16_t psm, uint16_t *result, uint16_t *status);
static int hold_accept(void *context, uint16_t handle, uint16_t psm, uint16_t *result, uint16_t *status);
static int fake_rf_send(void *context, const uint8_t *payload, size_t length);
static int fake_rf_accept(void *context, unsigned server_channel);
static void fake_rf_opened(void *context, unsigned dlci);
static void fake_rf_data(void *context, unsigned dlci, const uint8_t *data, size_t length);
static void fake_rf_writable(void *context, unsigned dlci);
static int fake_ob_write(void *context, const uint8_t *data, size_t length, size_t *written);
static int fake_ob_target(void *context, const uint8_t *target, size_t length);
static void hook_answer(void *context, const char *line);
static int no_bridge(void *context, int *descriptor);
static void no_told(void *context, const uint8_t *address, const char *line);
static void make_address(uint8_t *address, uint8_t last);
static size_t mas_record(uint8_t *bytes, size_t size);
static int hand_over(uint8_t key_type, unsigned key_size, int have_class, uint32_t class_of_device, const char **why);
static void hand_notice(uint16_t handle, uint8_t flags, uint16_t cid);
static void test_refusals(void);
static void test_taken(void);
static void test_probe(void);
static void test_notices(void);
static void test_snoop(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_refusals();
	test_taken();
	test_probe();
	test_notices();
	test_snoop();

	/* The count of what failed. */
	printf("bt-phone-link-host-test: %u checks, %u failed\n", checks, failures);
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

/* Makes the run's world: a ready session on a socket pair with its controller's thread, the router, the HID host, the phone link and the phone. */
static void
open_world(void)
{
	struct btd_hid_hooks hooks;
	struct btd_router_phone owner;
	struct btd_link_count *link;
	int ends[2];
	int status;

	/* A socket pair that keeps packets apart, as the node does. */
	status = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ends);
	if (status != 0) {
		perror("socketpair");
		exit(2);
	}

	/* The controller. */
	memset(&world, 0, sizeof(world));
	world.controller.descriptor = ends[1];
	(void)pthread_mutex_init(&world.controller.lock, NULL);
	status = pthread_create(&world.thread, NULL, controller_run, &world.controller);
	if (status != 0) {
		perror("pthread_create");
		exit(2);
	}

	/* The session, ready, with eight ACL buffers and the phone's link counted. */
	btd_session_init(&world.session, ends[0], NULL, NULL, "fake", "/nonexistent");
	world.session.timing.command_ms = 300U;
	world.session.state = BTD_STATE_READY;
	world.session.acl_pool.length = 1021U;
	world.session.acl_pool.total = 8U;
	world.session.acl_pool.free = 8U;
	world.session.le_shared = 1;
	link = &world.session.links[0];
	link->used = 1;
	link->handle = TEST_HANDLE;
	make_address(link->address, TEST_PHONE);
	link->frame_limit = BTD_SEND_FRAMES;

	/* The pairing, the router and the HID host, as the daemon has them. */
	btd_pair_init(&world.pair, &world.session, "/nonexistent", NULL, NULL, NULL, NULL, NULL);
	btd_router_init(&world.router, &world.pair);
	world.session.handler = btd_router_handle;
	world.session.handler_context = &world.router;
	memset(&hooks, 0, sizeof(hooks));
	hooks.open_bridge = no_bridge;
	hooks.answer = no_told;
	btd_hid_init(&world.hid, &world.session, "/nonexistent", &world.router, &hooks);

	/* The phone link, the router's owner of the phone's link. */
	btd_sdps_db_init(&world.records);
	btd_phone_init(&world.phone, &world.session, &world.router, &world.hid, &world.records, hook_answer, NULL);
	owner.context = &world.phone;
	owner.wants = btd_phone_wants;
	owner.claims = btd_phone_claims;
	owner.handle = btd_phone_handle;
	btd_router_set_phone(&world.router, &owner);

	/* The phone. */
	fake_init();
}

/* Ends the controller's thread (the session's end closes) and closes both ends. */
static void
close_world(void)
{
	/* The session's end, then the thread. */
	(void)shutdown(world.session.descriptor, SHUT_RDWR);
	(void)pthread_join(world.thread, NULL);
	(void)close(world.session.descriptor);
	(void)close(world.controller.descriptor);
	(void)pthread_mutex_destroy(&world.controller.lock);
}

/*
 * The controller: answers each command with a Command Status and keeps
 * each ACL packet, giving its buffer back at once, until the session's end
 * closes.
 */
static void *
controller_run(
	void *argument)
{
	struct controller *controller;
	uint8_t packet[TEST_ACL_BYTES];
	uint8_t answer[8];
	uint16_t opcode;
	unsigned slot;
	size_t answer_length;
	ssize_t written;
	ssize_t got;

	/* Each packet. */
	controller = argument;
	for (;;) {
		got = read(controller->descriptor, packet, sizeof(packet));
		if (got <= 0)
			return NULL;

		/* An ACL packet: kept for the phone, and its buffer given back (Number Of Completed Packets: one handle, one packet). */
		if (packet[0] == BT_PACKET_ACL) {
			(void)pthread_mutex_lock(&controller->lock);
			if (controller->acl_count < TEST_RECORDS_MAX) {
				slot = (controller->acl_first + controller->acl_count) % TEST_RECORDS_MAX;
				memcpy(controller->acl[slot], packet, (size_t)got);
				controller->acl_lengths[slot] = (size_t)got;
				controller->acl_count++;
				controller->acl_total++;
			}

			/* The record is the test's to take. */
			(void)pthread_mutex_unlock(&controller->lock);

			/* The buffer back. */
			answer[0] = BT_PACKET_EVENT;
			answer[1] = 0x13U;
			answer[2] = 5U;
			answer[3] = 1U;
			answer[4] = packet[1];
			answer[5] = (uint8_t)(packet[2] & 0x0fU);
			answer[6] = 1U;
			answer[7] = 0U;
			answer_length = 8U;
		} else if (packet[0] == 0x01U && got >= 4) {
			/* A command: recorded, and its Command Status. */
			opcode = (uint16_t)(packet[1] | (packet[2] << 8));
			(void)pthread_mutex_lock(&controller->lock);
			if (controller->command_count < TEST_RECORDS_MAX) {
				controller->opcodes[controller->command_count] = opcode;
				controller->command_count++;
			}

			/* The record is the test's again; the answer. */
			(void)pthread_mutex_unlock(&controller->lock);
			answer[0] = BT_PACKET_EVENT;
			answer[1] = 0x0fU;
			answer[2] = 4U;
			answer[3] = 0x00U;
			answer[4] = 0x01U;
			answer[5] = (uint8_t)(opcode & 0xffU);
			answer[6] = (uint8_t)(opcode >> 8);
			answer_length = 7U;
		} else {
			/* Anything else is passed over. */
			continue;
		}

		/* Sent back. */
		written = write(controller->descriptor, answer, answer_length);
		if (written != (ssize_t)answer_length)
			return NULL;
	}
}

/* Counts the commands of an opcode the controller was sent. */
static unsigned
commands_of(
	uint16_t opcode)
{
	unsigned count;
	unsigned index;

	/* Each one recorded. */
	count = 0U;
	(void)pthread_mutex_lock(&world.controller.lock);
	for (index = 0U; index < world.controller.command_count; index++) {
		if (world.controller.opcodes[index] == opcode)
			count++;
	}

	/* The record is the thread's again. */
	(void)pthread_mutex_unlock(&world.controller.lock);

	/* The count. */
	return count;
}

/* Takes the oldest ACL packet the controller kept; reports whether there was one. */
static int
take_acl(
	uint8_t *packet,
	size_t *length)
{
	int taken;

	/* The oldest, under the lock. */
	taken = 0;
	(void)pthread_mutex_lock(&world.controller.lock);

	if (world.controller.acl_count != 0U) {
		*length = world.controller.acl_lengths[world.controller.acl_first];
		memcpy(packet, world.controller.acl[world.controller.acl_first], *length);
		world.controller.acl_first = (world.controller.acl_first + 1U) % TEST_RECORDS_MAX;
		world.controller.acl_count--;
		taken = 1;
	}

	(void)pthread_mutex_unlock(&world.controller.lock);

	/* Whether one was taken. */
	return taken;
}

/*
 * Lets the phone link and the phone talk until neither has anything more
 * to say: the phone link's frames go to the session, the controller gives
 * the buffers back, and the phone answers each packet.
 */
static void
exchange(void)
{
	struct timespec pause;
	uint8_t packet[TEST_ACL_BYTES];
	size_t length;
	unsigned quiet;
	unsigned rounds;
	int taken;
	int error;

	/* Rounds of 2 ms, until 25 in a row bring nothing. */
	pause.tv_sec = 0;
	pause.tv_nsec = 2000000L;
	quiet = 0U;
	for (rounds = 0U; rounds < 5000U && quiet < 25U; rounds++) {
		btd_phone_pump(&world.phone);
		if (world.fake.rfcomm_active)
			btd_rfcomm_pump(&world.fake.rfcomm);

		/* The buffers given back. */
		do {
			error = btd_session_input(&world.session);
		} while (error == 0);

		/* Each packet the controller kept, to the phone. */
		quiet++;
		for (;;) {
			taken = take_acl(packet, &length);
			if (!taken)
				break;
			quiet = 0U;
			fake_acl(packet, length);
		}

		/* A little time for the controller's thread. */
		(void)nanosleep(&pause, NULL);
	}
}

/* Prepares the phone: its table accepting SDP and RFCOMM, its MAS record, its RFCOMM listening on the MAS channel. */
static void
fake_init(void)
{
	uint8_t pairs[256];
	uint32_t handle;
	size_t length;
	int error;

	/* Its channels. */
	btd_l2cap_init(&world.fake.l2cap);
	btd_l2cap_set_accept(&world.fake.l2cap, fake_accept, NULL);

	/* Its MAS record. */
	btd_sdps_db_init(&world.fake.db);
	length = mas_record(pairs, sizeof(pairs));
	error = btd_sdps_register(&world.fake.db, pairs, length, &handle);
	check(error == 0, "setup: the phone's MAS record");
}

/* Takes an ACL packet bluetoothd sent: a frame, once whole, goes to the phone's channel. */
static void
fake_acl(
	const uint8_t *packet,
	size_t length)
{
	struct btd_acl acl;
	const uint8_t *payload;
	uint8_t answer[BTD_L2CAP_MAX];
	size_t payload_length;
	size_t answer_length;
	uint16_t cid;
	int whole;
	int error;

	/* The frame, once whole. */
	error = btd_acl_parse(packet, length, &acl);
	if (error != 0)
		return;
	whole = btd_reassembly_feed(&world.fake.reassembly, &acl);
	if (whole <= 0)
		return;
	payload = world.fake.reassembly.frame + BTD_L2CAP_HEADER;
	payload_length = world.fake.reassembly.expected - BTD_L2CAP_HEADER;
	cid = (uint16_t)(world.fake.reassembly.frame[2] | (world.fake.reassembly.frame[3] << 8));

	/* The signalling. */
	if (cid == BTD_CID_SIGNALLING) {
		fake_signal(payload, payload_length);
		return;
	}

	/* The SDP server's request. */
	if (world.fake.sdps_cid != 0U && cid == world.fake.sdps_cid) {
		error = btd_sdps_input(&world.fake.sdps, payload, payload_length, answer, sizeof(answer), &answer_length);
		if (error == 0 && answer_length != 0U)
			fake_to_phone(world.fake.sdps_cid, answer, answer_length);
		return;
	}

	/* An answer of bluetoothd's SDP server. */
	if (world.fake.query_cid != 0U && cid == world.fake.query_cid) {
		world.fake.sdp_answers++;
		return;
	}

	/* RFCOMM. */
	if (world.fake.rfcomm_active && cid == world.fake.rfcomm_cid)
		btd_rfcomm_input(&world.fake.rfcomm, payload, payload_length, btd_now_ms());
}

/* Takes a signalling frame on the phone's side: answered, and the channels that open start their parts. */
static void
fake_signal(
	const uint8_t *payload,
	size_t length)
{
	struct btd_signal_effect effect;
	struct btd_rfcomm_events events;
	struct btd_channel *channel;
	uint8_t answer[BTD_SIGNAL_MAX];
	size_t answer_length;
	size_t offset;
	unsigned index;

	/* What bluetoothd's frame says: Disconnection Requests and Connection Responses counted. */
	for (offset = 0U; offset + 4U <= length; offset += 4U + (size_t)(payload[offset + 2] | (payload[offset + 3] << 8))) {
		if (payload[offset] == 0x06U)
			world.fake.disconnect_requests++;
		if (payload[offset] == 0x03U && offset + 12U <= length) {
			world.fake.connections++;
			world.fake.last_result = (uint16_t)(payload[offset + 8] | (payload[offset + 9] << 8));
		}
	}

	/* The commands, answered. */
	(void)btd_l2cap_signal(&world.fake.l2cap, TEST_HANDLE, 0, payload, length, answer, sizeof(answer), &answer_length, &effect);
	if (answer_length != 0U)
		fake_to_phone(BTD_CID_SIGNALLING, answer, answer_length);

	/* Each channel that opened. */
	for (index = 0U; index < effect.opened_count; index++) {
		channel = btd_l2cap_channel(&world.fake.l2cap, effect.opened[index]);
		if (channel == NULL)
			continue;

		/* bluetoothd's SDP query: served. */
		if (channel->psm == 0x0001U && channel->inbound) {
			world.fake.sdps_cid = channel->local_cid;
			btd_sdps_init(&world.fake.sdps, &world.fake.db, channel->remote_mtu);
			continue;
		}

		/* The phone's own SDP query of bluetoothd. */
		if (channel->psm == 0x0001U) {
			world.fake.query_cid = channel->local_cid;
			continue;
		}

		/* RFCOMM: the phone answers as the responder, with MAS's channel. */
		memset(&events, 0, sizeof(events));
		events.context = NULL;
		events.send = fake_rf_send;
		events.accept = fake_rf_accept;
		events.opened = fake_rf_opened;
		events.data = fake_rf_data;
		events.writable = fake_rf_writable;
		world.fake.rfcomm_cid = channel->local_cid;
		world.fake.rfcomm_active = 1;
		btd_rfcomm_init(&world.fake.rfcomm, &events, channel->remote_mtu);
		(void)btd_rfcomm_listen(&world.fake.rfcomm, TEST_MAS_CHANNEL);
	}

	/* Succeeded: a closed channel's part ends (its CID may be given again). */
	for (index = 0U; index < effect.closed_count; index++) {
		if (effect.closed[index] == world.fake.rfcomm_cid)
			world.fake.rfcomm_active = 0;
		if (effect.closed[index] == world.fake.sdps_cid)
			world.fake.sdps_cid = 0U;
		if (effect.closed[index] == world.fake.query_cid)
			world.fake.query_cid = 0U;
	}
}

/* Sends a frame of the phone to the phone link, on the signalling channel or on one of the phone's channels (to bluetoothd's end of it). */
static void
fake_to_phone(
	uint16_t cid,
	const uint8_t *payload,
	size_t length)
{
	struct btd_channel *channel;
	uint8_t frame[BTD_L2CAP_HEADER + BTD_L2CAP_MAX];
	uint8_t packet[TEST_ACL_BYTES];
	uint16_t destination;
	size_t frame_length;
	size_t packet_length;

	/* Where it goes. */
	destination = cid;
	if (cid != BTD_CID_SIGNALLING) {
		channel = btd_l2cap_channel(&world.fake.l2cap, cid);
		if (channel == NULL)
			return;
		destination = channel->remote_cid;
	}

	/* Succeeded: one ACL packet, handed over as the router would. */
	frame_length = btd_l2cap_frame(frame, sizeof(frame), destination, payload, length);
	packet_length = btd_acl_build(packet, sizeof(packet), TEST_HANDLE, BTD_ACL_FIRST_FLUSHABLE, frame, frame_length);
	btd_phone_handle(&world.phone, &world.session, packet, packet_length);
}

/* The phone's table's answer: SDP and RFCOMM accepted. */
static int
fake_accept(
	void *context,
	uint16_t handle,
	uint16_t psm,
	uint16_t *result,
	uint16_t *status)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(handle);

	/* Another PSM. */
	if (psm != 0x0001U && psm != 0x0003U)
		return 1;

	/* Accepted. */
	*result = BTD_L2CAP_SUCCESS;
	*status = 0x0000U;
	return 0;
}

/* The pairing's table's answer while a phone pairs (pair.c's): SDP held Pending. */
static int
hold_accept(
	void *context,
	uint16_t handle,
	uint16_t psm,
	uint16_t *result,
	uint16_t *status)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(handle);

	/* Another PSM. */
	if (psm != 0x0001U)
		return 1;

	/* Pending. */
	*result = BTD_L2CAP_PENDING;
	*status = 0x0000U;
	return 0;
}

/* The phone's RFCOMM sends on its channel. */
static int
fake_rf_send(
	void *context,
	const uint8_t *payload,
	size_t length)
{
	UNUSED_PARAMETER(context);

	/* To the phone link. */
	fake_to_phone(world.fake.rfcomm_cid, payload, length);
	return 0;
}

/* The phone's RFCOMM offers the MAS channel. */
static int
fake_rf_accept(
	void *context,
	unsigned server_channel)
{
	UNUSED_PARAMETER(context);

	/* MAS's. */
	if (server_channel == TEST_MAS_CHANNEL)
		return 1;

	/* Another. */
	return 0;
}

/* The phone's DLC opened: an OBEX server on it. */
static void
fake_rf_opened(
	void *context,
	unsigned dlci)
{
	struct btd_obex_events events;

	UNUSED_PARAMETER(context);

	/* The server. */
	world.fake.dlci = dlci;
	memset(&events, 0, sizeof(events));
	events.context = NULL;
	events.write = fake_ob_write;
	events.target = fake_ob_target;
	btd_obex_init(&world.fake.obex, &events, BTD_OBEX_SERVER);
}

/* The phone's DLC data: to its OBEX server. */
static void
fake_rf_data(
	void *context,
	unsigned dlci,
	const uint8_t *data,
	size_t length)
{
	UNUSED_PARAMETER(context);

	/* The server's DLC. */
	if (dlci != world.fake.dlci)
		return;
	btd_obex_input(&world.fake.obex, data, length, btd_now_ms());
}

/* The phone's DLC writable: its OBEX server's rest. */
static void
fake_rf_writable(
	void *context,
	unsigned dlci)
{
	UNUSED_PARAMETER(context);

	/* The server's DLC. */
	if (dlci != world.fake.dlci)
		return;
	btd_obex_pump(&world.fake.obex);
}

/* The phone's OBEX server writes on its DLC. */
static int
fake_ob_write(
	void *context,
	const uint8_t *data,
	size_t length,
	size_t *written)
{
	int error;

	UNUSED_PARAMETER(context);

	/* As far as the credits take it. */
	error = btd_rfcomm_write(&world.fake.rfcomm, world.fake.dlci, data, length, written);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* The phone's OBEX server serves MAS's Target. */
static int
fake_ob_target(
	void *context,
	const uint8_t *target,
	size_t length)
{
	static const uint8_t mas[16] = {
		0xbbU, 0x58U, 0x2bU, 0x40U, 0x42U, 0x0cU, 0x11U, 0xdbU,
		0xb0U, 0xdeU, 0x08U, 0x00U, 0x20U, 0x0cU, 0x9aU, 0x66U
	};
	int same;

	UNUSED_PARAMETER(context);

	/* MAS's sixteen bytes. */
	if (target == NULL || length != sizeof(mas))
		return 0;
	same = memcmp(target, mas, sizeof(mas));
	if (same == 0)
		return 1;

	/* Another. */
	return 0;
}

/* The probe's answer, kept. */
static void
hook_answer(
	void *context,
	const char *line)
{
	UNUSED_PARAMETER(context);

	/* Kept. */
	(void)snprintf(world.answer, sizeof(world.answer), "%s", line);
}

/* The HID host's bridge: none in this test. */
static int
no_bridge(
	void *context,
	int *descriptor)
{
	UNUSED_PARAMETER(context);

	/* Refused. */
	*descriptor = -1;
	return EACCES;
}

/* The HID host's answers: none in this test. */
static void
no_told(
	void *context,
	const uint8_t *address,
	const char *line)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(line);
}

/* Makes a device's address that ends in a byte. */
static void
make_address(
	uint8_t *address,
	uint8_t last)
{
	/* 0A:0B:0C:0D:0E:last, least significant byte first. */
	address[0] = last;
	address[1] = 0x0eU;
	address[2] = 0x0dU;
	address[3] = 0x0cU;
	address[4] = 0x0bU;
	address[5] = 0x0aU;
}

/* Writes the pairs of the phone's MAS record: the class 0x1132, L2CAP, RFCOMM with the channel, OBEX. */
static size_t
mas_record(
	uint8_t *bytes,
	size_t size)
{
	struct btd_sdp_writer writer;

	/* ServiceClassIDList. */
	btd_sdp_writer_init(&writer, bytes, size);
	btd_sdp_put_uint16(&writer, 0x0001U);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x1132U);
	btd_sdp_end(&writer);

	/* ProtocolDescriptorList. */
	btd_sdp_put_uint16(&writer, 0x0004U);
	btd_sdp_begin(&writer);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x0100U);
	btd_sdp_end(&writer);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x0003U);
	btd_sdp_put_uint8(&writer, (uint8_t)TEST_MAS_CHANNEL);
	btd_sdp_end(&writer);
	btd_sdp_begin(&writer);
	btd_sdp_put_uuid16(&writer, 0x0008U);
	btd_sdp_end(&writer);
	btd_sdp_end(&writer);

	/* Succeeded: the pairs' length. */
	check(!writer.overflow, "setup: MAS record written");
	return writer.used;
}

/*
 * Offers the phone's link to the phone link as a phone's pairing would,
 * with a key of a type and size and a Class of Device, and a channel the
 * phone asked for during the pairing held Pending in the pairing's table.
 * Returns the hook's answer.
 */
static int
hand_over(
	uint8_t key_type,
	unsigned key_size,
	int have_class,
	uint32_t class_of_device,
	const char **why)
{
	static struct btd_l2cap pairing;
	static struct btd_reassembly frame;
	struct btd_pair_handoff handoff;
	struct btd_signal_effect effect;
	struct btd_bond bond;
	uint8_t address[BTD_ADDRESS_BYTES];
	uint8_t request[16];
	uint8_t answer[BTD_SIGNAL_MAX];
	size_t request_length;
	size_t answer_length;
	uint16_t phone_cid;
	int taken;

	/* The pairing's table, its accept hook holding the phone's SDP channel Pending: the phone's Connection Request. */
	btd_l2cap_init(&pairing);
	btd_l2cap_set_accept(&pairing, hold_accept, NULL);
	(void)btd_l2cap_connect(&world.fake.l2cap, TEST_HANDLE, 0x0001U, request, sizeof(request), &request_length, &phone_cid);
	(void)btd_l2cap_signal(&pairing, TEST_HANDLE, 0, request, request_length, answer, sizeof(answer), &answer_length, &effect);
	memset(&frame, 0, sizeof(frame));

	/* The bond. */
	memset(&bond, 0, sizeof(bond));
	make_address(bond.address, TEST_PHONE);
	bond.type = BTD_ADDRESS_BREDR;
	bond.have_link_key = 1;
	bond.link_key_type = key_type;
	bond.key_size = (uint8_t)key_size;

	/* What the pairing hands over. */
	make_address(address, TEST_PHONE);
	memset(&handoff, 0, sizeof(handoff));
	handoff.address = address;
	handoff.type = BTD_ADDRESS_BREDR;
	handoff.handle = TEST_HANDLE;
	handoff.bond = &bond;
	handoff.uid = TEST_UID;
	handoff.key_size = key_size;
	handoff.have_class = have_class;
	handoff.class_of_device = class_of_device;
	handoff.l2cap = &pairing;
	handoff.reassembly = &frame;
	*why = NULL;
	taken = btd_phone_handoff(&world.phone, &handoff, why);

	/* Succeeded: the hook's answer. */
	return taken;
}

/* Hands the phone link a notice of dropped packets on its link. */
static void
hand_notice(
	uint16_t handle,
	uint8_t flags,
	uint16_t cid)
{
	uint8_t notice[BTD_DROP_LENGTH];

	/* The notice: its type, the handle, the flags, the first data channel, one packet. */
	notice[0] = BTD_PACKET_DROP;
	notice[1] = (uint8_t)(handle & 0xffU);
	notice[2] = (uint8_t)(handle >> 8);
	notice[3] = flags;
	notice[4] = (uint8_t)(cid & 0xffU);
	notice[5] = (uint8_t)(cid >> 8);
	notice[6] = 1U;
	notice[7] = 0U;
	btd_phone_handle(&world.phone, &world.session, notice, sizeof(notice));
}

/* Each condition that refuses a pairing's link, in its order. */
static void
test_refusals(void)
{
	const char *why;
	unsigned index;
	int taken;

	/* A Just Works key. */
	open_world();
	taken = hand_over(TEST_KEY_JUST_WORKS, 16U, 0, 0U, &why);
	check(!taken && why != NULL && strcmp(why, "unauthenticated") == 0, "refusals: a Just Works key");

	/* A short key. */
	taken = hand_over(TEST_KEY_MITM, 7U, 0, 0U, &why);
	check(!taken && why != NULL && strcmp(why, "key-size") == 0, "refusals: a short key");

	/* A computer's Class of Device. */
	taken = hand_over(TEST_KEY_MITM, 16U, 1, 0x00010cU, &why);
	check(!taken && why != NULL && strcmp(why, "not-phone") == 0, "refusals: not a phone");

	/* The HID host with six links. */
	for (index = 0U; index < BTD_HID_MAX; index++) {
		world.hid.devices[index].used = 1;
		world.hid.devices[index].state = BTD_HID_OPEN;
	}

	/* Refused for the links. */
	taken = hand_over(TEST_KEY_MITM, 16U, 1, 0x5a020cU, &why);
	check(!taken && why != NULL && strcmp(why, "busy-links") == 0, "refusals: the HID host's links all in use");
	memset(world.hid.devices, 0, sizeof(world.hid.devices));

	/* A phone there already. */
	world.phone.state = BTD_PHONE_READY;
	taken = hand_over(TEST_KEY_MITM, 16U, 1, 0x5a020cU, &why);
	check(!taken && why != NULL && strcmp(why, "busy") == 0, "refusals: a phone there already");
	world.phone.state = BTD_PHONE_NONE;
	check(btd_router_owner(&world.router, TEST_HANDLE) == BTD_OWNER_NONE, "refusals: the route not taken");
	close_world();
}

/* The link taken: its route, its share, the HID host's limit, the held channel answered and served. */
static void
test_taken(void)
{
	static const uint8_t search[] = {
		0x06U, 0x00U, 0x01U, 0x00U, 0x0fU,
		0x35U, 0x03U, 0x19U, 0x11U, 0x33U,
		0x00U, 0x40U,
		0x35U, 0x05U, 0x0aU, 0x00U, 0x00U, 0xffU, 0xffU,
		0x00U
	};
	const char *why;
	int taken;

	/* A phone the scan did not see. */
	open_world();
	taken = hand_over(TEST_KEY_MITM, 16U, 0, 0U, &why);
	check(taken == 1, "taken: the link taken");
	check(world.phone.state == BTD_PHONE_READY && world.phone.uid == TEST_UID && world.phone.class_unknown, "taken: the phone, its uid, no class");
	check(btd_router_owner(&world.router, TEST_HANDLE) == BTD_OWNER_PHONE, "taken: the route is the phone link's");
	check(world.session.links[0].frame_limit == BTD_SEND_PHONE_FRAMES && world.session.links[0].inflight_limit == 6U, "taken: the link's share of the session");
	check(world.hid.limit == BTD_PHONE_HID_LIMIT, "taken: the HID host keeps a link free");

	/* The held SDP channel answered: configured on both sides. */
	exchange();
	check(world.fake.connections == 1U && world.fake.last_result == BTD_L2CAP_SUCCESS, "taken: the held channel accepted");
	check(world.phone.sdps[0].cid != 0U, "taken: an SDP server on it");

	/* The phone's search of bluetoothd's records: answered (none offered). */
	fake_to_phone(world.fake.query_cid, search, sizeof(search));
	exchange();
	check(world.fake.sdp_answers == 1U, "taken: the phone's SDP search answered");
	close_world();
}

/* PHONE PROBE against the phone's MAS, and its refusals. */
static void
test_probe(void)
{
	uint8_t address[BTD_ADDRESS_BYTES];
	const char *why;
	int same;
	int error;

	/* No phone yet. */
	open_world();
	make_address(address, TEST_PHONE);
	error = btd_phone_probe(&world.phone, address, BTD_SDP_UUID_MAS, btd_now_ms());
	check(error == ENOTCONN, "probe: no phone");

	/* The phone's link, and the probe's refusals. */
	(void)hand_over(TEST_KEY_MITM, 16U, 1, 0x5a020cU, &why);
	exchange();
	error = btd_phone_probe(&world.phone, address, 0x1234U, btd_now_ms());
	check(error == EINVAL, "probe: another class");
	address[0] = 0x52U;
	error = btd_phone_probe(&world.phone, address, BTD_SDP_UUID_MAS, btd_now_ms());
	check(error == ENOTCONN, "probe: another device");

	/* The probe: SDP, RFCOMM, OBEX's Connect, Get (not implemented by the phone's server) and Disconnect. */
	address[0] = TEST_PHONE;
	error = btd_phone_probe(&world.phone, address, BTD_SDP_UUID_MAS, btd_now_ms());
	check(error == 0, "probe: started");
	error = btd_phone_probe(&world.phone, address, BTD_SDP_UUID_MAS, btd_now_ms());
	check(error == EBUSY, "probe: one at a time");
	exchange();
	same = strcmp(world.answer, "PROBE uuid=0x1132 channel=5 connect=0xa0 get=0xd1 bytes=0 disconnect=0xa0");
	check(same == 0, "probe: every step answered");
	if (same != 0)
		printf("  probe answer: %s\n", world.answer);
	check(world.phone.rfcomm_active && world.fake.rfcomm_active, "probe: the RFCOMM session up");
	close_world();
}

/* The notices of the phone's link, and its end. */
static void
test_notices(void)
{
	uint8_t address[BTD_ADDRESS_BYTES];
	uint8_t ended[7];
	const char *why;
	unsigned requests;
	int error;

	/* A link with an RFCOMM session (a probe made it). */
	open_world();
	make_address(address, TEST_PHONE);
	(void)hand_over(TEST_KEY_MITM, 16U, 1, 0x5a020cU, &why);
	exchange();
	error = btd_phone_probe(&world.phone, address, BTD_SDP_UUID_MAS, btd_now_ms());
	exchange();
	check(error == 0 && world.phone.rfcomm_active, "notices: a session up");

	/* Lost events: counted, nothing ends. */
	hand_notice(TEST_HANDLE, BTD_DROP_EVENT, 0U);
	check(world.phone.notices == 1U && world.phone.rfcomm_active, "notices: lost events counted");

	/* Lost RFCOMM data: its channel alone closes. */
	requests = world.fake.disconnect_requests;
	world.phone.reassembly.active = 1;
	hand_notice(TEST_HANDLE, BTD_DROP_DATA, world.phone.rfcomm_cid);
	exchange();
	check(world.phone.reassembly.active == 0, "notices: the frame being put together dropped");
	check(!world.phone.rfcomm_active && world.fake.disconnect_requests == requests + 1U, "notices: lost RFCOMM data closes its channel");
	check(world.phone.state == BTD_PHONE_READY && commands_of(TEST_DISCONNECT) == 0U, "notices: the link stays");

	/* Lost signalling: the link ends. */
	hand_notice(TEST_HANDLE, BTD_DROP_SIGNAL, 0x0001U);
	check(world.phone.state == BTD_PHONE_CLOSING && commands_of(TEST_DISCONNECT) == 1U, "notices: lost signalling ends the link");

	/* Its Disconnection Complete: no phone, the HID host's links all its own. */
	ended[0] = BT_PACKET_EVENT;
	ended[1] = 0x05U;
	ended[2] = 4U;
	ended[3] = 0x00U;
	ended[4] = (uint8_t)(TEST_HANDLE & 0xffU);
	ended[5] = (uint8_t)(TEST_HANDLE >> 8);
	ended[6] = 0x13U;
	btd_router_handle(&world.router, &world.session, ended, sizeof(ended));
	check(world.phone.state == BTD_PHONE_NONE && world.hid.limit == BTD_HID_MAX, "notices: the link's end");
	check(btd_router_owner(&world.router, TEST_HANDLE) == BTD_OWNER_NONE, "notices: the route gone");
	close_world();
}

/* A phone's ACL packet hidden in the btsnoop record. */
static void
test_snoop(void)
{
	static const uint8_t first[] = { 0x02U, 0x2aU, 0x20U, 0x08U, 0x00U, 0x04U, 0x00U, 0x41U, 0x00U, 0xdeU, 0xadU, 0xbeU, 0xefU };
	static const uint8_t continuing[] = { 0x02U, 0x2aU, 0x10U, 0x04U, 0x00U, 0xdeU, 0xadU, 0xbeU, 0xefU };
	uint8_t out[32];
	size_t length;
	int same;

	/* A packet that starts a frame: both headers kept. */
	length = btd_snoop_hide(first, sizeof(first), out, sizeof(out));
	same = memcmp(out, first, 9U);
	check(length == sizeof(first) && same == 0, "snoop: the headers kept");
	check(out[9] == 0U && out[10] == 0U && out[11] == 0U && out[12] == 0U, "snoop: the data zero");

	/* A continuing packet: the ACL header alone. */
	length = btd_snoop_hide(continuing, sizeof(continuing), out, sizeof(out));
	same = memcmp(out, continuing, 5U);
	check(length == sizeof(continuing) && same == 0 && out[5] == 0U && out[8] == 0U, "snoop: a continuing packet's data zero");
}
