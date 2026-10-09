/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the router's and the link manager's parts for the
 * phone link (ws197-p002, plan/ws197/phase002/phase.md sections 5, 6 and
 * 12.1), built with the host's compiler under ASan and UBSan.  The
 * controller is a thread on a socket pair that records each command and
 * answers it with a Command Status (status 0, or 0x0C for the opcode the
 * part makes fail); the events are handed to the router directly, and the
 * session's counted connections are set in its table.
 *
 *   router     a synchronous link's Connection Request refused with Reject
 *              Synchronous Connection Request (0x0D, or 0x0E when the
 *              device's ACL link is on AES-CCM), a synchronous Connection
 *              Complete ended or passed over, Synchronous Connection
 *              Complete passed over; an ACL Connection Request and Link Key
 *              Request to the HID host first, then the phone link, else
 *              refused; a connection's owner the HID host before the phone
 *              link; a connection's notice to its owner, the events' to
 *              each owner with its handle; a lost connection event checks
 *              the routes once the session's queue is empty (a route
 *              without its connection, or with another device's, hears a
 *              made-up Disconnection Complete and goes; a connection
 *              without a route is ended); the link manager's page ended by
 *              its device's Connection Complete
 *   linkmgr    page scan written when what anybody wants changes, and only
 *              then; a refused write tried again at the tick; one page at
 *              a time, ended by its caller, by another device's end not,
 *              and by the tick after 15 s; a reset writes again
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/linkmgr.h"
#include "userland/base/bluetoothd/pair.h"
#include "userland/base/bluetoothd/router.h"
#include "userland/base/bluetoothd/session.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The commands the controller keeps, and the bytes of each one's parameters. */
#define TEST_COMMANDS_MAX	64U
#define TEST_PARAMETERS_MAX	16U

/* The opcodes the test looks for. */
#define TEST_DISCONNECT		0x0406U
#define TEST_REJECT		0x040aU
#define TEST_KEY_NEGATIVE	0x040cU
#define TEST_REJECT_SYNC	0x042aU
#define TEST_WRITE_SCAN		0x0c1aU

/* What a hook keeps: the packets' first byte, the event code and the handle (or the notice's). */
#define TEST_HEARD_MAX		32U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/*
 * The controller: its end of the socket pair, the commands it was sent
 * (under the lock: the thread writes, the test reads), and the opcode it
 * answers with a failure (0: none).
 */
struct controller {
	int descriptor;
	pthread_mutex_t lock;
	unsigned count;
	uint16_t opcodes[TEST_COMMANDS_MAX];
	uint8_t lengths[TEST_COMMANDS_MAX];
	uint8_t parameters[TEST_COMMANDS_MAX][TEST_PARAMETERS_MAX];
	uint16_t fail_opcode;
};

/*
 * One owner the test plays (the HID host or the phone link): the
 * addresses it wants and claims (the last byte; 0 for none), and what it
 * heard.
 */
struct owner {
	uint8_t wants;
	uint8_t claims;
	unsigned count;
	uint8_t types[TEST_HEARD_MAX];
	uint8_t codes[TEST_HEARD_MAX];
	uint16_t handles[TEST_HEARD_MAX];
	uint8_t flags[TEST_HEARD_MAX];
	uint8_t reasons[TEST_HEARD_MAX];
};

/* A part's world: the controller and its thread, the session, the pairing, the router, the link manager, the two owners. */
struct world {
	struct controller controller;
	pthread_t thread;
	struct btd_session session;
	struct btd_pair pair;
	struct btd_router router;
	struct btd_linkmgr linkmgr;
	struct owner hid;
	struct owner phone;
};

static void check(int condition, const char *what);
static void open_world(struct world *world);
static void close_world(struct world *world);
static void *controller_run(void *argument);
static unsigned commands_of(struct world *world, uint16_t opcode, uint8_t *last_parameters);
static void forget_commands(struct world *world);
static void no_ask(void *context, unsigned kind, uint32_t number);
static void no_done(void *context, const char *answer);
static void test_random(void *context, uint8_t *bytes, size_t length);
static int owner_wants(void *context, const uint8_t *address);
static int owner_claims(void *context, const uint8_t *address);
static void owner_handle(void *context, struct btd_session *session, const uint8_t *packet, size_t length);
static void make_address(uint8_t *address, uint8_t last);
static void hand_request(struct world *world, uint8_t last, uint8_t link_type);
static void hand_connected(struct world *world, uint8_t status, uint16_t handle, uint8_t last, uint8_t link_type);
static void hand_key_request(struct world *world, uint8_t last);
static void hand_encryption(struct world *world, uint16_t handle, uint8_t enabled);
static void hand_notice(struct world *world, uint16_t handle, uint8_t flags);
static void count_link(struct world *world, unsigned slot, uint16_t handle, uint8_t last);
static void test_synchronous(void);
static void test_owners(void);
static void test_notices(void);
static void test_reconcile(void);
static void test_scan(void);
static void test_page(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_synchronous();
	test_owners();
	test_notices();
	test_reconcile();
	test_scan();
	test_page();

	/* The count of what failed. */
	printf("bt-router-host-test: %u checks, %u failed\n", checks, failures);
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

/* Makes a part's world: a ready session on a socket pair with its controller's thread, the router with both owners and the link manager. */
static void
open_world(
	struct world *world)
{
	struct btd_router_hid hid;
	struct btd_router_phone phone;
	int ends[2];
	int status;

	/* A socket pair that keeps packets apart, as the node does. */
	status = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ends);
	if (status != 0) {
		perror("socketpair");
		exit(2);
	}

	/* The controller. */
	memset(world, 0, sizeof(*world));
	world->controller.descriptor = ends[1];
	(void)pthread_mutex_init(&world->controller.lock, NULL);
	status = pthread_create(&world->thread, NULL, controller_run, &world->controller);
	if (status != 0) {
		perror("pthread_create");
		exit(2);
	}

	/* The session, ready, and the pairing, idle. */
	btd_session_init(&world->session, ends[0], NULL, NULL, "fake", "/nonexistent");
	world->session.timing.command_ms = 300U;
	world->session.state = BTD_STATE_READY;
	btd_pair_init(&world->pair, &world->session, "/nonexistent", no_ask, no_done, NULL, test_random, NULL);

	/* The router with the two owners and the link manager. */
	btd_router_init(&world->router, &world->pair);
	hid.context = &world->hid;
	hid.wants = owner_wants;
	hid.claims = owner_claims;
	hid.handle = owner_handle;
	btd_router_set_hid(&world->router, &hid);
	phone.context = &world->phone;
	phone.wants = owner_wants;
	phone.claims = owner_claims;
	phone.handle = owner_handle;
	btd_router_set_phone(&world->router, &phone);
	btd_linkmgr_init(&world->linkmgr, &world->session);
	btd_router_set_linkmgr(&world->router, &world->linkmgr);
	world->session.handler = btd_router_handle;
	world->session.handler_context = &world->router;
}

/* Ends the controller's thread (the session's end closes) and closes both ends. */
static void
close_world(
	struct world *world)
{
	/* The session's end, then the thread. */
	(void)shutdown(world->session.descriptor, SHUT_RDWR);
	(void)pthread_join(world->thread, NULL);
	(void)close(world->session.descriptor);
	(void)close(world->controller.descriptor);
	(void)pthread_mutex_destroy(&world->controller.lock);
}

/* The controller: records each command and answers it with a Command Status, until the session's end closes. */
static void *
controller_run(
	void *argument)
{
	struct controller *controller;
	uint8_t packet[300];
	uint8_t answer[7];
	uint16_t opcode;
	size_t length;
	ssize_t written;
	ssize_t got;

	/* Each command. */
	controller = argument;
	for (;;) {
		got = read(controller->descriptor, packet, sizeof(packet));
		if (got <= 0)
			return NULL;
		if (packet[0] != 0x01U || got < 4)
			continue;

		/* Recorded. */
		opcode = (uint16_t)(packet[1] | (packet[2] << 8));
		length = packet[3];
		if (length > TEST_PARAMETERS_MAX)
			length = TEST_PARAMETERS_MAX;
		(void)pthread_mutex_lock(&controller->lock);
		if (controller->count < TEST_COMMANDS_MAX) {
			controller->opcodes[controller->count] = opcode;
			controller->lengths[controller->count] = (uint8_t)length;
			memcpy(controller->parameters[controller->count], packet + 4, length);
			controller->count++;
		}

		/* The status the part asked for. */
		answer[3] = 0x00U;
		if (opcode == controller->fail_opcode)
			answer[3] = 0x0cU;
		(void)pthread_mutex_unlock(&controller->lock);

		/* Command Status: the status, one command allowed, the opcode. */
		answer[0] = BT_PACKET_EVENT;
		answer[1] = 0x0fU;
		answer[2] = 4U;
		answer[4] = 0x01U;
		answer[5] = (uint8_t)(opcode & 0xffU);
		answer[6] = (uint8_t)(opcode >> 8);
		written = write(controller->descriptor, answer, sizeof(answer));
		if (written != (ssize_t)sizeof(answer))
			return NULL;
	}
}

/* Counts the commands of an opcode the controller was sent, and copies the last one's parameters. */
static unsigned
commands_of(
	struct world *world,
	uint16_t opcode,
	uint8_t *last_parameters)
{
	unsigned count;
	unsigned index;

	/* Each one recorded. */
	count = 0U;
	(void)pthread_mutex_lock(&world->controller.lock);
	for (index = 0U; index < world->controller.count; index++) {
		if (world->controller.opcodes[index] != opcode)
			continue;
		count++;
		if (last_parameters != NULL)
			memcpy(last_parameters, world->controller.parameters[index], TEST_PARAMETERS_MAX);
	}

	/* The record is the thread's again. */
	(void)pthread_mutex_unlock(&world->controller.lock);

	/* The count. */
	return count;
}

/* Forgets the commands recorded, and what both owners heard. */
static void
forget_commands(
	struct world *world)
{
	/* The controller's record. */
	(void)pthread_mutex_lock(&world->controller.lock);
	world->controller.count = 0U;
	(void)pthread_mutex_unlock(&world->controller.lock);

	/* The owners'. */
	world->hid.count = 0U;
	world->phone.count = 0U;
}

/* The pairing's question hook: no pairing runs in this test. */
static void
no_ask(
	void *context,
	unsigned kind,
	uint32_t number)
{
	/* Nothing asked. */
	(void)context;
	(void)kind;
	(void)number;
}

/* The pairing's end hook: no pairing runs in this test. */
static void
no_done(
	void *context,
	const char *answer)
{
	/* Nothing ends. */
	(void)context;
	(void)answer;
}

/* Random bytes for the pairing: a fixed pattern (no pairing runs). */
static void
test_random(
	void *context,
	uint8_t *bytes,
	size_t length)
{
	/* The pattern. */
	(void)context;
	memset(bytes, 0x5a, length);
}

/* An owner's want of a device that connects: its address's last byte. */
static int
owner_wants(
	void *context,
	const uint8_t *address)
{
	struct owner *owner;

	/* Wanted when the last byte matches. */
	owner = context;
	if (owner->wants != 0U && address[5] == owner->wants)
		return 1;

	/* Not wanted. */
	return 0;
}

/* An owner's claim of a connection to a device: its address's last byte. */
static int
owner_claims(
	void *context,
	const uint8_t *address)
{
	struct owner *owner;

	/* Claimed when the last byte matches. */
	owner = context;
	if (owner->claims != 0U && address[5] == owner->claims)
		return 1;

	/* Not claimed. */
	return 0;
}

/* Keeps what an owner hears: an event's code and handle (Disconnection Complete's reason), a notice's handle and flags. */
static void
owner_handle(
	void *context,
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	struct owner *owner;
	unsigned index;

	/* Room in the record. */
	(void)session;
	owner = context;
	if (owner->count >= TEST_HEARD_MAX || length < 3U)
		return;
	index = owner->count;
	owner->count++;
	owner->types[index] = packet[0];
	owner->codes[index] = 0U;
	owner->handles[index] = 0U;
	owner->flags[index] = 0U;
	owner->reasons[index] = 0U;

	/* A notice: its handle and flags. */
	if (packet[0] == BTD_PACKET_DROP && length == BTD_DROP_LENGTH) {
		owner->handles[index] = (uint16_t)(packet[1] | (packet[2] << 8));
		owner->flags[index] = packet[3];
		return;
	}

	/* An event: its code; Disconnection Complete's handle and reason. */
	if (packet[0] == BT_PACKET_EVENT) {
		owner->codes[index] = packet[1];
		if (packet[1] == 0x05U && length >= 7U) {
			owner->handles[index] = (uint16_t)(packet[4] | (packet[5] << 8));
			owner->reasons[index] = packet[6];
		}
	}
}

/* Makes a device's address that ends in a byte. */
static void
make_address(
	uint8_t *address,
	uint8_t last)
{
	/* A fixed head, the byte last. */
	address[0] = 0x11U;
	address[1] = 0x22U;
	address[2] = 0x33U;
	address[3] = 0x44U;
	address[4] = 0x55U;
	address[5] = last;
}

/* Hands the router a Connection Request of a device and a link type. */
static void
hand_request(
	struct world *world,
	uint8_t last,
	uint8_t link_type)
{
	uint8_t packet[13];

	/* The address, a phone's class, the link type. */
	memset(packet, 0, sizeof(packet));
	packet[0] = BT_PACKET_EVENT;
	packet[1] = 0x04U;
	packet[2] = 10U;
	make_address(packet + 3, last);
	packet[10] = 0x02U;
	packet[12] = link_type;
	btd_router_handle(&world->router, &world->session, packet, sizeof(packet));
}

/* Hands the router a BR/EDR Connection Complete. */
static void
hand_connected(
	struct world *world,
	uint8_t status,
	uint16_t handle,
	uint8_t last,
	uint8_t link_type)
{
	uint8_t packet[14];

	/* The status, the handle, the address, the link type, no encryption. */
	memset(packet, 0, sizeof(packet));
	packet[0] = BT_PACKET_EVENT;
	packet[1] = 0x03U;
	packet[2] = 11U;
	packet[3] = status;
	packet[4] = (uint8_t)(handle & 0xffU);
	packet[5] = (uint8_t)(handle >> 8);
	make_address(packet + 6, last);
	packet[12] = link_type;
	btd_router_handle(&world->router, &world->session, packet, sizeof(packet));
}

/* Hands the router a Link Key Request of a device. */
static void
hand_key_request(
	struct world *world,
	uint8_t last)
{
	uint8_t packet[9];

	/* The address. */
	packet[0] = BT_PACKET_EVENT;
	packet[1] = 0x17U;
	packet[2] = 6U;
	make_address(packet + 3, last);
	btd_router_handle(&world->router, &world->session, packet, sizeof(packet));
}

/* Hands the router an Encryption Change that succeeded. */
static void
hand_encryption(
	struct world *world,
	uint16_t handle,
	uint8_t enabled)
{
	uint8_t packet[7];

	/* Status 0, the handle, the cipher. */
	packet[0] = BT_PACKET_EVENT;
	packet[1] = 0x08U;
	packet[2] = 4U;
	packet[3] = 0x00U;
	packet[4] = (uint8_t)(handle & 0xffU);
	packet[5] = (uint8_t)(handle >> 8);
	packet[6] = enabled;
	btd_router_handle(&world->router, &world->session, packet, sizeof(packet));
}

/* Hands the router a notice of dropped packets. */
static void
hand_notice(
	struct world *world,
	uint16_t handle,
	uint8_t flags)
{
	uint8_t packet[BTD_DROP_LENGTH];

	/* The handle, the flags, channel 0x40, one dropped. */
	packet[0] = BTD_PACKET_DROP;
	packet[1] = (uint8_t)(handle & 0xffU);
	packet[2] = (uint8_t)(handle >> 8);
	packet[3] = flags;
	packet[4] = 0x40U;
	packet[5] = 0x00U;
	packet[6] = 0x01U;
	packet[7] = 0x00U;
	btd_router_handle(&world->router, &world->session, packet, sizeof(packet));
}

/* Puts a connection in the session's table of counted connections. */
static void
count_link(
	struct world *world,
	unsigned slot,
	uint16_t handle,
	uint8_t last)
{
	struct btd_link_count *link;

	/* The slot, used, with its handle and device. */
	link = &world->session.links[slot];
	memset(link, 0, sizeof(*link));
	link->used = 1;
	link->handle = handle;
	make_address(link->address, last);
	link->frame_limit = BTD_SEND_FRAMES;
}

/* Synchronous links: refused, ended or passed over. */
static void
test_synchronous(void)
{
	static struct world world;
	uint8_t parameters[TEST_PARAMETERS_MAX];
	uint8_t packet[20];
	unsigned count;

	/* An SCO request: Reject Synchronous Connection Request, limited resources. */
	open_world(&world);
	world.hid.wants = 0x0aU;
	hand_request(&world, 0x0aU, 0x00U);
	count = commands_of(&world, TEST_REJECT_SYNC, parameters);
	check(count == 1U && parameters[5] == 0x0aU && parameters[6] == 0x0dU, "synchronous: SCO refused, resources");
	check(world.hid.count == 0U, "synchronous: the HID host does not hear an SCO request");

	/* An eSCO request of a device on an AES-CCM ACL link: security reasons. */
	forget_commands(&world);
	world.phone.claims = 0x0bU;
	hand_connected(&world, 0x00U, 0x0041U, 0x0bU, 0x01U);
	hand_encryption(&world, 0x0041U, 0x02U);
	hand_request(&world, 0x0bU, 0x02U);
	count = commands_of(&world, TEST_REJECT_SYNC, parameters);
	check(count == 1U && parameters[6] == 0x0eU, "synchronous: eSCO refused, security on AES-CCM");

	/* The same device after its link went to E0: resources again. */
	forget_commands(&world);
	hand_encryption(&world, 0x0041U, 0x01U);
	hand_request(&world, 0x0bU, 0x00U);
	count = commands_of(&world, TEST_REJECT_SYNC, parameters);
	check(count == 1U && parameters[6] == 0x0dU, "synchronous: resources without AES-CCM");

	/* An SCO Connection Complete that came up anyway is ended; a failed one passed over. */
	forget_commands(&world);
	hand_connected(&world, 0x00U, 0x0050U, 0x0bU, 0x00U);
	count = commands_of(&world, TEST_DISCONNECT, parameters);
	check(count == 1U && parameters[0] == 0x50U, "synchronous: an SCO connection ended");
	check(world.phone.count == 0U && btd_router_owner(&world.router, 0x0050U) == BTD_OWNER_NONE, "synchronous: nobody owns it");
	hand_connected(&world, 0x04U, 0x0051U, 0x0bU, 0x00U);
	count = commands_of(&world, TEST_DISCONNECT, NULL);
	check(count == 1U, "synchronous: a failed SCO passed over");

	/* Synchronous Connection Complete: passed over. */
	forget_commands(&world);
	memset(packet, 0, sizeof(packet));
	packet[0] = BT_PACKET_EVENT;
	packet[1] = 0x2cU;
	packet[2] = 17U;
	btd_router_handle(&world.router, &world.session, packet, sizeof(packet));
	check(world.router.synchronous_ignored >= 1U && world.phone.count == 0U && world.hid.count == 0U, "synchronous: Synchronous Connection Complete passed over");
	close_world(&world);
}

/* The owners in their order: requests, keys and connections. */
static void
test_owners(void)
{
	static struct world world;
	unsigned count;

	/* A device both want: the HID host hears its Connection Request. */
	open_world(&world);
	world.hid.wants = 0x0aU;
	world.phone.wants = 0x0aU;
	hand_request(&world, 0x0aU, 0x01U);
	check(world.hid.count == 1U && world.hid.codes[0] == 0x04U && world.phone.count == 0U, "owners: the HID host first");

	/* A device only the phone link wants: the phone link hears it. */
	world.phone.wants = 0x0cU;
	hand_request(&world, 0x0cU, 0x01U);
	check(world.phone.count == 1U && world.phone.codes[0] == 0x04U, "owners: then the phone link");

	/* A device nobody wants: refused. */
	hand_request(&world, 0x0dU, 0x01U);
	count = commands_of(&world, TEST_REJECT, NULL);
	check(count == 1U, "owners: nobody's request refused");

	/* Link Key Request: the phone link's device to it, an unknown one has none. */
	forget_commands(&world);
	hand_key_request(&world, 0x0cU);
	check(world.phone.count == 1U && world.phone.codes[0] == 0x17U, "owners: the phone link's key request");
	hand_key_request(&world, 0x0dU);
	count = commands_of(&world, TEST_KEY_NEGATIVE, NULL);
	check(count == 1U, "owners: no key for anyone else");

	/* Connections: one both claim is the HID host's, one only the phone link claims is its own. */
	forget_commands(&world);
	world.hid.claims = 0x0aU;
	world.phone.claims = 0x0aU;
	hand_connected(&world, 0x00U, 0x0040U, 0x0aU, 0x01U);
	check(btd_router_owner(&world.router, 0x0040U) == BTD_OWNER_HID, "owners: the HID host's connection");
	world.phone.claims = 0x0cU;
	hand_connected(&world, 0x00U, 0x0041U, 0x0cU, 0x01U);
	check(btd_router_owner(&world.router, 0x0041U) == BTD_OWNER_PHONE, "owners: the phone link's connection");
	check(world.phone.count == 1U && world.phone.codes[0] == 0x03U, "owners: the phone link hears its Connection Complete");
	close_world(&world);
}

/* The session's notices: a connection's to its owner, the events' to each owner. */
static void
test_notices(void)
{
	static struct world world;

	/* Two connections: the HID host's and the phone link's. */
	open_world(&world);
	world.hid.claims = 0x0aU;
	world.phone.claims = 0x0cU;
	hand_connected(&world, 0x00U, 0x0040U, 0x0aU, 0x01U);
	hand_connected(&world, 0x00U, 0x0041U, 0x0cU, 0x01U);
	forget_commands(&world);

	/* The phone link's connection's notice: to the phone link alone. */
	hand_notice(&world, 0x0041U, BTD_DROP_DATA);
	check(world.phone.count == 1U && world.phone.types[0] == BTD_PACKET_DROP && world.phone.handles[0] == 0x0041U, "notices: the connection's to its owner");
	check(world.hid.count == 0U, "notices: not to the other owner");

	/* A notice of nobody's connection: passed over. */
	hand_notice(&world, 0x0099U, BTD_DROP_DATA);
	check(world.phone.count == 1U && world.hid.count == 0U, "notices: nobody's passed over");

	/* The events' notice: to each owner with its own handle, marked as events. */
	forget_commands(&world);
	hand_notice(&world, BTD_DROP_ALL, BTD_DROP_EVENT);
	check(world.hid.count == 1U && world.hid.handles[0] == 0x0040U && world.hid.flags[0] == BTD_DROP_EVENT, "notices: the events' to the HID host with its handle");
	check(world.phone.count == 1U && world.phone.handles[0] == 0x0041U && world.phone.flags[0] == BTD_DROP_EVENT, "notices: the events' to the phone link with its handle");
	check(!world.router.reconcile_due, "notices: no check of the routes for events alone");
	close_world(&world);
}

/* A lost connection event: the routes checked against the session once its queue is empty. */
static void
test_reconcile(void)
{
	static struct world world;
	uint8_t parameters[TEST_PARAMETERS_MAX];
	unsigned count;

	/* Routes: the HID host's 0x40 (still counted), the phone link's 0x41 (gone), the HID host's 0x42 (its handle now another device's). */
	open_world(&world);
	world.hid.claims = 0x0aU;
	world.phone.claims = 0x0cU;
	hand_connected(&world, 0x00U, 0x0040U, 0x0aU, 0x01U);
	hand_connected(&world, 0x00U, 0x0041U, 0x0cU, 0x01U);
	world.hid.claims = 0x0eU;
	hand_connected(&world, 0x00U, 0x0042U, 0x0eU, 0x01U);
	memset(world.session.links, 0, sizeof(world.session.links));
	count_link(&world, 0U, 0x0040U, 0x0aU);
	count_link(&world, 1U, 0x0042U, 0x0fU);
	count_link(&world, 2U, 0x0043U, 0x0dU);
	forget_commands(&world);

	/* The notice while the session still holds queued packets: the check waits. */
	world.session.queue_used = 10U;
	hand_notice(&world, BTD_DROP_ALL, BTD_DROP_COUNTED);
	check(world.router.reconcile_due && world.router.reconciled == 0U, "reconcile: waits for the queue");
	check(btd_router_owner(&world.router, 0x0041U) == BTD_OWNER_PHONE, "reconcile: nothing changed yet");

	/* The next packet handed with the queue empty: the check runs. */
	world.session.queue_used = 0U;
	hand_encryption(&world, 0x0040U, 0x01U);
	check(!world.router.reconcile_due && world.router.reconciled == 1U, "reconcile: done once the queue is empty");

	/* The phone link's connection gone: it hears a Disconnection Complete with a timeout, and the route goes. */
	check(world.phone.count == 1U && world.phone.codes[0] == 0x05U && world.phone.handles[0] == 0x0041U && world.phone.reasons[0] == 0x08U, "reconcile: the phone link hears its connection ended");
	check(btd_router_owner(&world.router, 0x0041U) == BTD_OWNER_NONE, "reconcile: the gone route ended");

	/* The handle used again by another device: the HID host hears its connection ended. */
	check(btd_router_owner(&world.router, 0x0042U) == BTD_OWNER_NONE, "reconcile: the reused handle's route ended");
	check(btd_router_owner(&world.router, 0x0040U) == BTD_OWNER_HID, "reconcile: the counted route kept");

	/* The connections without a route (the reused handle, and one never routed) are ended. */
	count = commands_of(&world, TEST_DISCONNECT, parameters);
	check(count == 2U, "reconcile: the connections without a route ended");
	check(parameters[0] == 0x43U, "reconcile: the last one ended is 0x43");

	/* A notice while the queue is empty checks at once. */
	forget_commands(&world);
	hand_notice(&world, BTD_DROP_ALL, BTD_DROP_COUNTED | BTD_DROP_EVENT);
	check(world.router.reconciled == 2U && !world.router.reconcile_due, "reconcile: at once with the queue empty");
	check(world.hid.count >= 1U && world.hid.flags[0] == BTD_DROP_EVENT, "reconcile: the events' part still told");
	close_world(&world);
}

/* Page scan: written when what anybody wants changes. */
static void
test_scan(void)
{
	static struct world world;
	uint8_t parameters[TEST_PARAMETERS_MAX];
	unsigned count;
	int error;

	/* The HID host wants it: written once, page scan. */
	open_world(&world);
	error = btd_linkmgr_want_scan(&world.linkmgr, BTD_LINKMGR_HID, 1);
	count = commands_of(&world, TEST_WRITE_SCAN, parameters);
	check(error == 0 && count == 1U && parameters[0] == 0x02U, "scan: on for the HID host");

	/* The phone link too, the HID host again, the HID host's no: nothing written. */
	error = btd_linkmgr_want_scan(&world.linkmgr, BTD_LINKMGR_PHONE, 1);
	check(error == 0, "scan: the phone link's want");
	(void)btd_linkmgr_want_scan(&world.linkmgr, BTD_LINKMGR_HID, 1);
	(void)btd_linkmgr_want_scan(&world.linkmgr, BTD_LINKMGR_HID, 0);
	count = commands_of(&world, TEST_WRITE_SCAN, NULL);
	check(count == 1U, "scan: not written while somebody still wants it");

	/* Nobody: off. */
	(void)btd_linkmgr_want_scan(&world.linkmgr, BTD_LINKMGR_PHONE, 0);
	count = commands_of(&world, TEST_WRITE_SCAN, parameters);
	check(count == 2U && parameters[0] == 0x00U, "scan: off for nobody");

	/* A refused write: tried again at the tick, written once it goes through. */
	forget_commands(&world);
	world.controller.fail_opcode = TEST_WRITE_SCAN;
	error = btd_linkmgr_want_scan(&world.linkmgr, BTD_LINKMGR_PHONE, 1);
	check(error == EIO && world.linkmgr.scan_retry, "scan: a refusal reported and kept for a retry");
	world.controller.fail_opcode = 0U;
	(void)btd_linkmgr_tick(&world.linkmgr, 1000U);
	count = commands_of(&world, TEST_WRITE_SCAN, parameters);
	check(count == 2U && parameters[0] == 0x02U && !world.linkmgr.scan_retry, "scan: written at the tick");
	(void)btd_linkmgr_tick(&world.linkmgr, 2000U);
	count = commands_of(&world, TEST_WRITE_SCAN, NULL);
	check(count == 2U, "scan: not written again");

	/* A reset (a new controller): the same want is written again. */
	btd_linkmgr_reset(&world.linkmgr);
	(void)btd_linkmgr_want_scan(&world.linkmgr, BTD_LINKMGR_PHONE, 1);
	count = commands_of(&world, TEST_WRITE_SCAN, NULL);
	check(count == 3U, "scan: written again after a reset");
	close_world(&world);
}

/* One page at a time, and each way it ends. */
static void
test_page(void)
{
	static struct world world;
	uint8_t first[BTD_ADDRESS_BYTES];
	uint8_t second[BTD_ADDRESS_BYTES];
	int expired;
	int error;

	/* The HID host's page; the pairing's is refused meanwhile. */
	open_world(&world);
	make_address(first, 0x0aU);
	make_address(second, 0x0bU);
	error = btd_linkmgr_page_begin(&world.linkmgr, BTD_LINKMGR_HID, first, 1000U);
	check(error == 0 && btd_linkmgr_paging(&world.linkmgr), "page: begun");
	error = btd_linkmgr_page_begin(&world.linkmgr, BTD_LINKMGR_PAIR, second, 1100U);
	check(error == EBUSY, "page: a second refused");

	/* Another device's end: passed over; its own caller's end: over. */
	btd_linkmgr_page_end(&world.linkmgr, BTD_LINKMGR_PAIR, second);
	check(btd_linkmgr_paging(&world.linkmgr), "page: another device's end passed over");
	btd_linkmgr_page_end(&world.linkmgr, BTD_LINKMGR_HID, first);
	check(!btd_linkmgr_paging(&world.linkmgr), "page: ended by its caller");

	/* The router's Connection Complete of the device (a failure) ends it; another device's does not. */
	(void)btd_linkmgr_page_begin(&world.linkmgr, BTD_LINKMGR_PHONE, second, 2000U);
	hand_connected(&world, 0x04U, 0x0000U, 0x0aU, 0x01U);
	check(btd_linkmgr_paging(&world.linkmgr), "page: another device's Connection Complete");
	hand_connected(&world, 0x04U, 0x0000U, 0x0bU, 0x01U);
	check(!btd_linkmgr_paging(&world.linkmgr), "page: ended by its device's failed Connection Complete");

	/* An SCO Connection Complete of the device does not end it. */
	(void)btd_linkmgr_page_begin(&world.linkmgr, BTD_LINKMGR_PHONE, second, 3000U);
	hand_connected(&world, 0x04U, 0x0000U, 0x0bU, 0x00U);
	check(btd_linkmgr_paging(&world.linkmgr), "page: an SCO Connection Complete does not end it");

	/* The tick: within 15 s kept, after it ended with the device kept for the log. */
	expired = btd_linkmgr_tick(&world.linkmgr, 3000U + BTD_LINKMGR_PAGE_MS);
	check(expired == 0 && btd_linkmgr_paging(&world.linkmgr), "page: kept within its time");
	expired = btd_linkmgr_tick(&world.linkmgr, 3001U + BTD_LINKMGR_PAGE_MS);
	check(expired == 1 && !btd_linkmgr_paging(&world.linkmgr) && world.linkmgr.expired_address[5] == 0x0bU, "page: ended by the tick");

	/* A reset forgets a page out. */
	(void)btd_linkmgr_page_begin(&world.linkmgr, BTD_LINKMGR_HID, first, 4000U);
	btd_linkmgr_reset(&world.linkmgr);
	check(!btd_linkmgr_paging(&world.linkmgr), "page: forgotten by a reset");
	close_world(&world);
}
