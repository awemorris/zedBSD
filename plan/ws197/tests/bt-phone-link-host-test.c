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
 *   profile    ws197-p003 section 5.8: the profile told the link is ready
 *              while its owner sits at the seat, its SDP query (the MAS
 *              record's channel 5), its DLC opened on bluetoothd's RFCOMM
 *              session, and the link's end
 *   lifecycle  ws197-p003 section 5 with a controller that answers Create
 *              Connection Cancel, Authentication Requested and Read
 *              Encryption Key Size as the test sets: bluetoothd's page to
 *              a ready link (authentication, encryption, a key of 16), a
 *              short key; the phone's own connection (its encryption
 *              first, its authentication colliding with bluetoothd's: 0x0C,
 *              the wait for it running out); the crossing of a page and
 *              the phone's connection (Core 7.1.7: Cancel, Accept, the
 *              cancelled page's 0x02 passed over; and a page that made its
 *              link already: Reject and its own Connection Complete
 *              passed over, Core 7.1.9); a Connection Request while ready
 *              refused; the page's guard (Cancel); the stored key on a
 *              ready link; Key Missing stopping the pages; the reasons of
 *              a disconnection (0x08 pages again after the next step, a
 *              short link moves the step, two minutes ready start it over,
 *              three short 0x13 stop the pages); the owner leaving (a link
 *              ended, a page cancelled and its link ended when it comes);
 *              the HID host's six links holding the page
 *   notices    lost RFCOMM data closes its channel alone, lost events are
 *              counted, lost signalling ends the link; the link's end
 *              gives the HID host its links back
 *   snoop      a phone's ACL packet hidden: the headers kept, the rest zero
 *   records    ws197-p003 section 3: the handoff writes the owner's record
 *              (another phone's valid record and another owner's refuse,
 *              an invalid one goes), the pairing's check (owned,
 *              phone-seat, root), PHONE LINK, PHONE SHOW, FORGET, and the
 *              load (a record without its bond goes, two valid ones are
 *              neither used); the HID host's limit follows the record
 *
 * Each world has a new folder of bonds and records under the folder the
 * script gives.
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/hid.h"
#include "userland/base/bluetoothd/keys.h"
#include "userland/base/bluetoothd/l2cap.h"
#include "userland/base/bluetoothd/obex.h"
#include "userland/base/bluetoothd/pair.h"
#include "userland/base/bluetoothd/phone.h"
#include "userland/base/bluetoothd/phonerec.h"
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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* The commands and ACL packets the controller keeps, and the bytes it keeps of an ACL packet. */
#define TEST_RECORDS_MAX	256U
#define TEST_ACL_BYTES		1100U

/*
 * The opcodes the test looks for: Create Connection, Disconnect, Create
 * Connection Cancel, Accept and Reject Connection Request, Link Key
 * Request Reply, Authentication Requested, Set Connection Encryption, Read
 * Encryption Key Size.
 */
#define TEST_CREATE		0x0405U
#define TEST_DISCONNECT		0x0406U
#define TEST_CANCEL		0x0408U
#define TEST_ACCEPT		0x0409U
#define TEST_REJECT		0x040aU
#define TEST_KEY_REPLY		0x040bU
#define TEST_AUTHENTICATE	0x0411U
#define TEST_ENCRYPT		0x0413U
#define TEST_KEY_SIZE		0x1408U

/* The events the test gives: Connection Complete, Connection Request, Disconnection Complete, Authentication Complete, Encryption Change, Link Key Request. */
#define TEST_EVENT_CONNECTED	0x03U
#define TEST_EVENT_REQUEST	0x04U
#define TEST_EVENT_DISCONNECTED	0x05U
#define TEST_EVENT_AUTHENTICATED	0x06U
#define TEST_EVENT_ENCRYPTION	0x08U
#define TEST_EVENT_KEY_REQUEST	0x17U

/* The MAS service class the profile asks for. */
#define TEST_MAS_CLASS		0x1132U

/* The phone's link, its device's last address byte, and the uid that paired it. */
#define TEST_HANDLE		0x002aU
#define TEST_PHONE		0x51U
#define TEST_UID		1001U

/* Another account's uid, the controller's last address byte, and another phone's. */
#define TEST_OTHER_UID		1002U
#define TEST_CONTROLLER		0x10U
#define TEST_SECOND_PHONE	0x61U

/* The phone's MAS server channel. */
#define TEST_MAS_CHANNEL	5U

/* The link key types: authenticated P-256, Just Works P-256. */
#define TEST_KEY_MITM		0x08U
#define TEST_KEY_JUST_WORKS	0x07U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/*
 * The controller: its end of the socket pair, what it answers (the
 * encryption key's size, Create Connection Cancel's and Authentication
 * Requested's statuses: set by the test before the command), and what it
 * was sent (under the lock: the thread writes, the test takes): the
 * commands' opcodes, and the ACL packets not taken yet (a ring).
 */
struct controller {
	int descriptor;
	pthread_mutex_t lock;
	uint8_t key_size;
	uint8_t cancel_status;
	uint8_t auth_status;
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

/*
 * What the profile heard (ws197-p003 section 5.8): the link ready and
 * ended, its SDP query's end and the MAS channel found, its DLC opened and
 * closed, and DLCs that could not open.
 */
struct heard {
	unsigned ready;
	unsigned ended;
	unsigned sdp_done;
	int sdp_error;
	unsigned channel;
	unsigned opened;
	unsigned dlci;
	unsigned closed;
	unsigned open_failed;
};

/* The run's world: the controller and its thread, the session, the pairing, the router, the HID host, the phone link, the phone, and what the profile heard. */
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
	struct heard heard;
};

/* The run's world, one at a time; static for its size. */
static struct world world;

/* The folder the script gives, and the world's own folder of bonds and records under it. */
static const char *base_folder;
static char folder[512];

/* Whether the handoff finds TEST_UID at the seat (a pairing's owner is; 0 tells the link ends without the owner, Q14). */
static int hand_seated = 1;

/* The name of TEST_UID's account the hook gives (changed to tell a reused uid). */
static char account_name[BTD_PHONEREC_USER_MAX];

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
static void heard_ready(void *context);
static void heard_ended(void *context);
static void heard_sdp(void *context, const struct btd_sdp *sdp, int error);
static void heard_opened(void *context, unsigned dlci);
static void heard_closed(void *context, unsigned dlci, int reason);
static void heard_failed(void *context, unsigned server_channel);
static void give_event(uint8_t code, const uint8_t *parameters, size_t length);
static void give_connected(uint8_t status, uint16_t handle, uint8_t last, uint8_t encrypted);
static void give_request(uint8_t last);
static void give_handle_event(uint8_t code, uint8_t status, uint16_t handle, uint8_t extra, int with_extra);
static void give_key_request(uint8_t last);
static void present_world(void);
static void page_to_ready(uint64_t now);
static int no_bridge(void *context, int *descriptor);
static void no_told(void *context, const uint8_t *address, const char *line);
static void make_address(uint8_t *address, uint8_t last);
static size_t mas_record(uint8_t *bytes, size_t size);
static int hand_over(uint8_t key_type, unsigned key_size, int have_class, uint32_t class_of_device, const char **why);
static void hand_notice(uint16_t handle, uint8_t flags, uint16_t cid);
static void test_refusals(void);
static void test_taken(void);
static void test_profile(void);
static void test_page(void);
static void test_inbound(void);
static void test_crossing(void);
static void test_guard_and_keys(void);
static void test_reasons(void);
static void test_absent(void);
static void test_notices(void);
static void test_snoop(void);
static void test_records(void);
static int hook_account(void *context, uid_t uid, char *name, size_t size);
static void write_bond(uint8_t last, uint8_t key_type);
static void write_record(uint8_t last, uid_t uid, const char *user, int enabled);

/*
 * Runs every part and reports the checks.
 */
int
main(
	int argc,
	char **argv)
{
	/* The folder for the bonds and records. */
	if (argc < 2) {
		fprintf(stderr, "usage: bt-phone-link-host-test FOLDER\n");
		return 2;
	}

	/* Kept for each world. */
	base_folder = argv[1];

	/* Each part. */
	test_refusals();
	test_taken();
	test_profile();
	test_page();
	test_inbound();
	test_crossing();
	test_guard_and_keys();
	test_reasons();
	test_absent();
	test_notices();
	test_snoop();
	test_records();

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
	struct btd_phone_hooks phone_hooks;
	struct btd_phone_profile profile;
	struct btd_router_phone owner;
	struct btd_link_count *link;
	char *made;
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
	world.controller.key_size = 16U;
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

	/* The world's own folder of bonds and records. */
	(void)snprintf(folder, sizeof(folder), "%s/phone.XXXXXX", base_folder);
	made = mkdtemp(folder);
	if (made == NULL) {
		perror("mkdtemp");
		exit(2);
	}

	/* The controller's address, which names the records' folder, and TEST_UID's account. */
	world.session.have_address = 1;
	make_address(world.session.address, TEST_CONTROLLER);
	(void)snprintf(account_name, sizeof(account_name), "%s", "tester");

	/* The phone link, the router's owner of the phone's link. */
	btd_sdps_db_init(&world.records);
	phone_hooks.context = NULL;
	phone_hooks.account = hook_account;
	btd_phone_init(&world.phone, &world.session, &world.router, &world.hid, &world.records, folder, &phone_hooks);

	/* The profile: what it hears is kept. */
	memset(&profile, 0, sizeof(profile));
	profile.ready = heard_ready;
	profile.ended = heard_ended;
	profile.sdp_done = heard_sdp;
	profile.opened = heard_opened;
	profile.closed = heard_closed;
	profile.open_failed = heard_failed;
	btd_phone_set_profile(&world.phone, &profile);
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
	uint8_t answer[16];
	uint8_t key_size;
	uint8_t cancel_status;
	uint8_t auth_status;
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
			/* A command: recorded, with what it is to be answered. */
			opcode = (uint16_t)(packet[1] | (packet[2] << 8));
			(void)pthread_mutex_lock(&controller->lock);
			if (controller->command_count < TEST_RECORDS_MAX) {
				controller->opcodes[controller->command_count] = opcode;
				controller->command_count++;
			}
			key_size = controller->key_size;
			cancel_status = controller->cancel_status;
			auth_status = controller->auth_status;

			/* The record is the test's again; the answer: a Command Status, or a Command Complete of the commands that have one. */
			(void)pthread_mutex_unlock(&controller->lock);
			answer[0] = BT_PACKET_EVENT;
			answer[1] = 0x0fU;
			answer[2] = 4U;
			answer[3] = 0x00U;
			answer[4] = 0x01U;
			answer[5] = (uint8_t)(opcode & 0xffU);
			answer[6] = (uint8_t)(opcode >> 8);
			answer_length = 7U;
			if (opcode == TEST_AUTHENTICATE)
				answer[3] = auth_status;
			if (opcode == TEST_KEY_SIZE && got >= 6) {
				/* Read Encryption Key Size: the status, the handle, the size. */
				answer[1] = 0x0eU;
				answer[2] = 7U;
				answer[3] = 0x01U;
				answer[4] = (uint8_t)(opcode & 0xffU);
				answer[5] = (uint8_t)(opcode >> 8);
				answer[6] = 0x00U;
				answer[7] = packet[4];
				answer[8] = packet[5];
				answer[9] = key_size;
				answer_length = 10U;
			}
			if (opcode == TEST_CANCEL && got >= 10) {
				/* Create Connection Cancel: the status, the address. */
				answer[1] = 0x0eU;
				answer[2] = 10U;
				answer[3] = 0x01U;
				answer[4] = (uint8_t)(opcode & 0xffU);
				answer[5] = (uint8_t)(opcode >> 8);
				answer[6] = cancel_status;
				memcpy(answer + 7, packet + 4, 6U);
				answer_length = 13U;
			}
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
	(void)btd_keys_write(folder, world.session.address, &bond);

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
	btd_phone_set_seat(&world.phone, hand_seated, TEST_UID, btd_now_ms());
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

/* The profile told the link is ready, its SDP query, its DLC, and the link's end. */
static void
test_profile(void)
{
	const char *why;
	int error;

	/* A handed-over link whose owner is not at the seat ends at once (Q14), the profile not told. */
	open_world();
	hand_seated = 0;
	(void)hand_over(TEST_KEY_MITM, 16U, 1, 0x5a020cU, &why);
	hand_seated = 1;
	check(world.phone.state == BTD_PHONE_CLOSING && commands_of(TEST_DISCONNECT) == 1U && world.heard.ready == 0U, "profile: the owner away, the link ended");
	close_world();

	/* The owner at the seat: told at the handoff, once. */
	open_world();
	(void)hand_over(TEST_KEY_MITM, 16U, 1, 0x5a020cU, &why);
	exchange();
	check(world.phone.state == BTD_PHONE_READY && world.phone.present && world.heard.ready == 1U, "profile: told at the handoff");
	btd_phone_set_seat(&world.phone, 1, TEST_UID, btd_now_ms());
	check(world.heard.ready == 1U, "profile: told once");

	/* Its SDP query. */
	error = btd_phone_sdp_query(&world.phone, TEST_MAS_CLASS);
	check(error == 0, "profile: a query on the ready link");
	error = btd_phone_sdp_query(&world.phone, TEST_MAS_CLASS);
	check(error == EBUSY, "profile: one query at a time");
	exchange();
	check(world.heard.sdp_done == 1U && world.heard.sdp_error == 0 && world.heard.channel == TEST_MAS_CHANNEL, "profile: the MAS record's channel found");

	/* A DLC to the MAS channel: the RFCOMM session made, the DLC opened. */
	error = btd_phone_dlc_open(&world.phone, TEST_MAS_CHANNEL, btd_now_ms());
	check(error == 0, "profile: the DLC asked for");
	exchange();
	check(world.phone.rfcomm_active && world.fake.rfcomm_active, "profile: the RFCOMM session up");
	check(world.heard.opened == 1U && world.heard.dlci == 2U * TEST_MAS_CHANNEL, "profile: the DLC opened");

	/* The owner leaves: the link ends, the profile hears its DLC and the end. */
	btd_phone_set_seat(&world.phone, 0, 0, btd_now_ms());
	check(world.phone.state == BTD_PHONE_CLOSING && commands_of(TEST_DISCONNECT) == 1U, "profile: the owner gone, the link ended");
	give_handle_event(TEST_EVENT_DISCONNECTED, 0x00U, TEST_HANDLE, 0x16U, 1);
	check(world.phone.state == BTD_PHONE_NONE && world.heard.ended == 1U && world.heard.closed >= 1U, "profile: the end heard");
	check(world.phone.stopped && strcmp(world.phone.why, "absent") == 0, "profile: no page while the owner is away");
	error = btd_phone_dlc_open(&world.phone, TEST_MAS_CHANNEL, btd_now_ms());
	check(error == ENOTCONN, "profile: no DLC without a link");
	close_world();
}

/* Bluetoothd's page to a ready link, and a short key. */
static void
test_page(void)
{
	uint64_t now;

	/* The owner at the seat: a page at once. */
	present_world();
	now = btd_now_ms();
	btd_phone_tick(&world.phone, now);
	check(world.phone.state == BTD_PHONE_PAGING && commands_of(TEST_CREATE) == 1U && world.phone.page_outstanding, "page: Create Connection");

	/* Its Connection Complete: authentication asked for at once. */
	give_connected(0x00U, TEST_HANDLE, TEST_PHONE, 0U);
	check(world.phone.state == BTD_PHONE_SECURING && world.phone.secure_step == BTD_PHONE_SECURE_AUTH, "page: securing, authentication first");
	check(commands_of(TEST_AUTHENTICATE) == 1U && !world.phone.page_outstanding, "page: Authentication Requested, the page over");
	check(btd_router_owner(&world.router, TEST_HANDLE) == BTD_OWNER_PHONE, "page: the route is the phone link's");

	/* The phone's Link Key Request: the bond's key. */
	give_key_request(TEST_PHONE);
	check(commands_of(TEST_KEY_REPLY) == 1U, "page: the stored key given");

	/* Authenticated, then encrypted, then a key of 16: ready, the profile told. */
	give_handle_event(TEST_EVENT_AUTHENTICATED, 0x00U, TEST_HANDLE, 0U, 0);
	check(world.phone.secure_step == BTD_PHONE_SECURE_ENCRYPT && commands_of(TEST_ENCRYPT) == 1U, "page: encryption asked for");
	give_handle_event(TEST_EVENT_ENCRYPTION, 0x00U, TEST_HANDLE, 0x01U, 1);
	check(commands_of(TEST_KEY_SIZE) == 1U && world.phone.key_size == 16U, "page: the key's size read");
	check(world.phone.state == BTD_PHONE_READY && world.heard.ready == 1U, "page: ready, the profile told");

	/* The ready link's Link Key Request (the phone authenticates again): the key once more. */
	give_key_request(TEST_PHONE);
	check(commands_of(TEST_KEY_REPLY) == 2U, "page: the key given on a ready link");
	close_world();

	/* A key of 7: the link ended, the longest wait. */
	present_world();
	now = btd_now_ms();
	world.controller.key_size = 7U;
	btd_phone_tick(&world.phone, now);
	give_connected(0x00U, TEST_HANDLE, TEST_PHONE, 0U);
	give_handle_event(TEST_EVENT_AUTHENTICATED, 0x00U, TEST_HANDLE, 0U, 0);
	give_handle_event(TEST_EVENT_ENCRYPTION, 0x00U, TEST_HANDLE, 0x01U, 1);
	check(world.phone.state == BTD_PHONE_CLOSING && strcmp(world.phone.why, "key-size") == 0, "page: a short key ends the link");
	give_handle_event(TEST_EVENT_DISCONNECTED, 0x00U, TEST_HANDLE, 0x16U, 1);
	check(world.phone.state == BTD_PHONE_NONE && world.phone.backoff_step == BTD_PHONE_BACKOFF_STEPS - 1U, "page: the longest wait after a short key");
	check(world.heard.ready == 0U, "page: the profile never told");
	close_world();

	/* The HID host with six links: no page, a short wait. */
	present_world();
	for (now = 0U; now < BTD_HID_MAX; now++) {
		world.hid.devices[now].used = 1;
		world.hid.devices[now].state = BTD_HID_OPEN;
	}
	now = btd_now_ms();
	btd_phone_tick(&world.phone, now);
	check(world.phone.state == BTD_PHONE_NONE && commands_of(TEST_CREATE) == 0U && strcmp(world.phone.why, "busy-links") == 0, "page: held while the HID host uses six links");
	check(world.phone.next_page_at == now + BTD_PHONE_WAIT_MS, "page: tried again after a short wait");
	close_world();
}

/* The phone's own connection: its encryption first, its authentication colliding with bluetoothd's, the wait for it running out. */
static void
test_inbound(void)
{
	uint64_t now;

	/* Accepted with no role switch; its Connection Complete waits for the phone's own encryption. */
	present_world();
	give_request(TEST_PHONE);
	check(world.phone.state == BTD_PHONE_ACCEPTING && commands_of(TEST_ACCEPT) == 1U && world.phone.inbound, "inbound: accepted");
	give_connected(0x00U, TEST_HANDLE, TEST_PHONE, 0U);
	check(world.phone.state == BTD_PHONE_SECURING && world.phone.secure_step == BTD_PHONE_SECURE_WAIT_PEER, "inbound: waiting for the phone's encryption");
	check(commands_of(TEST_AUTHENTICATE) == 0U, "inbound: no authentication asked yet");

	/* The phone encrypts: ready without bluetoothd's authentication. */
	give_handle_event(TEST_EVENT_ENCRYPTION, 0x00U, TEST_HANDLE, 0x01U, 1);
	check(world.phone.state == BTD_PHONE_READY && commands_of(TEST_AUTHENTICATE) == 0U, "inbound: ready on the phone's encryption");
	close_world();

	/* The wait runs out: authentication asked; Command Disallowed (the phone began) waits again; then its encryption. */
	present_world();
	give_request(TEST_PHONE);
	give_connected(0x00U, TEST_HANDLE, TEST_PHONE, 0U);
	now = btd_now_ms();
	world.controller.auth_status = 0x0cU;
	btd_phone_tick(&world.phone, now + BTD_PHONE_PEER_MS + 1U);
	check(commands_of(TEST_AUTHENTICATE) == 1U && world.phone.secure_step == BTD_PHONE_SECURE_WAIT_PEER, "inbound: Command Disallowed waits for the phone again");
	give_handle_event(TEST_EVENT_ENCRYPTION, 0x00U, TEST_HANDLE, 0x01U, 1);
	check(world.phone.state == BTD_PHONE_READY, "inbound: ready after the collision");
	close_world();

	/* The wait runs out and bluetoothd authenticates: an LMP collision asked again once, then encryption. */
	present_world();
	give_request(TEST_PHONE);
	give_connected(0x00U, TEST_HANDLE, TEST_PHONE, 0U);
	now = btd_now_ms();
	btd_phone_tick(&world.phone, now + BTD_PHONE_PEER_MS + 1U);
	check(world.phone.secure_step == BTD_PHONE_SECURE_AUTH, "inbound: authentication after the wait");
	give_handle_event(TEST_EVENT_AUTHENTICATED, 0x23U, TEST_HANDLE, 0U, 0);
	check(world.phone.state == BTD_PHONE_SECURING && world.phone.auth_again_at != 0U, "inbound: a collision asked again later");
	btd_phone_tick(&world.phone, world.phone.auth_again_at);
	check(commands_of(TEST_AUTHENTICATE) == 2U, "inbound: authentication asked again");
	give_handle_event(TEST_EVENT_AUTHENTICATED, 0x00U, TEST_HANDLE, 0U, 0);
	give_handle_event(TEST_EVENT_ENCRYPTION, 0x00U, TEST_HANDLE, 0x01U, 1);
	check(world.phone.state == BTD_PHONE_READY, "inbound: ready after asking again");
	close_world();
}

/* A page crossing the phone's connection, and a Connection Request while ready. */
static void
test_crossing(void)
{
	uint64_t now;

	/* Paging, the phone connects: the page cancelled, the phone accepted; the page's 0x02 passed over. */
	present_world();
	now = btd_now_ms();
	btd_phone_tick(&world.phone, now);
	give_request(TEST_PHONE);
	check(commands_of(TEST_CANCEL) == 1U && commands_of(TEST_ACCEPT) == 1U, "crossing: Cancel, then Accept");
	check(world.phone.state == BTD_PHONE_ACCEPTING && world.phone.page_outstanding, "crossing: accepting, the page's end to come");
	give_connected(0x02U, 0x0000U, TEST_PHONE, 0U);
	check(world.phone.state == BTD_PHONE_ACCEPTING && !world.phone.page_outstanding, "crossing: the cancelled page's 0x02 passed over");
	give_connected(0x00U, TEST_HANDLE, TEST_PHONE, 0U);
	check(world.phone.state == BTD_PHONE_SECURING && world.phone.secure_step == BTD_PHONE_SECURE_WAIT_PEER, "crossing: the phone's link secured");
	close_world();

	/* The page made its link already (Cancel answers 0x0B): the phone refused, its own Connection Complete passed over, the page's link taken. */
	present_world();
	now = btd_now_ms();
	btd_phone_tick(&world.phone, now);
	world.controller.cancel_status = 0x0bU;
	give_request(TEST_PHONE);
	check(commands_of(TEST_REJECT) == 1U && commands_of(TEST_ACCEPT) == 0U && world.phone.state == BTD_PHONE_PAGING, "crossing: a page with its link refuses the phone");
	give_connected(0x0dU, 0x0000U, TEST_PHONE, 0U);
	check(world.phone.state == BTD_PHONE_PAGING && !world.phone.reject_pending, "crossing: the refusal's Connection Complete passed over");
	give_connected(0x00U, TEST_HANDLE, TEST_PHONE, 0U);
	check(world.phone.state == BTD_PHONE_SECURING && world.phone.secure_step == BTD_PHONE_SECURE_AUTH, "crossing: the page's link secured");
	close_world();

	/* Ready: another Connection Request refused, its own Connection Complete passed over. */
	present_world();
	page_to_ready(btd_now_ms());
	give_request(TEST_PHONE);
	check(commands_of(TEST_REJECT) == 1U && world.phone.reject_pending, "crossing: refused while ready");
	give_connected(0x0dU, 0x0000U, TEST_PHONE, 0U);
	check(world.phone.state == BTD_PHONE_READY && !world.phone.reject_pending, "crossing: the ready link kept");
	close_world();
}

/* The page's guard, Key Missing, and the wait after a failed page. */
static void
test_guard_and_keys(void)
{
	uint64_t now;

	/* A page with no answer: cancelled after its guard; its 0x02 ends it, the next after 30 s. */
	present_world();
	now = btd_now_ms();
	btd_phone_tick(&world.phone, now);
	btd_phone_tick(&world.phone, now + BTD_PHONE_PAGE_MS);
	check(world.phone.state == BTD_PHONE_CANCELLING && commands_of(TEST_CANCEL) == 1U, "guard: the page cancelled");
	give_connected(0x02U, 0x0000U, TEST_PHONE, 0U);
	check(world.phone.state == BTD_PHONE_NONE && world.phone.backoff_step == 1U, "guard: no link, a step on");
	check(world.phone.next_page_at >= now + 30000U && world.phone.next_page_at <= btd_now_ms() + 30000U, "guard: the next page after 30 s");
	close_world();

	/* The phone forgot the bond: Key Missing ends the link, no page after it. */
	present_world();
	now = btd_now_ms();
	btd_phone_tick(&world.phone, now);
	give_connected(0x00U, TEST_HANDLE, TEST_PHONE, 0U);
	give_handle_event(TEST_EVENT_AUTHENTICATED, 0x06U, TEST_HANDLE, 0U, 0);
	check(world.phone.state == BTD_PHONE_CLOSING && strcmp(world.phone.why, "key-missing") == 0, "keys: Key Missing ends the link");
	give_handle_event(TEST_EVENT_DISCONNECTED, 0x00U, TEST_HANDLE, 0x16U, 1);
	check(world.phone.state == BTD_PHONE_NONE && world.phone.stopped, "keys: no page after Key Missing");
	btd_phone_tick(&world.phone, now + 700000U);
	check(commands_of(TEST_CREATE) == 1U, "keys: no page later either");

	/* A sleep's end pages again. */
	btd_phone_resume(&world.phone, now);
	btd_phone_tick(&world.phone, now);
	check(commands_of(TEST_CREATE) == 2U, "keys: a sleep's end pages again");
	close_world();
}

/* What a disconnection's reason leaves: the step, its start over, and the phone's user ending short links. */
static void
test_reasons(void)
{
	uint64_t now;
	unsigned round;

	/* Out of range (0x08) soon after ready: a short link, the next page after a step. */
	present_world();
	now = btd_now_ms();
	page_to_ready(now);
	give_handle_event(TEST_EVENT_DISCONNECTED, 0x00U, TEST_HANDLE, 0x08U, 1);
	check(world.phone.state == BTD_PHONE_NONE && !world.phone.stopped && world.phone.backoff_step == 1U, "reasons: 0x08 pages again after a step");
	check(world.heard.ended == 1U, "reasons: the profile hears the end");

	/* Ready again and for two minutes: the wait starts over. */
	world.phone.backoff_step = 3U;
	page_to_ready(world.phone.next_page_at);
	btd_phone_tick(&world.phone, world.phone.ready_since + BTD_PHONE_STABLE_MS);
	check(world.phone.backoff_step == 0U, "reasons: two minutes ready start the wait over");
	close_world();

	/* The phone's user ends short links (0x13): twice the pages go on, the third time they stop. */
	present_world();
	now = btd_now_ms();
	for (round = 0U; round < BTD_PHONE_PEER_CLOSED_MAX; round++) {
		page_to_ready(now);
		give_handle_event(TEST_EVENT_DISCONNECTED, 0x00U, TEST_HANDLE, 0x13U, 1);
		now = world.phone.next_page_at;
		if (round + 1U < BTD_PHONE_PEER_CLOSED_MAX)
			check(!world.phone.stopped, "reasons: a short 0x13 pages again");
	}
	check(world.phone.stopped && strcmp(world.phone.why, "peer-closed") == 0, "reasons: three short 0x13 stop the pages");

	/* PHONE LINK on starts them again. */
	(void)btd_phone_link_set(&world.phone, world.phone.record.address, TEST_UID, 1, -1);
	check(!world.phone.stopped, "reasons: PHONE LINK on pages again");
	close_world();
}

/* The owner leaving: a ready link ended, a page cancelled and its link ended when it comes. */
static void
test_absent(void)
{
	uint64_t now;

	/* Paging when the owner leaves: cancelled, the link ended when it comes, no page after. */
	present_world();
	now = btd_now_ms();
	btd_phone_tick(&world.phone, now);
	btd_phone_set_seat(&world.phone, 1, TEST_OTHER_UID, now);
	check(world.phone.state == BTD_PHONE_CANCELLING && world.phone.stop_wanted && commands_of(TEST_CANCEL) == 1U, "absent: the page cancelled");
	give_connected(0x00U, TEST_HANDLE, TEST_PHONE, 0U);
	check(world.phone.state == BTD_PHONE_CLOSING && commands_of(TEST_DISCONNECT) == 1U, "absent: the page's link ended at once");
	give_handle_event(TEST_EVENT_DISCONNECTED, 0x00U, TEST_HANDLE, 0x16U, 1);
	check(world.phone.state == BTD_PHONE_NONE && world.phone.stopped, "absent: no page while away");
	btd_phone_tick(&world.phone, now + 700000U);
	check(commands_of(TEST_CREATE) == 1U, "absent: no page later");

	/* Back at the seat: a page at once. */
	btd_phone_set_seat(&world.phone, 1, TEST_UID, now);
	btd_phone_tick(&world.phone, now);
	check(commands_of(TEST_CREATE) == 2U, "absent: back at the seat, a page");
	close_world();
}

/* Gives the router an event of the controller. */
static void
give_event(
	uint8_t code,
	const uint8_t *parameters,
	size_t length)
{
	uint8_t packet[64];

	/* The event's header, then its parameters. */
	packet[0] = BT_PACKET_EVENT;
	packet[1] = code;
	packet[2] = (uint8_t)length;
	memcpy(packet + 3, parameters, length);
	btd_router_handle(&world.router, &world.session, packet, length + 3U);
}

/* Gives Connection Complete (status, handle, a device, ACL, encryption). */
static void
give_connected(
	uint8_t status,
	uint16_t handle,
	uint8_t last,
	uint8_t encrypted)
{
	uint8_t parameters[11];

	/* The status, the handle, the address, ACL, the encryption. */
	parameters[0] = status;
	parameters[1] = (uint8_t)(handle & 0xffU);
	parameters[2] = (uint8_t)(handle >> 8);
	make_address(parameters + 3, last);
	parameters[9] = 0x01U;
	parameters[10] = encrypted;
	give_event(TEST_EVENT_CONNECTED, parameters, sizeof(parameters));
}

/* Gives Connection Request of a device: a phone's class, ACL. */
static void
give_request(
	uint8_t last)
{
	uint8_t parameters[10];

	/* The address, the class, ACL. */
	make_address(parameters, last);
	parameters[6] = 0x0cU;
	parameters[7] = 0x02U;
	parameters[8] = 0x5aU;
	parameters[9] = 0x01U;
	give_event(TEST_EVENT_REQUEST, parameters, sizeof(parameters));
}

/* Gives an event of a link: its status, its handle, and a byte after them (the reason, the encryption) when it has one. */
static void
give_handle_event(
	uint8_t code,
	uint8_t status,
	uint16_t handle,
	uint8_t extra,
	int with_extra)
{
	uint8_t parameters[4];
	size_t length;

	/* The status, the handle, and the byte after them. */
	parameters[0] = status;
	parameters[1] = (uint8_t)(handle & 0xffU);
	parameters[2] = (uint8_t)(handle >> 8);
	parameters[3] = extra;
	length = 3U;
	if (with_extra)
		length = 4U;
	give_event(code, parameters, length);
}

/* Gives Link Key Request of a device. */
static void
give_key_request(
	uint8_t last)
{
	uint8_t parameters[BTD_ADDRESS_BYTES];

	/* The address. */
	make_address(parameters, last);
	give_event(TEST_EVENT_KEY_REQUEST, parameters, sizeof(parameters));
}

/* Makes a world whose phone's owner sits at the seat: the phone's bond and record, loaded. */
static void
present_world(void)
{
	/* The bond, the record, the load and the seat. */
	open_world();
	write_bond(TEST_PHONE, TEST_KEY_MITM);
	write_record(TEST_PHONE, TEST_UID, "tester", 1);
	(void)btd_phone_load(&world.phone);
	btd_phone_set_seat(&world.phone, 1, TEST_UID, btd_now_ms());
	check(world.phone.present, "setup: the owner at the seat");
}

/* Pages the phone at a time and makes its link ready (a fresh link each time). */
static void
page_to_ready(
	uint64_t now)
{
	/* The page, its Connection Complete, authentication, encryption, the key's size. */
	btd_phone_tick(&world.phone, now);
	give_connected(0x00U, TEST_HANDLE, TEST_PHONE, 0U);
	give_handle_event(TEST_EVENT_AUTHENTICATED, 0x00U, TEST_HANDLE, 0U, 0);
	give_handle_event(TEST_EVENT_ENCRYPTION, 0x00U, TEST_HANDLE, 0x01U, 1);
	check(world.phone.state == BTD_PHONE_READY, "setup: the paged link ready");
}

/* The profile heard the link ready. */
static void
heard_ready(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* Counted. */
	world.heard.ready++;
}

/* The profile heard the link's end. */
static void
heard_ended(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* Counted. */
	world.heard.ended++;
}

/* The profile heard its SDP query's end: the MAS record's channel kept. */
static void
heard_sdp(
	void *context,
	const struct btd_sdp *sdp,
	int error)
{
	unsigned channel;
	int found;

	UNUSED_PARAMETER(context);

	/* Counted, with the channel when it ended well. */
	world.heard.sdp_done++;
	world.heard.sdp_error = error;
	if (error != 0)
		return;
	found = btd_sdp_rfcomm_channel(sdp, TEST_MAS_CLASS, 0U, &channel);
	if (found == 0)
		world.heard.channel = channel;
}

/* The profile heard its DLC open. */
static void
heard_opened(
	void *context,
	unsigned dlci)
{
	UNUSED_PARAMETER(context);

	/* Counted, its DLCI kept. */
	world.heard.opened++;
	world.heard.dlci = dlci;
}

/* The profile heard a DLC close. */
static void
heard_closed(
	void *context,
	unsigned dlci,
	int reason)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(dlci);
	UNUSED_PARAMETER(reason);

	/* Counted. */
	world.heard.closed++;
}

/* The profile heard a DLC that could not open. */
static void
heard_failed(
	void *context,
	unsigned server_channel)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(server_channel);

	/* Counted. */
	world.heard.open_failed++;
}

/* The notices of the phone's link, and its end. */
static void
test_notices(void)
{
	uint8_t ended[7];
	const char *why;
	unsigned requests;
	int error;

	/* A link with an RFCOMM session (a profile's DLC made it). */
	open_world();
	(void)hand_over(TEST_KEY_MITM, 16U, 1, 0x5a020cU, &why);
	exchange();
	error = btd_phone_dlc_open(&world.phone, TEST_MAS_CHANNEL, btd_now_ms());
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
	check(world.phone.state == BTD_PHONE_NONE && world.hid.limit == BTD_PHONE_HID_LIMIT, "notices: the link's end (the HID host's limit follows the record)");
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

/*
 * The owner's record (ws197-p003 section 3): written by the handoff,
 * checked before a pairing, switched by PHONE LINK, shown, forgotten, and
 * read when the controller opens.
 */
static void
test_records(void)
{
	struct btd_phonerec record;
	uint8_t address[BTD_ADDRESS_BYTES];
	uint8_t second[BTD_ADDRESS_BYTES];
	char line[256];
	const char *refusal;
	const char *why;
	int taken;
	int error;

	/* Another phone's valid record: the handoff refuses. */
	open_world();
	make_address(address, TEST_PHONE);
	make_address(second, TEST_SECOND_PHONE);
	write_bond(TEST_SECOND_PHONE, TEST_KEY_MITM);
	write_record(TEST_SECOND_PHONE, TEST_UID, "tester", 1);
	taken = hand_over(TEST_KEY_MITM, 16U, 1, 0x5a020cU, &why);
	check(!taken && why != NULL && strcmp(why, "other-phone") == 0, "records: another phone's valid record refuses");
	check(btd_router_owner(&world.router, TEST_HANDLE) == BTD_OWNER_NONE, "records: the route not taken");

	/* The uid's account renamed: that record is invalid, does not refuse, and goes; the new one has the new name. */
	(void)snprintf(account_name, sizeof(account_name), "%s", "renamed");
	taken = hand_over(TEST_KEY_MITM, 16U, 1, 0x5a020cU, &why);
	check(taken == 1, "records: an invalid record of another phone does not refuse");
	error = btd_phonerec_read(folder, world.session.address, second, &record);
	check(error == ENOENT, "records: the invalid record of another phone goes");
	error = btd_phonerec_read(folder, world.session.address, address, &record);
	check(error == 0 && record.uid == TEST_UID && strcmp(record.user, "renamed") == 0, "records: the handoff writes the owner's record");
	check(error == 0 && record.enabled == 1 && record.profiles == BTD_PHONEREC_PROFILES, "records: wanted, every profile on");
	check(world.phone.have_record && world.phone.record_valid && world.hid.limit == BTD_PHONE_HID_LIMIT, "records: kept, the HID host's limit five");
	close_world();

	/* This phone's record of another owner: the handoff refuses. */
	open_world();
	make_address(address, TEST_PHONE);
	write_bond(TEST_PHONE, TEST_KEY_MITM);
	write_record(TEST_PHONE, TEST_OTHER_UID, "other", 1);
	taken = hand_over(TEST_KEY_MITM, 16U, 1, 0x5a020cU, &why);
	check(!taken && why != NULL && strcmp(why, "owned") == 0, "records: another owner's phone refuses");

	/* The check before a pairing: another's phone, the seat, root. */
	refusal = btd_phone_pair_check(&world.phone, address, TEST_UID, 0, 1);
	check(refusal != NULL && strcmp(refusal, "owned") == 0, "records: a pairing of another owner's phone refused");
	refusal = btd_phone_pair_check(&world.phone, address, 0, 0, 0);
	check(refusal == NULL, "records: root pairs it");
	refusal = btd_phone_pair_check(&world.phone, address, TEST_OTHER_UID, 1, 0);
	check(refusal != NULL && strcmp(refusal, "phone-seat") == 0, "records: a phone's pairing off the seat refused");
	refusal = btd_phone_pair_check(&world.phone, address, TEST_OTHER_UID, 1, 1);
	check(refusal == NULL, "records: the owner at the seat pairs it");

	/* PHONE LINK: another uid refused; the owner turns it off, then on with messages alone. */
	(void)btd_phone_load(&world.phone);
	error = btd_phone_link_set(&world.phone, address, TEST_UID, 0, -1);
	check(error == EPERM, "records: PHONE LINK by another refused");
	error = btd_phone_link_set(&world.phone, address, TEST_OTHER_UID, 0, -1);
	check(error == 0 && world.hid.limit == BTD_HID_MAX, "records: off, the HID host's links all its own");
	error = btd_phonerec_read(folder, world.session.address, address, &record);
	check(error == 0 && record.enabled == 0 && record.profiles == BTD_PHONEREC_PROFILES, "records: off written, the profiles kept");
	error = btd_phone_link_set(&world.phone, address, TEST_OTHER_UID, 1, (int)BTD_PHONEREC_MESSAGES);
	check(error == 0 && world.hid.limit == BTD_PHONE_HID_LIMIT, "records: on again");
	error = btd_phonerec_read(folder, world.session.address, address, &record);
	check(error == 0 && record.enabled == 1 && record.profiles == BTD_PHONEREC_MESSAGES, "records: on written with messages alone");
	make_address(second, TEST_SECOND_PHONE);
	error = btd_phone_link_set(&world.phone, second, 0, 1, -1);
	check(error == ENOENT, "records: PHONE LINK of a device without a record");

	/* PHONE SHOW: the owner sees it whole, another the address and the switch, a stranger nothing. */
	error = btd_phone_show(&world.phone, TEST_OTHER_UID, 1, line, sizeof(line));
	check(error == 0 && strstr(line, "owner=1002 mine=1 enabled=1 profiles=m ") != NULL, "records: SHOW to the owner");
	error = btd_phone_show(&world.phone, TEST_UID, 1, line, sizeof(line));
	check(error == 0 && strstr(line, "mine=0 enabled=1") != NULL && strstr(line, "owner=") == NULL, "records: SHOW to another");
	error = btd_phone_show(&world.phone, TEST_UID, 0, line, sizeof(line));
	check(error == ENOENT, "records: SHOW to a stranger");

	/* FORGET: another refused, the owner's goes. */
	error = btd_phone_forget(&world.phone, address, TEST_UID);
	check(error == EPERM, "records: FORGET by another refused");
	error = btd_phone_forget(&world.phone, address, TEST_OTHER_UID);
	check(error == 0 && !world.phone.have_record && world.hid.limit == BTD_HID_MAX, "records: FORGET by the owner");
	error = btd_phonerec_read(folder, world.session.address, address, &record);
	check(error == ENOENT, "records: the file gone");
	error = btd_phone_forget(&world.phone, address, TEST_OTHER_UID);
	check(error == ENOENT, "records: FORGET of none");

	/* An invalid record (its account renamed) is forgotten by anyone. */
	write_record(TEST_PHONE, TEST_UID, "tester", 1);
	(void)snprintf(account_name, sizeof(account_name), "%s", "renamed");
	error = btd_phone_forget(&world.phone, address, TEST_OTHER_UID);
	check(error == 0, "records: an invalid record forgotten by anyone");
	close_world();

	/* The load: a record without its bond goes. */
	open_world();
	make_address(second, TEST_SECOND_PHONE);
	write_record(TEST_SECOND_PHONE, TEST_UID, "tester", 1);
	error = btd_phone_load(&world.phone);
	check(error == 0 && !world.phone.have_record, "records: load without records");
	error = btd_phonerec_read(folder, world.session.address, second, &record);
	check(error == ENOENT, "records: a record without its bond pruned");

	/* Two valid records: neither used. */
	write_bond(TEST_PHONE, TEST_KEY_MITM);
	write_record(TEST_PHONE, TEST_UID, "tester", 1);
	write_bond(TEST_SECOND_PHONE, TEST_KEY_MITM);
	write_record(TEST_SECOND_PHONE, TEST_UID, "tester", 1);
	error = btd_phone_load(&world.phone);
	check(error == EEXIST && !world.phone.have_record && world.hid.limit == BTD_HID_MAX, "records: two valid records, neither used");

	/* One forgotten by root: the other is the phone. */
	error = btd_phone_forget(&world.phone, second, 0);
	check(error == 0, "records: root forgets one");
	error = btd_phone_load(&world.phone);
	check(error == 0 && world.phone.have_record && world.phone.record_valid && world.hid.limit == BTD_PHONE_HID_LIMIT, "records: the other loaded");

	/* A Just Works bond makes its record invalid: shown as such. */
	write_bond(TEST_PHONE, TEST_KEY_JUST_WORKS);
	error = btd_phone_load(&world.phone);
	check(error == 0 && world.phone.have_record && !world.phone.record_valid && world.hid.limit == BTD_HID_MAX, "records: a Just Works bond's record invalid");
	error = btd_phone_show(&world.phone, 0, 1, line, sizeof(line));
	check(error == 0 && strstr(line, "owner=invalid") != NULL, "records: SHOW of an invalid record");
	close_world();
}

/* The phone link's account hook: TEST_UID's account (by its current name) and TEST_OTHER_UID's "other". */
static int
hook_account(
	void *context,
	uid_t uid,
	char *name,
	size_t size)
{
	UNUSED_PARAMETER(context);

	/* TEST_UID's. */
	if (uid == TEST_UID) {
		(void)snprintf(name, size, "%s", account_name);
		return 0;
	}

	/* TEST_OTHER_UID's. */
	if (uid == TEST_OTHER_UID) {
		(void)snprintf(name, size, "%s", "other");
		return 0;
	}

	/* No such account. */
	return ENOENT;
}

/* Writes a bond of a phone (its address's last byte) with a key of a type. */
static void
write_bond(
	uint8_t last,
	uint8_t key_type)
{
	struct btd_bond bond;
	int error;

	/* The bond. */
	memset(&bond, 0, sizeof(bond));
	make_address(bond.address, last);
	bond.type = BTD_ADDRESS_BREDR;
	bond.have_link_key = 1;
	bond.link_key_type = key_type;
	bond.key_size = 16U;

	/* Written, or the test cannot run. */
	error = btd_keys_write(folder, world.session.address, &bond);
	if (error != 0) {
		fprintf(stderr, "btd_keys_write: %s\n", strerror(error));
		exit(2);
	}
}

/* Writes a phone's record (its address's last byte) of an owner, every profile on. */
static void
write_record(
	uint8_t last,
	uid_t uid,
	const char *user,
	int enabled)
{
	struct btd_phonerec record;
	int error;

	/* The record. */
	memset(&record, 0, sizeof(record));
	make_address(record.address, last);
	record.uid = uid;
	(void)snprintf(record.user, sizeof(record.user), "%s", user);
	record.profiles = BTD_PHONEREC_PROFILES;
	record.enabled = enabled;

	/* Written, or the test cannot run. */
	error = btd_phonerec_write(folder, world.session.address, &record);
	if (error != 0) {
		fprintf(stderr, "btd_phonerec_write: %s\n", strerror(error));
		exit(2);
	}
}
