/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of how the HID host and the pairing use the link manager
 * and take the session's notices of dropped packets (ws197-p002,
 * plan/ws197/phase002/phase.md sections 3.5, 6.2 and 12.1), built with
 * the host's compiler under ASan and UBSan.  The controller is a thread on
 * a socket pair that records each command and answers it with a Command
 * Status (status 0, or 0x0C for the opcode a part makes fail); the events
 * are handed to the HID host, the pairing or the router directly.
 *
 *   scan       the HID host's page scan written by the link manager, once
 *   token      the HID host's page takes the controller's one BR/EDR page;
 *              a pairing meanwhile is busy; a HID page while the pairing's
 *              page is out waits 2 s and is not counted as a try
 *   exits      the HID host's page ends at its Connection Complete (made
 *              or failed), its time, the controller's loss and its Create
 *              Connection's refusal; the pairing's at its Connection
 *              Complete, its time and its Create Connection's refusal
 *   notices    a notice of lost data, signalling or unknown packets ends a
 *              HID connection; lost events end one that waits for an event
 *              and leave an open one; the frame being put together goes;
 *              a notice on the pairing's connection stops the pairing
 *
 *   plan/ws197/tests/bt-phone-host-test.sh KEYS_FOLDER
 */

#include "userland/base/bluetoothd/hid.h"
#include "userland/base/bluetoothd/hidcache.h"
#include "userland/base/bluetoothd/keys.h"
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

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* The commands the controller keeps. */
#define TEST_COMMANDS_MAX	64U

/* The opcodes the test looks for. */
#define TEST_CREATE		0x0405U
#define TEST_DISCONNECT		0x0406U
#define TEST_CREATE_CANCEL	0x0408U
#define TEST_WRITE_SCAN		0x0c1aU

/* The HID device's and the paired device's last address byte, and the HID device's handle. */
#define TEST_KEYBOARD		0x41U
#define TEST_OTHER		0x42U
#define TEST_HANDLE		0x0021U
#define TEST_PAIR_HANDLE	0x0022U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/* The folder of the bonds and records (the script's new folder). */
static const char *keys_folder;

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
	uint16_t fail_opcode;
};

/* A run's world: the controller and its thread, the session, the pairing, the router, the link manager, the HID host, and the pairing's end. */
struct world {
	struct controller controller;
	pthread_t thread;
	struct btd_session session;
	struct btd_pair pair;
	struct btd_router router;
	struct btd_linkmgr linkmgr;
	struct btd_hid hid;
	char answer[BTD_PAIR_ANSWER_MAX];
};

/*
 * The run's world, one at a time.  It is static for its size (the HID
 * host's table), filled by open_world and emptied by close_world.
 */
static struct world world;

static void check(int condition, const char *what);
static void open_world(void);
static void close_world(void);
static void *controller_run(void *argument);
static unsigned commands_of(uint16_t opcode);
static void fail_opcode(uint16_t opcode);
static void no_ask(void *context, unsigned kind, uint32_t number);
static void pair_done(void *context, const char *answer);
static void test_random(void *context, uint8_t *bytes, size_t length);
static int no_bridge(void *context, int *descriptor);
static void no_answer(void *context, const uint8_t *address, const char *line);
static void make_address(uint8_t *address, uint8_t last);
static void store_keyboard(void);
static struct btd_hid_device *keyboard(void);
static void hand_connected(int to_hid, uint8_t status, uint16_t handle, uint8_t last);
static void hand_notice(int to_hid, uint16_t handle, uint8_t flags);
static void page_keyboard(void);
static void test_scan(void);
static void test_token(void);
static void test_hid_exits(void);
static void test_pair_exits(void);
static void test_hid_notices(void);
static void test_pair_notices(void);

/*
 * Runs every part and reports the checks.
 */
int
main(
	int argc,
	char **argv)
{
	/* The folder the script made. */
	keys_folder = "build/tmp";
	if (argc > 1)
		keys_folder = argv[1];

	/* The keyboard's bond and record, read by every run's HID host. */
	store_keyboard();

	/* Each part. */
	test_scan();
	test_token();
	test_hid_exits();
	test_pair_exits();
	test_hid_notices();
	test_pair_notices();

	/* The count of what failed. */
	printf("bt-linkuse-host-test: %u checks, %u failed\n", checks, failures);
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

/* Makes the run's world: a ready session on a socket pair with its controller's thread, the router with the pairing, the link manager and the HID host. */
static void
open_world(void)
{
	struct btd_hid_hooks hooks;
	struct btd_router_hid owner;
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

	/* The session, ready, with eight ACL buffers. */
	btd_session_init(&world.session, ends[0], NULL, NULL, "fake", "/nonexistent");
	world.session.timing.command_ms = 300U;
	world.session.state = BTD_STATE_READY;
	world.session.acl_pool.length = 1021U;
	world.session.acl_pool.total = 8U;
	world.session.acl_pool.free = 8U;
	world.session.le_shared = 1;

	/* The pairing, the router and the link manager, as the daemon has them. */
	btd_pair_init(&world.pair, &world.session, keys_folder, no_ask, pair_done, NULL, test_random, NULL);
	btd_router_init(&world.router, &world.pair);
	btd_linkmgr_init(&world.linkmgr, &world.session);
	btd_router_set_linkmgr(&world.router, &world.linkmgr);
	btd_pair_set_linkmgr(&world.pair, &world.linkmgr);
	world.session.handler = btd_router_handle;
	world.session.handler_context = &world.router;

	/* The HID host, the router's owner of its connections. */
	memset(&hooks, 0, sizeof(hooks));
	hooks.open_bridge = no_bridge;
	hooks.answer = no_answer;
	btd_hid_init(&world.hid, &world.session, keys_folder, &world.router, &hooks);
	owner.context = &world.hid;
	owner.wants = btd_hid_wants;
	owner.claims = btd_hid_claims;
	owner.handle = btd_hid_handle;
	btd_router_set_hid(&world.router, &owner);
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

/* The controller: records each command and answers it with a Command Status, until the session's end closes. */
static void *
controller_run(
	void *argument)
{
	struct controller *controller;
	uint8_t packet[300];
	uint8_t answer[7];
	uint16_t opcode;
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
		(void)pthread_mutex_lock(&controller->lock);
		if (controller->count < TEST_COMMANDS_MAX) {
			controller->opcodes[controller->count] = opcode;
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
	for (index = 0U; index < world.controller.count; index++) {
		if (world.controller.opcodes[index] == opcode)
			count++;
	}

	/* The record is the thread's again. */
	(void)pthread_mutex_unlock(&world.controller.lock);

	/* The count. */
	return count;
}

/* Makes the controller refuse an opcode from now on (0: none). */
static void
fail_opcode(
	uint16_t opcode)
{
	/* Under the lock. */
	(void)pthread_mutex_lock(&world.controller.lock);

	world.controller.fail_opcode = opcode;

	(void)pthread_mutex_unlock(&world.controller.lock);
}

/* The pairing's question hook: Just Works is never asked in this test. */
static void
no_ask(
	void *context,
	unsigned kind,
	uint32_t number)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(kind);
	UNUSED_PARAMETER(number);
}

/* The pairing's end hook: the line kept. */
static void
pair_done(
	void *context,
	const char *answer)
{
	UNUSED_PARAMETER(context);

	/* Kept. */
	(void)snprintf(world.answer, sizeof(world.answer), "%s", answer);
}

/* Random bytes for the pairing: a fixed pattern (BR/EDR does not use them). */
static void
test_random(
	void *context,
	uint8_t *bytes,
	size_t length)
{
	UNUSED_PARAMETER(context);

	/* The pattern. */
	memset(bytes, 0x5a, length);
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

/* The HID host's answers to a client: none waits in this test. */
static void
no_answer(
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

/* Stores the keyboard's bond and HID record (a BR/EDR device bluetoothd pages). */
static void
store_keyboard(void)
{
	static struct btd_hidcache record;
	struct btd_bond bond;
	uint8_t controller[BTD_ADDRESS_BYTES];
	int error;

	/* The bond, under the session's address (all zeros). */
	memset(controller, 0, sizeof(controller));
	memset(&bond, 0, sizeof(bond));
	make_address(bond.address, TEST_KEYBOARD);
	bond.type = BTD_ADDRESS_BREDR;
	bond.have_link_key = 1;
	memset(bond.link_key, 0x33, sizeof(bond.link_key));
	bond.link_key_type = 0x08U;
	bond.key_size = 16U;
	bond.authenticated = 1;
	bond.secure = 1;
	(void)snprintf(bond.name, sizeof(bond.name), "%s", "Keyboard");
	error = btd_keys_write(keys_folder, controller, &bond);
	check(error == 0, "setup: the keyboard's bond stored");

	/* Its record: a keyboard that does not connect by itself. */
	memset(&record, 0, sizeof(record));
	make_address(record.address, TEST_KEYBOARD);
	record.type = BTD_ADDRESS_BREDR;
	record.confirmed = 1;
	record.device_class = 0x002540U;
	(void)snprintf(record.name, sizeof(record.name), "%s", "Keyboard");
	error = btd_hidcache_write(keys_folder, controller, &record);
	check(error == 0, "setup: the keyboard's record stored");
}

/* Finds the keyboard in the HID host's table. */
static struct btd_hid_device *
keyboard(void)
{
	uint8_t address[BTD_ADDRESS_BYTES];
	unsigned index;
	int same;

	/* Each slot in use. */
	make_address(address, TEST_KEYBOARD);
	for (index = 0U; index < BTD_HID_MAX; index++) {
		if (!world.hid.devices[index].used)
			continue;
		same = memcmp(world.hid.devices[index].address, address, BTD_ADDRESS_BYTES);
		if (same == 0)
			return &world.hid.devices[index];
	}

	/* Not there: the test cannot go on. */
	printf("FAIL: the keyboard is not in the table\n");
	exit(1);
}

/* Hands a BR/EDR ACL Connection Complete to the HID host (to_hid) or the pairing. */
static void
hand_connected(
	int to_hid,
	uint8_t status,
	uint16_t handle,
	uint8_t last)
{
	uint8_t packet[14];

	/* Connection Complete: status, handle, the device, ACL, no encryption. */
	packet[0] = BT_PACKET_EVENT;
	packet[1] = 0x03U;
	packet[2] = 11U;
	packet[3] = status;
	packet[4] = (uint8_t)(handle & 0xffU);
	packet[5] = (uint8_t)(handle >> 8);
	make_address(packet + 6, last);
	packet[12] = 0x01U;
	packet[13] = 0x00U;
	if (to_hid) {
		btd_hid_handle(&world.hid, &world.session, packet, sizeof(packet));
		return;
	}

	/* The pairing's. */
	btd_pair_handle(&world.pair, &world.session, packet, sizeof(packet));
}

/* Hands a notice of dropped packets on a connection to the HID host (to_hid) or the pairing. */
static void
hand_notice(
	int to_hid,
	uint16_t handle,
	uint8_t flags)
{
	uint8_t notice[BTD_DROP_LENGTH];

	/* The notice: its type, the handle, the flags, the first data channel, the count. */
	notice[0] = BTD_PACKET_DROP;
	notice[1] = (uint8_t)(handle & 0xffU);
	notice[2] = (uint8_t)(handle >> 8);
	notice[3] = flags;
	notice[4] = 0x41U;
	notice[5] = 0x00U;
	notice[6] = 1U;
	notice[7] = 0U;
	if (to_hid) {
		btd_hid_handle(&world.hid, &world.session, notice, sizeof(notice));
		return;
	}

	/* The pairing's. */
	btd_pair_handle(&world.pair, &world.session, notice, sizeof(notice));
}

/* Reads the keyboard into the HID host and lets its page start. */
static void
page_keyboard(void)
{
	/* The table from the bonds, then the tick that pages. */
	btd_hid_refresh(&world.hid);
	btd_hid_tick(&world.hid, btd_now_ms());
}

/* The HID host's page scan, written by the link manager once. */
static void
test_scan(void)
{
	/* The refresh wants page scan. */
	open_world();
	btd_hid_refresh(&world.hid);
	check(commands_of(TEST_WRITE_SCAN) == 1U, "scan: Write Scan Enable written");
	check(world.linkmgr.scan_writes == 1U, "scan: by the link manager");
	check((world.linkmgr.scan_wanted & BTD_LINKMGR_HID) != 0U, "scan: the HID host's want");

	/* Another refresh does not write it again. */
	btd_hid_refresh(&world.hid);
	check(commands_of(TEST_WRITE_SCAN) == 1U, "scan: not written twice");
	close_world();
}

/* One BR/EDR page at a time between the HID host and the pairing. */
static void
test_token(void)
{
	struct btd_hid_device *device;
	uint8_t address[BTD_ADDRESS_BYTES];
	uint64_t before;
	unsigned retries;
	int error;

	/* The HID host's page holds the token. */
	open_world();
	page_keyboard();
	device = keyboard();
	check(device->state == BTD_HID_PAGING && commands_of(TEST_CREATE) == 1U, "token: the keyboard paged");
	check(world.linkmgr.paging && world.linkmgr.page_owner == BTD_LINKMGR_HID, "token: the page is the HID host's");

	/* A pairing meanwhile is busy, and sends nothing. */
	make_address(address, TEST_OTHER);
	error = btd_pair_start(&world.pair, address, BTD_ADDRESS_BREDR, 1, 0, 0);
	check(error == EBUSY, "token: a pairing while the HID host pages is busy");
	check(!btd_pair_active(&world.pair) && commands_of(TEST_CREATE) == 1U, "token: no pairing, no second page");
	close_world();

	/* The pairing's page holds the token. */
	open_world();
	make_address(address, TEST_OTHER);
	error = btd_pair_start(&world.pair, address, BTD_ADDRESS_BREDR, 1, 0, 0);
	check(error == 0 && world.linkmgr.page_owner == BTD_LINKMGR_PAIR, "token: the pairing's page");

	/* The HID host's page waits 2 s, not counted as a try. */
	btd_hid_refresh(&world.hid);
	device = keyboard();
	retries = device->retries;
	before = btd_now_ms();
	btd_hid_tick(&world.hid, before);
	check(device->state == BTD_HID_IDLE, "token: the keyboard waits");
	check(device->retry_at >= before + 1900U && device->retry_at <= btd_now_ms() + 2000U, "token: for 2 s");
	check(device->retries == retries, "token: not counted as a try");
	check(commands_of(TEST_CREATE) == 1U, "token: only the pairing's page sent");
	close_world();
}

/* Each way out of the HID host's page ends it at the link manager. */
static void
test_hid_exits(void)
{
	struct btd_hid_device *device;

	/* Made: Connection Complete. */
	open_world();
	page_keyboard();
	hand_connected(1, 0x00U, TEST_HANDLE, TEST_KEYBOARD);
	device = keyboard();
	check(device->state == BTD_HID_AUTHENTICATING && !world.linkmgr.paging, "hid exits: made, the page over");
	close_world();

	/* Failed: Connection Complete with a status. */
	open_world();
	page_keyboard();
	hand_connected(1, 0x04U, TEST_HANDLE, TEST_KEYBOARD);
	device = keyboard();
	check(device->state == BTD_HID_IDLE && !world.linkmgr.paging, "hid exits: failed, the page over");
	close_world();

	/* Its time: cancelled. */
	open_world();
	page_keyboard();
	btd_hid_tick(&world.hid, btd_now_ms() + BTD_HID_PAGE_MS + 1000U);
	check(commands_of(TEST_CREATE_CANCEL) == 1U && !world.linkmgr.paging, "hid exits: timed out, cancelled, the page over");
	close_world();

	/* The controller lost. */
	open_world();
	page_keyboard();
	btd_hid_lost(&world.hid);
	check(!world.linkmgr.paging, "hid exits: the controller lost, the page over");
	close_world();

	/* Create Connection refused. */
	open_world();
	fail_opcode(TEST_CREATE);
	page_keyboard();
	device = keyboard();
	check(commands_of(TEST_CREATE) == 1U && device->state == BTD_HID_IDLE && !world.linkmgr.paging, "hid exits: refused, the page over");
	close_world();
}

/* Each way out of the pairing's page ends it at the link manager. */
static void
test_pair_exits(void)
{
	uint8_t address[BTD_ADDRESS_BYTES];
	int error;

	/* Failed: Connection Complete with a status. */
	open_world();
	make_address(address, TEST_OTHER);
	error = btd_pair_start(&world.pair, address, BTD_ADDRESS_BREDR, 1, 0, 0);
	check(error == 0 && world.linkmgr.paging, "pair exits: the page out");
	hand_connected(0, 0x04U, TEST_PAIR_HANDLE, TEST_OTHER);
	check(!world.linkmgr.paging && strcmp(world.answer, "ERROR unreachable") == 0, "pair exits: failed, the page over");
	close_world();

	/* Made: Connection Complete. */
	open_world();
	error = btd_pair_start(&world.pair, address, BTD_ADDRESS_BREDR, 1, 0, 0);
	hand_connected(0, 0x00U, TEST_PAIR_HANDLE, TEST_OTHER);
	check(error == 0 && !world.linkmgr.paging && btd_pair_active(&world.pair), "pair exits: made, the page over");
	close_world();

	/* Its time: cancelled. */
	open_world();
	error = btd_pair_start(&world.pair, address, BTD_ADDRESS_BREDR, 1, 0, 0);
	btd_pair_tick(&world.pair, btd_now_ms() + BTD_PAIR_CONNECT_MS + 1000U);
	check(error == 0 && commands_of(TEST_CREATE_CANCEL) == 1U && !world.linkmgr.paging, "pair exits: timed out, cancelled, the page over");
	close_world();

	/* Create Connection refused. */
	open_world();
	fail_opcode(TEST_CREATE);
	error = btd_pair_start(&world.pair, address, BTD_ADDRESS_BREDR, 1, 0, 0);
	check(error == 0 && !world.linkmgr.paging && strcmp(world.answer, "ERROR unreachable") == 0, "pair exits: refused, the page over");
	close_world();
}

/* The HID host's notices: what ends a connection, what does not, and the frame dropped. */
static void
test_hid_notices(void)
{
	struct btd_hid_device *device;

	/* Lost events while the link waits for its authentication: made again. */
	open_world();
	page_keyboard();
	hand_connected(1, 0x00U, TEST_HANDLE, TEST_KEYBOARD);
	device = keyboard();
	device->reassembly.active = 1;
	hand_notice(1, TEST_HANDLE, BTD_DROP_EVENT);
	check(device->reassembly.active == 0, "hid notices: the frame being put together dropped");
	check(device->state == BTD_HID_CLOSING && commands_of(TEST_DISCONNECT) == 1U, "hid notices: lost events while authenticating end the link");
	check(device->last != NULL && strcmp(device->last, "lost-packets") == 0, "hid notices: why");
	close_world();

	/* An open link: lost events go on, lost data ends it. */
	open_world();
	page_keyboard();
	hand_connected(1, 0x00U, TEST_HANDLE, TEST_KEYBOARD);
	device = keyboard();
	device->state = BTD_HID_OPEN;
	hand_notice(1, TEST_HANDLE, BTD_DROP_EVENT);
	check(device->state == BTD_HID_OPEN && world.hid.notices == 1U, "hid notices: an open link goes on after lost events");
	hand_notice(1, TEST_HANDLE, BTD_DROP_DATA);
	check(device->state == BTD_HID_CLOSING && commands_of(TEST_DISCONNECT) == 1U, "hid notices: lost data ends an open link");
	close_world();

	/* Lost signalling, and a packet of no known channel, end it too. */
	open_world();
	page_keyboard();
	hand_connected(1, 0x00U, TEST_HANDLE, TEST_KEYBOARD);
	device = keyboard();
	device->state = BTD_HID_OPEN;
	hand_notice(1, TEST_HANDLE, BTD_DROP_SIGNAL);
	check(device->state == BTD_HID_CLOSING, "hid notices: lost signalling ends the link");
	close_world();
	open_world();
	page_keyboard();
	hand_connected(1, 0x00U, TEST_HANDLE, TEST_KEYBOARD);
	device = keyboard();
	device->state = BTD_HID_OPEN;
	hand_notice(1, TEST_HANDLE, BTD_DROP_UNKNOWN);
	check(device->state == BTD_HID_CLOSING, "hid notices: a lost packet of no known channel ends the link");

	/* A notice of a connection that is not the HID host's is counted. */
	hand_notice(1, 0x0077U, BTD_DROP_DATA);
	check(world.hid.notices == 1U, "hid notices: another connection's counted");
	close_world();
}

/* The pairing's notices: its connection's stop the pairing, another's are passed over. */
static void
test_pair_notices(void)
{
	uint8_t address[BTD_ADDRESS_BYTES];
	uint8_t ended[7];
	int error;

	/* A pairing whose connection is up. */
	open_world();
	make_address(address, TEST_OTHER);
	error = btd_pair_start(&world.pair, address, BTD_ADDRESS_BREDR, 1, 0, 0);
	hand_connected(0, 0x00U, TEST_PAIR_HANDLE, TEST_OTHER);
	check(error == 0 && btd_pair_active(&world.pair), "pair notices: the pairing's connection up");

	/* Another connection's notice is passed over. */
	hand_notice(0, 0x0077U, BTD_DROP_DATA);
	check(commands_of(TEST_DISCONNECT) == 0U, "pair notices: another connection's passed over");

	/* Its own: the frame dropped, the link ended, the end says why. */
	world.pair.reassembly.active = 1;
	hand_notice(0, TEST_PAIR_HANDLE, BTD_DROP_SIGNAL);
	check(world.pair.reassembly.active == 0, "pair notices: the frame dropped");
	check(commands_of(TEST_DISCONNECT) == 1U, "pair notices: the link ended");
	ended[0] = BT_PACKET_EVENT;
	ended[1] = 0x05U;
	ended[2] = 4U;
	ended[3] = 0x00U;
	ended[4] = (uint8_t)(TEST_PAIR_HANDLE & 0xffU);
	ended[5] = (uint8_t)(TEST_PAIR_HANDLE >> 8);
	ended[6] = 0x16U;
	btd_pair_handle(&world.pair, &world.session, ended, sizeof(ended));
	check(strcmp(world.answer, "ERROR lost-packets") == 0, "pair notices: ERROR lost-packets");
	close_world();
}
