/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of a phone's pairing (ws197-p002, plan/ws197/phase002/
 * phase.md sections 7.1, 7.2 and 12.1), built with the host's compiler
 * under ASan and UBSan.  The controller is a thread on a socket pair that
 * records each command and ACL packet and answers each command (Read
 * Encryption Key Size with a key of 16 bytes, any other with a Command
 * Status); the pairing's events and the device's ACL frames are handed to
 * the pairing directly, and the connection is set in the session's table.
 *
 *   held       a phone's pairing holds the device's SDP and RFCOMM
 *              channels Pending and refuses another PSM; an ordinary
 *              pairing refuses them all
 *   handoff    the phone link's hook gets the connection, the bond, the
 *              uid, the key's size, the scan's Class of Device and the
 *              pairing's channels; it moves them to its table (same local
 *              CIDs, still Pending) and answers them; PAIRED ends with
 *              phone=1, the HID host is not asked
 *   refused    a hook that does not take the connection: the held
 *              channels refused (no resources) before the HID host is
 *              asked, PAIRED ends with phone=0 why=...; without the phone
 *              link's hook why=unsupported; an ordinary pairing's line has
 *              no phone=
 *   keys       a phone's pairing refuses a stored key, Just Works or
 *              authenticated (Negative Reply, ws197-p003 N2); an ordinary
 *              pairing takes the stored key as before; the same uid's
 *              bond of this run (BTD_PAIR_PHONE_OWN, BUG-287) gives its
 *              authenticated key but not a Just Works one
 *   le         a phone's pairing of an LE address is refused (EINVAL)
 *
 *   plan/ws197/tests/bt-phone-host-test.sh KEYS_FOLDER
 */

#include "userland/base/bluetoothd/keys.h"
#include "userland/base/bluetoothd/l2cap.h"
#include "userland/base/bluetoothd/pair.h"
#include "userland/base/bluetoothd/session.h"

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

/* The commands and ACL packets the controller keeps, and the bytes it keeps of each. */
#define TEST_RECORDS_MAX	64U
#define TEST_BYTES_MAX		64U

/* The opcodes the test looks for. */
#define TEST_DISCONNECT		0x0406U
#define TEST_KEY_REPLY		0x040bU
#define TEST_KEY_NEGATIVE	0x040cU
#define TEST_KEY_SIZE		0x1408U

/* The connection's handle, the device's last address byte, and the uid that asks. */
#define TEST_HANDLE		0x0042U
#define TEST_DEVICE		0x31U
#define TEST_UID		1001U

/* The PSMs the device asks for: SDP, RFCOMM and HID's control (held by nobody during a pairing). */
#define TEST_PSM_SDP		0x0001U
#define TEST_PSM_RFCOMM		0x0003U
#define TEST_PSM_CONTROL	0x0011U

/* The device's source CIDs for those channels. */
#define TEST_CID_SDP		0x0070U
#define TEST_CID_RFCOMM		0x0071U
#define TEST_CID_CONTROL	0x0072U

/* The link key types the test stores: Just Works P-256, authenticated P-256. */
#define TEST_KEY_JUST_WORKS	0x07U
#define TEST_KEY_MITM		0x08U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/* The folder of the bonds (the script's new folder). */
static const char *keys_folder;

/*
 * The controller: its end of the socket pair, and what it was sent (under
 * the lock: the thread writes, the test reads): each command's opcode and
 * parameters, and each ACL packet's first bytes.
 */
struct controller {
	int descriptor;
	pthread_mutex_t lock;
	unsigned command_count;
	uint16_t opcodes[TEST_RECORDS_MAX];
	unsigned acl_count;
	uint8_t acl[TEST_RECORDS_MAX][TEST_BYTES_MAX];
};

/*
 * The hooks' stand-ins: the phone link's (whether it takes the connection,
 * its why otherwise, what it was handed and its own table of channels)
 * and the HID host's (whether it takes it, how often it was asked and how
 * many ACL packets the controller had then); and the pairing's end.
 */
struct stubs {
	int phone_takes;
	const char *phone_why;
	unsigned phone_calls;
	struct btd_pair_handoff handed;
	struct btd_bond bond;
	unsigned moved;
	unsigned left;
	struct btd_l2cap l2cap;
	int hid_takes;
	unsigned hid_calls;
	unsigned acl_at_hid;
	char answer[BTD_PAIR_ANSWER_MAX];
	unsigned asked;
};

/* A run's world: the controller and its thread, the session, the pairing and the stand-ins. */
struct world {
	struct controller controller;
	pthread_t thread;
	struct btd_session session;
	struct btd_pair pair;
	struct stubs stubs;
};

static void check(int condition, const char *what);
static int ends_with(const char *text, const char *suffix);
static void open_world(struct world *world);
static void close_world(struct world *world);
static void *controller_run(void *argument);
static unsigned commands_of(struct world *world, uint16_t opcode);
static unsigned acl_count(struct world *world);
static int wait_acl(struct world *world, unsigned count);
static int find_response(struct world *world, uint16_t source_cid, uint16_t *destination_cid, uint16_t *result);
static void hook_ask(void *context, unsigned kind, uint32_t number);
static void hook_done(void *context, const char *answer);
static void test_random(void *context, uint8_t *bytes, size_t length);
static int stub_phone(void *context, const struct btd_pair_handoff *handoff, const char **why);
static int stub_hid(void *context, const uint8_t *address, unsigned type, uint16_t handle, const struct btd_bond *bond);
static void make_address(uint8_t *address, uint8_t last);
static void hand_event(struct world *world, uint8_t code, const uint8_t *parameters, size_t length);
static void hand_address_event(struct world *world, uint8_t code, const uint8_t *more, size_t more_length);
static void hand_request(struct world *world, uint16_t psm, uint16_t source_cid);
static void store_key(uint8_t key_type);
static void forget_key(void);
static void run_to_key(struct world *world, int phone);
static void run_new_key(struct world *world, uint8_t peer_io, uint8_t key_type);
static void run_to_end(struct world *world);
static void test_handoff(void);
static void test_refused(void);
static void test_unsupported(void);
static void test_ordinary(void);
static void test_keys(void);
static void test_le(void);

/*
 * Runs every part and reports the checks.
 */
int
main(
	int argc,
	char **argv)
{
	/* The bonds' folder the script made. */
	keys_folder = "build/tmp";
	if (argc > 1)
		keys_folder = argv[1];

	/* Each part. */
	test_handoff();
	test_refused();
	test_unsupported();
	test_ordinary();
	test_keys();
	test_le();

	/* The count of what failed. */
	printf("bt-pair-phone-host-test: %u checks, %u failed\n", checks, failures);
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

/* Tells whether a text ends with a suffix. */
static int
ends_with(
	const char *text,
	const char *suffix)
{
	size_t text_length;
	size_t suffix_length;
	int same;

	/* Long enough at all. */
	text_length = strlen(text);
	suffix_length = strlen(suffix);
	if (text_length < suffix_length)
		return 0;

	/* The last bytes. */
	same = strcmp(text + text_length - suffix_length, suffix);
	if (same == 0)
		return 1;

	/* Another ending. */
	return 0;
}

/* Makes a run's world: a ready session with buffers on a socket pair with its controller's thread, and an idle pairing with the stand-ins' hooks. */
static void
open_world(
	struct world *world)
{
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

	/* The session, ready, with eight ACL buffers. */
	btd_session_init(&world->session, ends[0], NULL, NULL, "fake", "/nonexistent");
	world->session.timing.command_ms = 300U;
	world->session.state = BTD_STATE_READY;
	world->session.acl_pool.length = 1021U;
	world->session.acl_pool.total = 8U;
	world->session.acl_pool.free = 8U;
	world->session.le_shared = 1;

	/* The pairing, idle, with both hooks. */
	btd_pair_init(&world->pair, &world->session, keys_folder, hook_ask, hook_done, &world->stubs, test_random, NULL);
	btd_pair_set_handoff(&world->pair, stub_hid, world);
	btd_pair_set_phone_handoff(&world->pair, stub_phone, world);
	btd_l2cap_init(&world->stubs.l2cap);
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

/* The controller: records each command and ACL packet and answers each command, until the session's end closes. */
static void *
controller_run(
	void *argument)
{
	struct controller *controller;
	uint8_t packet[300];
	uint8_t answer[16];
	uint16_t opcode;
	size_t answer_length;
	size_t kept;
	ssize_t written;
	ssize_t got;

	/* Each packet. */
	controller = argument;
	for (;;) {
		got = read(controller->descriptor, packet, sizeof(packet));
		if (got <= 0)
			return NULL;

		/* An ACL packet: its first bytes recorded. */
		if (packet[0] == BT_PACKET_ACL) {
			kept = (size_t)got;
			if (kept > TEST_BYTES_MAX)
				kept = TEST_BYTES_MAX;
			(void)pthread_mutex_lock(&controller->lock);
			if (controller->acl_count < TEST_RECORDS_MAX) {
				memcpy(controller->acl[controller->acl_count], packet, kept);
				controller->acl_count++;
			}

			/* The record is the test's to read again. */
			(void)pthread_mutex_unlock(&controller->lock);
			continue;
		}

		/* A command at all. */
		if (packet[0] != 0x01U || got < 4)
			continue;

		/* Recorded. */
		opcode = (uint16_t)(packet[1] | (packet[2] << 8));
		(void)pthread_mutex_lock(&controller->lock);
		if (controller->command_count < TEST_RECORDS_MAX) {
			controller->opcodes[controller->command_count] = opcode;
			controller->command_count++;
		}

		/* The record is the test's to read again. */
		(void)pthread_mutex_unlock(&controller->lock);

		/* Read Encryption Key Size: Command Complete with status 0, the handle and 16 bytes. */
		if (opcode == TEST_KEY_SIZE) {
			answer[0] = BT_PACKET_EVENT;
			answer[1] = 0x0eU;
			answer[2] = 7U;
			answer[3] = 0x01U;
			answer[4] = (uint8_t)(opcode & 0xffU);
			answer[5] = (uint8_t)(opcode >> 8);
			answer[6] = 0x00U;
			answer[7] = (uint8_t)(TEST_HANDLE & 0xffU);
			answer[8] = (uint8_t)(TEST_HANDLE >> 8);
			answer[9] = 16U;
			answer_length = 10U;
		} else {
			/* Any other: Command Status with status 0, one command allowed, the opcode. */
			answer[0] = BT_PACKET_EVENT;
			answer[1] = 0x0fU;
			answer[2] = 4U;
			answer[3] = 0x00U;
			answer[4] = 0x01U;
			answer[5] = (uint8_t)(opcode & 0xffU);
			answer[6] = (uint8_t)(opcode >> 8);
			answer_length = 7U;
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
	struct world *world,
	uint16_t opcode)
{
	unsigned count;
	unsigned index;

	/* Each one recorded. */
	count = 0U;
	(void)pthread_mutex_lock(&world->controller.lock);
	for (index = 0U; index < world->controller.command_count; index++) {
		if (world->controller.opcodes[index] == opcode)
			count++;
	}

	/* The record is the thread's again. */
	(void)pthread_mutex_unlock(&world->controller.lock);

	/* The count. */
	return count;
}

/* Counts the ACL packets the controller was sent. */
static unsigned
acl_count(
	struct world *world)
{
	unsigned count;

	/* Under the lock. */
	(void)pthread_mutex_lock(&world->controller.lock);

	count = world->controller.acl_count;

	(void)pthread_mutex_unlock(&world->controller.lock);

	/* The count. */
	return count;
}

/* Waits up to a second for the controller to have read a number of ACL packets; reports whether it did. */
static int
wait_acl(
	struct world *world,
	unsigned count)
{
	struct timespec pause;
	unsigned tries;
	unsigned got;

	/* Every 10 ms. */
	pause.tv_sec = 0;
	pause.tv_nsec = 10000000L;
	for (tries = 0U; tries < 100U; tries++) {
		got = acl_count(world);
		if (got >= count)
			return 1;
		(void)nanosleep(&pause, NULL);
	}

	/* Not in time. */
	return 0;
}

/*
 * Finds the last Connection Response the controller was sent to a source
 * CID of the device: its destination CID (ours) and its result.  Reports
 * whether there was one.
 */
static int
find_response(
	struct world *world,
	uint16_t source_cid,
	uint16_t *destination_cid,
	uint16_t *result)
{
	const uint8_t *packet;
	const uint8_t *command;
	uint16_t cid;
	size_t end;
	size_t offset;
	size_t length;
	unsigned index;
	int found;

	/*
	 * Each ACL packet on the signalling channel (HCI's byte, ACL's 4,
	 * L2CAP's 4), and each command in it within the bytes kept: code,
	 * identifier, length; a Connection Response then has the destination
	 * CID, the source CID and the result.
	 */
	found = 0;
	(void)pthread_mutex_lock(&world->controller.lock);
	for (index = 0U; index < world->controller.acl_count; index++) {
		packet = world->controller.acl[index];
		cid = (uint16_t)(packet[7] | (packet[8] << 8));
		if (cid != BTD_CID_SIGNALLING)
			continue;

		/* The commands of the frame. */
		end = 9U + (size_t)(packet[5] | (packet[6] << 8));
		if (end > TEST_BYTES_MAX)
			end = TEST_BYTES_MAX;
		for (offset = 9U; offset + 12U <= end; offset += 4U + length) {
			command = packet + offset;
			length = (size_t)(command[2] | (command[3] << 8));
			if (command[0] != 0x03U)
				continue;
			cid = (uint16_t)(command[6] | (command[7] << 8));
			if (cid != source_cid)
				continue;

			/* The response's CID and result. */
			*destination_cid = (uint16_t)(command[4] | (command[5] << 8));
			*result = (uint16_t)(command[8] | (command[9] << 8));
			found = 1;
		}
	}

	/* The record is the thread's again. */
	(void)pthread_mutex_unlock(&world->controller.lock);

	/* Whether one was found. */
	return found;
}

/* The pairing's question hook: counted (the run answers yes). */
static void
hook_ask(
	void *context,
	unsigned kind,
	uint32_t number)
{
	struct stubs *stubs;

	UNUSED_PARAMETER(kind);
	UNUSED_PARAMETER(number);

	/* Counted. */
	stubs = context;
	stubs->asked++;
}

/* The pairing's end hook: the line kept. */
static void
hook_done(
	void *context,
	const char *answer)
{
	struct stubs *stubs;

	/* Kept. */
	stubs = context;
	(void)snprintf(stubs->answer, sizeof(stubs->answer), "%s", answer);
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

/*
 * The phone link's stand-in: keeps what it was handed; when told to take
 * the connection, moves the pairing's channels into its own table.
 */
static int
stub_phone(
	void *context,
	const struct btd_pair_handoff *handoff,
	const char **why)
{
	struct world *world;
	struct stubs *stubs;

	/* What it was handed (the bond copied: the pairing wipes its own). */
	world = context;
	stubs = &world->stubs;
	stubs->phone_calls++;
	stubs->handed = *handoff;
	stubs->bond = *handoff->bond;

	/* Not taken. */
	if (!stubs->phone_takes) {
		*why = stubs->phone_why;
		return 0;
	}

	/* Taken: the channels move to its table. */
	btd_l2cap_move(handoff->l2cap, &stubs->l2cap, handoff->handle, &stubs->moved, &stubs->left);

	/* Succeeded: the connection is its own. */
	return 1;
}

/* The HID host's stand-in: counts the call and how many ACL packets the controller had by then, and takes the connection when told to. */
static int
stub_hid(
	void *context,
	const uint8_t *address,
	unsigned type,
	uint16_t handle,
	const struct btd_bond *bond)
{
	struct world *world;

	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(type);
	UNUSED_PARAMETER(handle);
	UNUSED_PARAMETER(bond);

	/* Counted, after what the pairing sent before (a refusal goes first). */
	world = context;
	world->stubs.hid_calls++;
	(void)wait_acl(world, world->stubs.acl_at_hid);
	world->stubs.acl_at_hid = acl_count(world);

	/* Its answer. */
	if (world->stubs.hid_takes)
		return 1;

	/* Not taken. */
	return 0;
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

/* Hands the pairing an event. */
static void
hand_event(
	struct world *world,
	uint8_t code,
	const uint8_t *parameters,
	size_t length)
{
	uint8_t packet[64];

	/* The packet: event, code, length, the parameters. */
	packet[0] = BT_PACKET_EVENT;
	packet[1] = code;
	packet[2] = (uint8_t)length;
	memcpy(packet + 3, parameters, length);
	btd_pair_handle(&world->pair, &world->session, packet, 3U + length);
}

/* Hands the pairing an event led by the device's address. */
static void
hand_address_event(
	struct world *world,
	uint8_t code,
	const uint8_t *more,
	size_t more_length)
{
	uint8_t parameters[48];

	/* The address, then the rest. */
	make_address(parameters, TEST_DEVICE);
	if (more_length != 0U)
		memcpy(parameters + BTD_ADDRESS_BYTES, more, more_length);
	hand_event(world, code, parameters, BTD_ADDRESS_BYTES + more_length);
}

/* Hands the pairing the device's L2CAP Connection Request for a PSM. */
static void
hand_request(
	struct world *world,
	uint16_t psm,
	uint16_t source_cid)
{
	uint8_t command[8];
	uint8_t frame[16];
	uint8_t packet[32];
	size_t frame_length;
	size_t length;

	/* Connection Request: code, identifier, length 4, the PSM, the device's CID. */
	command[0] = 0x02U;
	command[1] = (uint8_t)(0x20U + (source_cid & 0x0fU));
	command[2] = 4U;
	command[3] = 0U;
	command[4] = (uint8_t)(psm & 0xffU);
	command[5] = (uint8_t)(psm >> 8);
	command[6] = (uint8_t)(source_cid & 0xffU);
	command[7] = (uint8_t)(source_cid >> 8);

	/* On the signalling channel, in one ACL packet. */
	frame_length = btd_l2cap_frame(frame, sizeof(frame), BTD_CID_SIGNALLING, command, sizeof(command));
	length = btd_acl_build(packet, sizeof(packet), TEST_HANDLE, BTD_ACL_FIRST_FLUSHABLE, frame, frame_length);
	btd_pair_handle(&world->pair, &world->session, packet, length);
}

/* Stores a bond of the device with a link key of a type. */
static void
store_key(
	uint8_t key_type)
{
	struct btd_bond bond;
	uint8_t controller[BTD_ADDRESS_BYTES];
	int error;

	/* The bond, under the session's address (all zeros). */
	memset(controller, 0, sizeof(controller));
	memset(&bond, 0, sizeof(bond));
	make_address(bond.address, TEST_DEVICE);
	bond.type = BTD_ADDRESS_BREDR;
	bond.have_link_key = 1;
	memset(bond.link_key, 0x11, sizeof(bond.link_key));
	bond.link_key_type = key_type;
	bond.key_size = 16U;
	if (key_type == TEST_KEY_MITM)
		bond.authenticated = 1;
	bond.secure = 1;
	error = btd_keys_write(keys_folder, controller, &bond);
	check(error == 0, "keys: the bond stored");
}

/* Forgets the device's bond (none may be there). */
static void
forget_key(void)
{
	uint8_t controller[BTD_ADDRESS_BYTES];
	uint8_t address[BTD_ADDRESS_BYTES];

	/* Under the session's address. */
	memset(controller, 0, sizeof(controller));
	make_address(address, TEST_DEVICE);
	(void)btd_keys_forget(keys_folder, controller, address, BTD_ADDRESS_BREDR);
}

/*
 * Starts a BR/EDR pairing of the device and takes it to the controller's
 * Link Key Request: the connection made and counted by the session, and
 * the device's SDP, RFCOMM and HID control channels asked for meanwhile.
 */
static void
run_to_key(
	struct world *world,
	int phone)
{
	uint8_t address[BTD_ADDRESS_BYTES];
	uint8_t connected[11];
	struct btd_link_count *link;
	int error;

	/* The pairing, asked by the uid. */
	make_address(address, TEST_DEVICE);
	error = btd_pair_start(&world->pair, address, BTD_ADDRESS_BREDR, 1, phone, TEST_UID);
	check(error == 0, "start: the pairing started");

	/* The session counts the connection. */
	link = &world->session.links[0];
	link->used = 1;
	link->handle = TEST_HANDLE;
	memcpy(link->address, address, BTD_ADDRESS_BYTES);
	link->frame_limit = BTD_SEND_FRAMES;

	/* Connection Complete: status 0, the handle, the device, ACL, no encryption. */
	connected[0] = 0x00U;
	connected[1] = (uint8_t)(TEST_HANDLE & 0xffU);
	connected[2] = (uint8_t)(TEST_HANDLE >> 8);
	memcpy(connected + 3, address, BTD_ADDRESS_BYTES);
	connected[9] = 0x01U;
	connected[10] = 0x00U;
	hand_event(world, 0x03U, connected, sizeof(connected));

	/* The device's channels, each answered on its own. */
	hand_request(world, TEST_PSM_SDP, TEST_CID_SDP);
	hand_request(world, TEST_PSM_RFCOMM, TEST_CID_RFCOMM);
	hand_request(world, TEST_PSM_CONTROL, TEST_CID_CONTROL);
	check(wait_acl(world, 3U), "start: the three channels answered");

	/* Link Key Request. */
	hand_address_event(world, 0x17U, NULL, 0U);
}

/*
 * Runs Secure Simple Pairing after a Negative Reply: the IO capabilities
 * (the device's given), the question answered yes, the new key of a type.
 */
static void
run_new_key(
	struct world *world,
	uint8_t peer_io,
	uint8_t key_type)
{
	uint8_t more[17];

	/* IO Capability Request, then the device's: its IO, no OOB, MITM with dedicated bonding. */
	hand_address_event(world, 0x31U, NULL, 0U);
	more[0] = peer_io;
	more[1] = 0x00U;
	more[2] = 0x03U;
	hand_address_event(world, 0x32U, more, 3U);

	/* User Confirmation Request with a number, answered yes. */
	more[0] = 0x40U;
	more[1] = 0xe2U;
	more[2] = 0x01U;
	more[3] = 0x00U;
	hand_address_event(world, 0x33U, more, 4U);
	btd_pair_answer(&world->pair, 1);

	/* Simple Pairing Complete, then Link Key Notification: the key and its type. */
	more[0] = 0x00U;
	make_address(more + 1, TEST_DEVICE);
	hand_event(world, 0x36U, more, 7U);
	memset(more, 0x22, 16U);
	more[16] = key_type;
	hand_address_event(world, 0x18U, more, 17U);
}

/*
 * Takes the pairing from its authentication to its end: Authentication
 * Complete, Encryption Change, the key's size, the probe's time, and the
 * Disconnection Complete of a connection nobody took.
 */
static void
run_to_end(
	struct world *world)
{
	uint8_t parameters[4];

	/* Authentication Complete: status 0, the handle. */
	parameters[0] = 0x00U;
	parameters[1] = (uint8_t)(TEST_HANDLE & 0xffU);
	parameters[2] = (uint8_t)(TEST_HANDLE >> 8);
	hand_event(world, 0x06U, parameters, 3U);

	/* Encryption Change: status 0, the handle, on; the key's size is read and the probe goes. */
	parameters[3] = 0x01U;
	hand_event(world, 0x08U, parameters, 4U);

	/* The probe's time passes: the pairing succeeds and offers the connection. */
	btd_pair_tick(&world->pair, btd_now_ms() + BTD_PAIR_PROBE_MS + 1000U);

	/* A connection nobody took is ended: its Disconnection Complete ends the pairing. */
	if (world->stubs.answer[0] != '\0')
		return;
	parameters[0] = 0x00U;
	parameters[1] = (uint8_t)(TEST_HANDLE & 0xffU);
	parameters[2] = (uint8_t)(TEST_HANDLE >> 8);
	parameters[3] = 0x16U;
	hand_event(world, 0x05U, parameters, 4U);
}

/* A phone's pairing whose connection the phone link takes. */
static void
test_handoff(void)
{
	struct world world;
	struct btd_channel *channel;
	struct btd_device *seen;
	uint8_t answer[BTD_SIGNAL_MAX];
	uint16_t sdp_cid;
	uint16_t rfcomm_cid;
	uint16_t control_cid;
	uint16_t result;
	size_t length;
	int found;
	int error;

	/* A pairing of a phone the last scan saw (Class of Device: Phone, smartphone). */
	forget_key();
	open_world(&world);
	world.stubs.phone_takes = 1;
	seen = &world.session.devices.entries[0];
	make_address(seen->address, TEST_DEVICE);
	seen->type = BTD_ADDRESS_BREDR;
	seen->has_class = 1;
	seen->class_of_device = 0x5a020cU;
	world.session.devices.count = 1U;
	run_to_key(&world, 1);

	/* SDP and RFCOMM held Pending, HID's control refused. */
	found = find_response(&world, TEST_CID_SDP, &sdp_cid, &result);
	check(found && result == BTD_L2CAP_PENDING && sdp_cid >= BTD_CID_DYNAMIC, "held: SDP Pending");
	found = find_response(&world, TEST_CID_RFCOMM, &rfcomm_cid, &result);
	check(found && result == BTD_L2CAP_PENDING && rfcomm_cid != sdp_cid, "held: RFCOMM Pending");
	found = find_response(&world, TEST_CID_CONTROL, &control_cid, &result);
	check(found && result == BTD_L2CAP_PSM_NOT_SUPPORTED, "held: another PSM not supported");

	/* No stored key: Negative Reply; then Numeric Comparison and an authenticated key. */
	check(commands_of(&world, TEST_KEY_NEGATIVE) == 1U, "handoff: no stored key, Negative Reply");
	run_new_key(&world, 0x01U, TEST_KEY_MITM);
	check(world.stubs.asked == 1U, "handoff: the number asked");
	run_to_end(&world);

	/* The phone link was handed the connection and what it needs. */
	check(world.stubs.phone_calls == 1U, "handoff: the phone link asked once");
	check(world.stubs.handed.handle == TEST_HANDLE, "handoff: the handle");
	check(world.stubs.handed.type == BTD_ADDRESS_BREDR, "handoff: the type");
	check(world.stubs.handed.uid == TEST_UID, "handoff: the uid that asked");
	check(world.stubs.handed.key_size == 16U, "handoff: the key's size");
	check(world.stubs.handed.have_class && world.stubs.handed.class_of_device == 0x5a020cU, "handoff: the scan's Class of Device");
	check(world.stubs.bond.have_link_key && world.stubs.bond.link_key_type == TEST_KEY_MITM, "handoff: the bond just stored");
	check(world.stubs.handed.l2cap == &world.pair.l2cap, "handoff: the pairing's channels");
	check(world.stubs.handed.reassembly == &world.pair.reassembly, "handoff: the pairing's frame");

	/* The channels moved with their CIDs, still Pending; the pairing keeps none. */
	check(world.stubs.moved == 2U && world.stubs.left == 0U, "handoff: both held channels moved");
	channel = btd_l2cap_channel(&world.stubs.l2cap, sdp_cid);
	check(channel != NULL && channel->state == BTD_CHANNEL_PENDING && channel->psm == TEST_PSM_SDP, "handoff: SDP found by its CID, Pending");
	channel = btd_l2cap_channel(&world.pair.l2cap, sdp_cid);
	check(channel == NULL, "handoff: the pairing's table let it go");

	/* The end: PAIRED with phone=1, the HID host not asked, the connection not ended. */
	check(strncmp(world.stubs.answer, "PAIRED ", 7U) == 0, "handoff: PAIRED");
	check(ends_with(world.stubs.answer, " phone=1"), "handoff: the line ends with phone=1");
	check(world.stubs.hid_calls == 0U, "handoff: the HID host not asked");
	check(commands_of(&world, TEST_DISCONNECT) == 0U, "handoff: the connection stays");

	/* The phone link answers the held channels: success under the same CIDs, its Configure Requests after. */
	error = btd_l2cap_answer_pending(&world.stubs.l2cap, TEST_HANDLE, BTD_L2CAP_SUCCESS, answer, sizeof(answer), &length);
	check(error == 0 && length >= 2U * 12U, "handoff: the responses and their Configure Requests");
	check(answer[0] == 0x03U && (uint16_t)(answer[4] | (answer[5] << 8)) == sdp_cid, "handoff: SDP's success under its CID");
	check((uint16_t)(answer[8] | (answer[9] << 8)) == BTD_L2CAP_SUCCESS, "handoff: the result success");
	check(answer[12] == 0x04U, "handoff: its configuration follows");
	channel = btd_l2cap_channel(&world.stubs.l2cap, rfcomm_cid);
	check(channel != NULL && channel->state == BTD_CHANNEL_CONFIGURING, "handoff: RFCOMM configuring");
	close_world(&world);
}

/* A phone's pairing whose connection the phone link does not take: the held channels refused, then the HID host. */
static void
test_refused(void)
{
	struct world world;
	uint16_t cid;
	uint16_t result;
	int found;

	/* A device the phone link does not take; the HID host does. */
	forget_key();
	open_world(&world);
	world.stubs.phone_why = "not-phone";
	world.stubs.hid_takes = 1;
	run_to_key(&world, 1);
	run_new_key(&world, 0x01U, TEST_KEY_MITM);
	world.stubs.acl_at_hid = acl_count(&world) + 2U;
	run_to_end(&world);

	/* The scan did not see it: no class. */
	check(world.stubs.phone_calls == 1U, "refused: the phone link asked");
	check(!world.stubs.handed.have_class, "refused: no class without the scan");

	/* The held channels refused with no resources, before the HID host was asked. */
	found = find_response(&world, TEST_CID_SDP, &cid, &result);
	check(found && result == BTD_L2CAP_NO_RESOURCES && cid == 0U, "refused: SDP refused, no resources");
	found = find_response(&world, TEST_CID_RFCOMM, &cid, &result);
	check(found && result == BTD_L2CAP_NO_RESOURCES, "refused: RFCOMM refused");
	check(world.stubs.hid_calls == 1U, "refused: then the HID host asked");
	check(world.stubs.acl_at_hid == 5U, "refused: the probe and the refusals sent before the HID host was asked");

	/* PAIRED with phone=0 and why. */
	check(ends_with(world.stubs.answer, " phone=0 why=not-phone"), "refused: the line ends with phone=0 why=not-phone");
	close_world(&world);
}

/* A phone's pairing without the phone link's hook. */
static void
test_unsupported(void)
{
	struct world world;
	uint16_t cid;
	uint16_t result;
	int found;

	/* No phone link; the HID host does not take it either. */
	forget_key();
	open_world(&world);
	btd_pair_set_phone_handoff(&world.pair, NULL, NULL);
	run_to_key(&world, 1);
	run_new_key(&world, 0x01U, TEST_KEY_MITM);
	run_to_end(&world);

	/* The held channels refused, the line says why, the connection ended. */
	found = find_response(&world, TEST_CID_SDP, &cid, &result);
	check(found && result == BTD_L2CAP_NO_RESOURCES, "unsupported: SDP refused");
	check(ends_with(world.stubs.answer, " phone=0 why=unsupported"), "unsupported: the line ends with phone=0 why=unsupported");
	check(world.stubs.hid_calls == 1U, "unsupported: the HID host asked");
	check(commands_of(&world, TEST_DISCONNECT) == 1U, "unsupported: the connection ended");
	close_world(&world);
}

/* An ordinary pairing: no channel held, no phone link asked, no phone= on the line. */
static void
test_ordinary(void)
{
	struct world world;
	uint16_t cid;
	uint16_t result;
	int found;

	/* A pairing not for a phone. */
	forget_key();
	open_world(&world);
	run_to_key(&world, 0);

	/* Every channel refused as before. */
	found = find_response(&world, TEST_CID_SDP, &cid, &result);
	check(found && result == BTD_L2CAP_PSM_NOT_SUPPORTED, "ordinary: SDP not supported");
	found = find_response(&world, TEST_CID_RFCOMM, &cid, &result);
	check(found && result == BTD_L2CAP_PSM_NOT_SUPPORTED, "ordinary: RFCOMM not supported");

	/* The pairing to its end. */
	run_new_key(&world, 0x01U, TEST_KEY_MITM);
	run_to_end(&world);
	check(world.stubs.phone_calls == 0U, "ordinary: the phone link not asked");
	check(world.stubs.hid_calls == 1U, "ordinary: the HID host asked");
	check(strncmp(world.stubs.answer, "PAIRED ", 7U) == 0, "ordinary: PAIRED");
	check(strstr(world.stubs.answer, "phone=") == NULL, "ordinary: no phone= on the line");
	close_world(&world);
}

/* The stored key of a phone's pairing: never taken (ws197-p003 N2); an ordinary pairing takes it. */
static void
test_keys(void)
{
	struct world world;

	/* A phone's pairing with a stored Just Works key: Negative Reply. */
	store_key(TEST_KEY_JUST_WORKS);
	open_world(&world);
	run_to_key(&world, 1);
	check(commands_of(&world, TEST_KEY_NEGATIVE) == 1U, "keys: a phone's pairing refuses a stored Just Works key");
	check(commands_of(&world, TEST_KEY_REPLY) == 0U, "keys: the Just Works key not given");
	btd_pair_stop(&world.pair, "test");
	close_world(&world);

	/* An ordinary pairing with the same key: Link Key Request Reply, as before. */
	open_world(&world);
	run_to_key(&world, 0);
	check(commands_of(&world, TEST_KEY_REPLY) == 1U, "keys: an ordinary pairing takes the stored key");
	btd_pair_stop(&world.pair, "test");
	close_world(&world);

	/* A phone's pairing with a stored authenticated key: Negative Reply too, the numbers compared anew (ws197-p003 section 3.2, review-2 N2). */
	store_key(TEST_KEY_MITM);
	open_world(&world);
	run_to_key(&world, 1);
	check(commands_of(&world, TEST_KEY_NEGATIVE) == 1U, "keys: a phone's pairing refuses a stored authenticated key too");
	check(commands_of(&world, TEST_KEY_REPLY) == 0U, "keys: the authenticated key not given");
	btd_pair_stop(&world.pair, "test");
	close_world(&world);

	/* The same uid's bond of this run (BTD_PAIR_PHONE_OWN, BUG-287): its authenticated key is given, the phone is not asked to pair again. */
	open_world(&world);
	run_to_key(&world, BTD_PAIR_PHONE_OWN);
	check(commands_of(&world, TEST_KEY_REPLY) == 1U, "keys: the own bond's authenticated key is given");
	check(commands_of(&world, TEST_KEY_NEGATIVE) == 0U, "keys: no Negative Reply for the own bond");
	check(world.pair.phone == 1 && world.pair.phone_own == 1, "keys: the pairing is a phone's, of the own bond");
	btd_pair_stop(&world.pair, "test");
	close_world(&world);

	/* The own bond's Just Works key is still not taken. */
	store_key(TEST_KEY_JUST_WORKS);
	open_world(&world);
	run_to_key(&world, BTD_PAIR_PHONE_OWN);
	check(commands_of(&world, TEST_KEY_NEGATIVE) == 1U, "keys: the own bond's Just Works key is refused");
	check(commands_of(&world, TEST_KEY_REPLY) == 0U, "keys: the own bond's Just Works key not given");
	btd_pair_stop(&world.pair, "test");
	close_world(&world);
	forget_key();
}

/* A phone's pairing of an LE address is refused before it starts. */
static void
test_le(void)
{
	struct world world;
	uint8_t address[BTD_ADDRESS_BYTES];
	int active;
	int error;

	/* LE public and random. */
	open_world(&world);
	make_address(address, TEST_DEVICE);
	error = btd_pair_start(&world.pair, address, BTD_ADDRESS_LE_PUBLIC, 1, 1, TEST_UID);
	check(error == EINVAL, "le: a phone's pairing of an LE public address refused");
	error = btd_pair_start(&world.pair, address, BTD_ADDRESS_LE_RANDOM, 1, 1, TEST_UID);
	check(error == EINVAL, "le: a phone's pairing of an LE random address refused");
	active = btd_pair_active(&world.pair);
	check(!active, "le: no pairing runs");
	close_world(&world);
}
