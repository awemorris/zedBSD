/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of bluetoothd's session queue, ACL buffers and pairing
 * (ws143-p004 i02, plan/ws143/phase004/phase.md sections 9 and 10):
 * session.c and pair.c, with hci.c, acl.c, l2cap.c, smp.c, crypto.c and
 * keys.c, built with the host's compiler, against a scripted controller on
 * a socket pair.
 *
 *   start    the event masks (review B1), Write Simple Pairing Mode,
 *            Secure Connections and LE Host Support, LE Read Buffer Size
 *            (review B5)
 *   queue    packets that come while a command waits are queued and come
 *            out after it, in order; a flood past the queue is counted
 *            (review B3)
 *   bredr    the controller plays the other device's side of Secure
 *            Simple Pairing: Numeric Comparison yes (the Command Status
 *            of Authentication Requested comes after the Link Key
 *            Request, review B3), the stored key, no, Just Works, the
 *            debug key, a key of 7 bytes, a key the device lost, a PIN
 *            (Simple Pairing off), an unreachable device, a device that
 *            goes, the agent's silence, pairings this side did not start
 *   le       the controller plays an LE device's Security Manager with
 *            Secure Connections' Just Works (its values from crypto.c and
 *            the test's P-256 keys and DHKey, which python checked), the
 *            keys stored under the identity address; LE's own buffers of
 *            27 bytes and 2 packets (a public key goes in 3 packets); a
 *            bonded device; a connection that does not come (cancelled)
 *
 *   plan/ws143/tests/bt-daemon-host-test.sh
 */

#include "userland/base/bluetoothd/acl.h"
#include "userland/base/bluetoothd/crypto.h"
#include "userland/base/bluetoothd/hci.h"
#include "userland/base/bluetoothd/keys.h"
#include "userland/base/bluetoothd/pair.h"
#include "userland/base/bluetoothd/session.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The devices the scripted controller plays (the last byte of 0A:0B:0C:0D:0E:xx). */
#define DEVICE_NUMERIC		0x01U
#define DEVICE_NUMERIC_TWO	0x02U
#define DEVICE_MOUSE_LE		0x03U
#define DEVICE_DEBUG		0x05U
#define DEVICE_SHORT		0x06U
#define DEVICE_JUST_WORKS	0x07U
#define DEVICE_FORGOT		0x08U
#define DEVICE_AWAY		0x09U
#define DEVICE_LE_AWAY		0x0aU
#define DEVICE_GOES		0x0bU
#define DEVICE_KEYBOARD		0x0cU

/* The handles of the BR/EDR and the LE connection. */
#define HANDLE_BREDR		0x0040U
#define HANDLE_LE		0x0041U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/* The bonds' folder the script made. */
static const char *keys_folder;

/* The test's P-256 keys (made up, not the Core's debug key) and their DHKey, most significant first. */
static const char key_a_x[] = "e9f97fa093e059947e2b04296d94fcaedc7c39ccaee9c485fc201ae248b6c153";
static const char key_a_y[] = "076c1053224d5784a17567fe0ad585252185951f73bae27ae5c1786c4ba2c28b";
static const char key_b_x[] = "15246a54914ceab36f71fc8de373d8c453655170e3914031e75a3d7f728ebf84";
static const char key_b_y[] = "6f5d5e43a59cbd641f443cb01086c98180576f9be3b34818ab09a7c76adcb985";
static const char key_dh[] = "ac83af932027039b42c742d520774337c0bb9f1425ee556ff54ee960ebf9ea01";

/* The LE device's identity: a random static address C0:11:22:33:44:55 and its IRK. */
static const uint8_t identity_address[6] = { 0x55U, 0x44U, 0x33U, 0x22U, 0x11U, 0xc0U };

/*
 * The scripted controller of one run: what it plays, what it saw, and its
 * end of the socket pair.  The test's thread and the controller's thread
 * share it; the test reads the counters after joining the controller, or
 * reads only those the controller does not write meanwhile.
 */
struct fake {
	int descriptor;

	/* What it answers at the start. */
	int refuse_ssp;
	unsigned acl_total;
	unsigned le_length;
	unsigned le_total;

	/* What the start sent. */
	uint8_t event_mask[8];
	uint8_t le_mask[8];
	int ssp_mode;
	int sc_host;
	int le_host;
	int le_buffer_read;

	/* The BR/EDR device of the connection, and the link key it gave. */
	uint8_t device[6];
	int connected;
	int confirmed;
	uint8_t host_io;

	/* The LE device's Security Manager: the Pairing Request and Response, the nonces, the keys of f5. */
	uint8_t preq[7];
	uint8_t pres[7];
	uint8_t na[16];
	uint8_t nb[16];
	uint8_t mac_key[16];
	uint8_t ltk[16];
	int le_connected;
	unsigned le_step;
	unsigned smp_errors;

	/* The ACL path: the frames put together, the packets not completed yet, the breaches of the buffers. */
	struct btd_reassembly bredr_frames;
	struct btd_reassembly le_frames;
	unsigned bredr_outstanding;
	unsigned le_outstanding;
	unsigned credit_breaches;
	unsigned le_packets;
	unsigned information_requests;

	/* The commands seen, and a flood asked for. */
	uint16_t opcodes[512];
	unsigned opcode_count;
	unsigned flood;
};

/* What the pairing's hooks saw: the questions, the answer to give, the end. */
struct hooks {
	unsigned asked_kind;
	uint32_t asked_number;
	unsigned asked_count;
	int answer;
	int silent;
	int done;
	char end[BTD_PAIR_ANSWER_MAX];
};

static void expect(int condition, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void hex(const char *text, uint8_t *bytes);
static void reverse(uint8_t *to, const uint8_t *from, size_t length);
static void test_random(void *context, uint8_t *bytes, size_t length);
static void hook_ask(void *context, unsigned kind, uint32_t number);
static void hook_done(void *context, const char *answer);
static void test_start(void);
static void test_queue(void);
static void test_bredr(void);
static void test_pin(void);
static void test_le(void);
static void run_pair(struct btd_session *session, struct btd_pair *pair, struct hooks *hooks, uint8_t last, unsigned type);
static void open_run(struct fake *fake, struct btd_session *session, struct btd_pair *pair, struct hooks *hooks, pthread_t *thread);
static void close_run(struct fake *fake, struct btd_session *session, pthread_t thread);
static void *fake_run(void *argument);
static void fake_command(struct fake *fake, const uint8_t *packet, size_t length);
static void fake_acl(struct fake *fake, const uint8_t *packet, size_t length);
static void fake_smp(struct fake *fake, const uint8_t *pdu, size_t length);
static void fake_send(struct fake *fake, const uint8_t *packet, size_t length);
static void fake_event(struct fake *fake, uint8_t code, const uint8_t *parameters, size_t length);
static void fake_complete(struct fake *fake, uint16_t opcode, const uint8_t *returned, size_t count);
static void fake_status(struct fake *fake, uint16_t opcode, uint8_t status);
static void fake_frame(struct fake *fake, uint16_t handle, uint16_t cid, const uint8_t *payload, size_t length);
static void fake_completed(struct fake *fake, uint16_t handle, unsigned count);
static void fake_address_event(struct fake *fake, uint8_t code, const uint8_t *address, const uint8_t *more, size_t more_length);
static int fake_saw(const struct fake *fake, uint16_t opcode);
static void device_address(uint8_t last, uint8_t *address);

/*
 * Runs every part; the exit status says whether every check held.
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

	/* The parts. */
	test_start();
	test_queue();
	test_bredr();
	test_pin();
	test_le();

	/* The verdict. */
	if (failures != 0U) {
		printf("bt-link-host-test: FAIL (%u of %u checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("bt-link-host-test: PASS (%u checks)\n", checks);
	return 0;
}

/* Counts a check, and prints it when it failed. */
static void
expect(
	int condition,
	const char *format,
	...)
{
	va_list arguments;

	/* One more check. */
	checks++;
	if (condition)
		return;

	/* A failure, named. */
	failures++;
	printf("FAIL: ");
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
}

/* Reads hex digits into bytes. */
static void
hex(
	const char *text,
	uint8_t *bytes)
{
	unsigned value;
	size_t index;

	/* Two digits a byte. */
	for (index = 0U; text[2U * index] != '\0'; index++) {
		(void)sscanf(text + 2U * index, "%2x", &value);
		bytes[index] = (uint8_t)value;
	}
}

/* Copies bytes in the other order. */
static void
reverse(
	uint8_t *to,
	const uint8_t *from,
	size_t length)
{
	size_t index;

	/* The last first. */
	for (index = 0U; index < length; index++)
		to[index] = from[length - 1U - index];
}

/* The test's random source: a counter, so a run is the same every time. */
static void
test_random(
	void *context,
	uint8_t *bytes,
	size_t length)
{
	static uint8_t counter;
	size_t index;

	/* Each byte the next count. */
	(void)context;
	for (index = 0U; index < length; index++) {
		counter = (uint8_t)(counter * 29U + 17U);
		bytes[index] = counter;
	}
}

/* Records the agent's question; a confirmation is answered by the run's loop. */
static void
hook_ask(
	void *context,
	unsigned kind,
	uint32_t number)
{
	struct hooks *hooks;

	/* The question. */
	hooks = context;
	hooks->asked_kind = kind;
	hooks->asked_number = number;
	hooks->asked_count++;
}

/* Records the pairing's end. */
static void
hook_done(
	void *context,
	const char *answer)
{
	struct hooks *hooks;

	/* The line. */
	hooks = context;
	hooks->done = 1;
	(void)snprintf(hooks->end, sizeof(hooks->end), "%s", answer);
}

/* The start: masks, Simple Pairing, Secure Connections, LE on the host, LE's own buffers. */
static void
test_start(void)
{
	static const uint8_t mask[8] = { 0xbfU, 0x80U, 0xe0U, 0x02U, 0x02U, 0xc0U, 0x2fU, 0x24U };
	static const uint8_t le_mask[8] = { 0x87U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
	struct btd_session *session;
	struct btd_pair pair;
	struct hooks hooks;
	struct fake fake;
	pthread_t thread;
	int same;
	int error;

	/* A controller with LE buffers of its own. */
	session = calloc(1U, sizeof(*session));
	memset(&fake, 0, sizeof(fake));
	fake.acl_total = 2U;
	fake.le_length = 27U;
	fake.le_total = 2U;
	open_run(&fake, session, &pair, &hooks, &thread);
	error = btd_session_start(session);
	expect(error == 0 && session->state == BTD_STATE_READY, "start: ready (%d %s)", error, session->reason);
	close_run(&fake, session, thread);

	/* The masks (Core Vol 4 Part E §7.3.1 and §7.8.1). */
	same = memcmp(fake.event_mask, mask, sizeof(mask));
	expect(same == 0, "start: the event mask (review B1)");
	same = memcmp(fake.le_mask, le_mask, sizeof(le_mask));
	expect(same == 0, "start: the LE event mask");

	/* Simple Pairing, Secure Connections and LE on the host's side. */
	expect(fake.ssp_mode == 1 && fake.sc_host == 1 && fake.le_host == 1, "start: SSP, SC and LE host support written");
	expect(session->ssp && session->secure_connections, "start: SSP and SC are on");

	/* The pools: BR/EDR's 1021 bytes and 2 packets, LE's 27 bytes and 2 packets. */
	expect(fake.le_buffer_read && !session->le_shared && session->le_pool.length == 27U && session->le_pool.total == 2U &&
	       session->acl_pool.length == 1021U && session->acl_pool.total == 2U,
	       "start: the ACL pools (review B5)");
	free(session);

	/* A controller that shares BR/EDR's pool with LE (LE Read Buffer Size says 0). */
	session = calloc(1U, sizeof(*session));
	memset(&fake, 0, sizeof(fake));
	fake.acl_total = 4U;
	open_run(&fake, session, &pair, &hooks, &thread);
	error = btd_session_start(session);
	close_run(&fake, session, thread);
	expect(error == 0 && session->le_shared && session->acl_pool.total == 4U, "start: LE shares BR/EDR's pool");
	free(session);
}

/* Packets that come while a command waits: queued in order, and a flood past the queue counted. */
static void
test_queue(void)
{
	struct btd_session *session;
	struct btd_pair pair;
	struct hooks hooks;
	struct fake fake;
	pthread_t thread;
	unsigned handled;
	int error;

	/* A started controller. */
	session = calloc(1U, sizeof(*session));
	memset(&fake, 0, sizeof(fake));
	fake.acl_total = 2U;
	open_run(&fake, session, &pair, &hooks, &thread);
	error = btd_session_start(session);
	expect(error == 0, "queue: started");

	/* A command answered after 200 vendor events of 255 bytes (51 KiB, past the queue's 32 KiB). */
	fake.flood = 200U;
	error = btd_session_command(session, 0xfc77U, NULL, 0U);
	expect(error == 0, "queue: the command's answer came after the flood (%d)", error);
	expect(session->queue_dropped != 0U && btd_session_pending(session), "queue: the flood filled the queue (%u dropped)", session->queue_dropped);

	/* The queued packets come out, then the node has none. */
	handled = 0U;
	for (;;) {
		/* One packet. */
		error = btd_session_input(session);
		if (error != 0)
			break;
		handled++;
	}

	/* Every queued packet, then nothing. */
	expect(error == EAGAIN && handled + session->queue_dropped == 200U && !btd_session_pending(session),
	       "queue: %u packets handled after the command", handled);

	/* A command still works. */
	error = btd_session_command(session, 0x1009U, NULL, 0U);
	expect(error == 0 && session->returned[0] == 0x55U, "queue: a command after the flood");
	close_run(&fake, session, thread);
	free(session);
}

/* BR/EDR pairings against the controller's Secure Simple Pairing. */
static void
test_bredr(void)
{
	struct btd_session *session;
	struct btd_pair pair;
	struct btd_bond bond;
	struct hooks hooks;
	struct fake fake;
	uint8_t address[6];
	uint8_t other[6];
	pthread_t thread;
	int error;

	/* A started controller. */
	session = calloc(1U, sizeof(*session));
	memset(&fake, 0, sizeof(fake));
	fake.acl_total = 2U;
	open_run(&fake, session, &pair, &hooks, &thread);
	error = btd_session_start(session);
	expect(error == 0, "bredr: started");

	/* Numeric Comparison, the agent says yes: paired, the key stored, the data path tried. */
	hooks.answer = 1;
	run_pair(session, &pair, &hooks, DEVICE_NUMERIC, BTD_ADDRESS_BREDR);
	expect(hooks.asked_kind == BTD_PAIR_ASK_CONFIRM && hooks.asked_number == 123456U, "bredr: the agent is asked 123456");
	expect(strstr(hooks.end, "PAIRED address=0A:0B:0C:0D:0E:01 type=bredr authenticated=1 secure=1 legacy=0 key_size=16 stored=0 l2cap=1") != NULL,
	       "bredr: numeric yes is paired (%s)", hooks.end);
	device_address(DEVICE_NUMERIC, address);
	error = btd_keys_read(keys_folder, session->address, address, BTD_ADDRESS_BREDR, &bond);
	expect(error == 0 && bond.have_link_key && bond.link_key_type == 8U && bond.link_key[0] == 0x01U && bond.authenticated,
	       "bredr: the key is stored with its type (%d)", error);
	expect(fake.information_requests == 1U && !fake.connected, "bredr: one Information Request, then disconnected");

	/* The same device again: its stored key, no question. */
	hooks.asked_count = 0U;
	run_pair(session, &pair, &hooks, DEVICE_NUMERIC, BTD_ADDRESS_BREDR);
	expect(strstr(hooks.end, "PAIRED") != NULL && strstr(hooks.end, "stored=1") != NULL && hooks.asked_count == 0U,
	       "bredr: the stored key (%s)", hooks.end);

	/* The agent says no. */
	hooks.answer = 0;
	run_pair(session, &pair, &hooks, DEVICE_NUMERIC_TWO, BTD_ADDRESS_BREDR);
	device_address(DEVICE_NUMERIC_TWO, address);
	error = btd_keys_read(keys_folder, session->address, address, BTD_ADDRESS_BREDR, &bond);
	expect(strcmp(hooks.end, "ERROR rejected") == 0 && error != 0, "bredr: numeric no is rejected, nothing stored (%s)", hooks.end);

	/* The agent says nothing for 30 seconds: no. */
	hooks.silent = 1;
	run_pair(session, &pair, &hooks, DEVICE_NUMERIC_TWO, BTD_ADDRESS_BREDR);
	hooks.silent = 0;
	expect(strcmp(hooks.end, "ERROR rejected") == 0, "bredr: the agent's silence is no (%s)", hooks.end);

	/* Just Works: the agent agrees (design section 6.5), not authenticated. */
	hooks.answer = 1;
	run_pair(session, &pair, &hooks, DEVICE_JUST_WORKS, BTD_ADDRESS_BREDR);
	expect(strstr(hooks.end, "PAIRED") != NULL && strstr(hooks.end, "authenticated=0 secure=1") != NULL &&
	       hooks.asked_kind == BTD_PAIR_ASK_CONSENT,
	       "bredr: Just Works with the agent's agreement (%s)", hooks.end);

	/* Just Works refused by the agent. */
	hooks.answer = 0;
	device_address(DEVICE_JUST_WORKS, address);
	(void)btd_keys_forget(keys_folder, session->address, address, BTD_ADDRESS_BREDR);
	run_pair(session, &pair, &hooks, DEVICE_JUST_WORKS, BTD_ADDRESS_BREDR);
	expect(strcmp(hooks.end, "ERROR rejected") == 0, "bredr: Just Works refused (%s)", hooks.end);
	hooks.answer = 1;

	/* A keyboard: the passkey shown for it to type (Passkey Entry, review S-c). */
	run_pair(session, &pair, &hooks, DEVICE_KEYBOARD, BTD_ADDRESS_BREDR);
	expect(strstr(hooks.end, "PAIRED") != NULL && strstr(hooks.end, "authenticated=1") != NULL &&
	       hooks.asked_kind == BTD_PAIR_ASK_PASSKEY && hooks.asked_number == 654321U,
	       "bredr: the passkey 654321 shown (%s)", hooks.end);

	/* The debug key, a key of 7 bytes. */
	run_pair(session, &pair, &hooks, DEVICE_DEBUG, BTD_ADDRESS_BREDR);
	device_address(DEVICE_DEBUG, address);
	error = btd_keys_read(keys_folder, session->address, address, BTD_ADDRESS_BREDR, &bond);
	expect(strcmp(hooks.end, "ERROR debug-key") == 0 && error != 0, "bredr: the debug key is refused (%s)", hooks.end);
	run_pair(session, &pair, &hooks, DEVICE_SHORT, BTD_ADDRESS_BREDR);
	device_address(DEVICE_SHORT, address);
	error = btd_keys_read(keys_folder, session->address, address, BTD_ADDRESS_BREDR, &bond);
	expect(strcmp(hooks.end, "ERROR key-size") == 0 && error != 0, "bredr: a key of 7 bytes is refused, nothing stored (%s)", hooks.end);

	/* A device that lost the key bluetoothd stored: the bond stays, the user forgets it. */
	memset(&bond, 0, sizeof(bond));
	device_address(DEVICE_FORGOT, bond.address);
	bond.type = BTD_ADDRESS_BREDR;
	bond.have_link_key = 1;
	bond.link_key_type = 8U;
	error = btd_keys_write(keys_folder, session->address, &bond);
	expect(error == 0, "bredr: a bond written by hand");
	run_pair(session, &pair, &hooks, DEVICE_FORGOT, BTD_ADDRESS_BREDR);
	error = btd_keys_read(keys_folder, session->address, bond.address, BTD_ADDRESS_BREDR, &bond);
	expect(strcmp(hooks.end, "ERROR key-missing") == 0 && error == 0, "bredr: key missing, the bond kept (%s)", hooks.end);

	/* A device that does not answer the page, and one that goes in the middle. */
	run_pair(session, &pair, &hooks, DEVICE_AWAY, BTD_ADDRESS_BREDR);
	expect(strcmp(hooks.end, "ERROR unreachable") == 0, "bredr: unreachable (%s)", hooks.end);
	run_pair(session, &pair, &hooks, DEVICE_GOES, BTD_ADDRESS_BREDR);
	expect(strcmp(hooks.end, "ERROR lost") == 0, "bredr: a device that goes (%s)", hooks.end);

	/* Pairings this side did not start: a connection request, an IO capability request, a confirmation. */
	device_address(0x0fU, other);
	fake_address_event(&fake, 0x04U, other, (const uint8_t *)"\x00\x05\x25\x01", 4U);
	fake_address_event(&fake, 0x31U, other, NULL, 0U);
	fake_address_event(&fake, 0x33U, other, (const uint8_t *)"\x01\x00\x00\x00", 4U);
	(void)usleep(50000U);
	for (;;) {
		/* Each packet. */
		error = btd_session_input(session);
		if (error != 0)
			break;
	}

	/* The controller's answers to the refusals. */
	(void)usleep(50000U);
	expect(fake_saw(&fake, 0x040aU) && fake_saw(&fake, 0x0434U) && fake_saw(&fake, 0x042dU) && pair.refused >= 3U,
	       "bredr: pairings from the other side are refused");
	close_run(&fake, session, thread);
	expect(fake.credit_breaches == 0U, "bredr: no packet past the controller's buffers");
	free(session);
}

/* A controller without Secure Simple Pairing: the PIN is refused. */
static void
test_pin(void)
{
	struct btd_session *session;
	struct btd_pair pair;
	struct hooks hooks;
	struct fake fake;
	pthread_t thread;
	int error;

	/* A controller that refuses Write Simple Pairing Mode. */
	session = calloc(1U, sizeof(*session));
	memset(&fake, 0, sizeof(fake));
	fake.acl_total = 2U;
	fake.refuse_ssp = 1;
	open_run(&fake, session, &pair, &hooks, &thread);
	error = btd_session_start(session);
	expect(error == 0 && !session->ssp, "pin: started without SSP");

	/* The device asks for a PIN. */
	hooks.answer = 1;
	run_pair(session, &pair, &hooks, DEVICE_NUMERIC_TWO, BTD_ADDRESS_BREDR);
	expect(strcmp(hooks.end, "ERROR pin-unsupported") == 0 && fake_saw(&fake, 0x040eU), "pin: refused (%s)", hooks.end);
	close_run(&fake, session, thread);
	free(session);
}

/* LE pairings against the controller's Security Manager of a device. */
static void
test_le(void)
{
	struct btd_session *session;
	struct btd_pair pair;
	struct btd_bond bond;
	struct hooks hooks;
	struct fake fake;
	uint8_t irk[16];
	uint8_t prand[3];
	uint8_t hash[3];
	uint8_t rpa[6];
	pthread_t thread;
	int error;

	/* A started controller with LE buffers of 27 bytes and 2 packets. */
	session = calloc(1U, sizeof(*session));
	memset(&fake, 0, sizeof(fake));
	fake.acl_total = 2U;
	fake.le_length = 27U;
	fake.le_total = 2U;
	open_run(&fake, session, &pair, &hooks, &thread);
	error = btd_session_start(session);
	expect(error == 0 && session->p256 && session->dhkey, "le: started");

	/* Secure Connections' Just Works with a device that gives its identity. */
	hooks.answer = 1;
	run_pair(session, &pair, &hooks, DEVICE_MOUSE_LE, BTD_ADDRESS_LE_PUBLIC);
	expect(strstr(hooks.end, "PAIRED address=0A:0B:0C:0D:0E:03 type=le-public authenticated=0 secure=1 legacy=0 key_size=16") != NULL,
	       "le: Just Works is paired (%s)", hooks.end);
	expect(fake.smp_errors == 0U && fake.le_step == 4U, "le: the device's Security Manager saw a right pairing (%u errors, step %u)",
	       fake.smp_errors, fake.le_step);
	error = btd_keys_read(keys_folder, session->address, identity_address, BTD_ADDRESS_LE_RANDOM, &bond);
	expect(error == 0 && bond.have_ltk && bond.have_irk && bond.have_identity && bond.secure && bond.irk[0] == 0x11U,
	       "le: the keys are stored under the identity (%d)", error);
	expect(fake.le_packets >= 6U && fake.credit_breaches == 0U, "le: %u LE packets, none past the buffers (review B5)", fake.le_packets);

	/* Just Works asked the agent (design section 6.5). */
	expect(hooks.asked_kind == BTD_PAIR_ASK_CONSENT, "le: Just Works asked the agent to agree");

	/* The identity is bonded: not paired over (forgotten first). */
	hooks.done = 0;
	error = btd_pair_start(&pair, identity_address, BTD_ADDRESS_LE_RANDOM, 1);
	expect(error == 0 && hooks.done && strcmp(hooks.end, "ERROR bonded") == 0, "le: a bonded device (%s)", hooks.end);

	/* A private address the bond's IRK resolves (the IRK 0x11..., prand 0x4a 0x01 0x02): bonded too (review S-g). */
	memset(irk, 0x11U, sizeof(irk));
	rpa[5] = 0x4aU;
	rpa[4] = 0x01U;
	rpa[3] = 0x02U;
	prand[0] = rpa[5];
	prand[1] = rpa[4];
	prand[2] = rpa[3];
	btd_smp_ah(irk, prand, hash);
	rpa[2] = hash[0];
	rpa[1] = hash[1];
	rpa[0] = hash[2];
	hooks.done = 0;
	error = btd_pair_start(&pair, rpa, BTD_ADDRESS_LE_RANDOM, 1);
	expect(error == 0 && hooks.done && strcmp(hooks.end, "ERROR bonded") == 0, "le: a private address of a bond (%s)", hooks.end);

	/* The same device paired again under its public address: its identity is bonded, nothing written over. */
	run_pair(session, &pair, &hooks, DEVICE_MOUSE_LE, BTD_ADDRESS_LE_PUBLIC);
	expect(strcmp(hooks.end, "ERROR bonded") == 0, "le: the identity is not written over (%s)", hooks.end);

	/* Just Works refused by the agent. */
	(void)btd_keys_forget(keys_folder, session->address, identity_address, BTD_ADDRESS_LE_RANDOM);
	hooks.answer = 0;
	run_pair(session, &pair, &hooks, DEVICE_MOUSE_LE, BTD_ADDRESS_LE_PUBLIC);
	hooks.answer = 1;
	expect(strcmp(hooks.end, "ERROR rejected") == 0, "le: Just Works refused (%s)", hooks.end);

	/* A device that does not connect: cancelled after 10 seconds. */
	run_pair(session, &pair, &hooks, DEVICE_LE_AWAY, BTD_ADDRESS_LE_PUBLIC);
	expect(strcmp(hooks.end, "ERROR timeout") == 0 && fake_saw(&fake, 0x200eU), "le: a connection cancelled (%s)", hooks.end);
	close_run(&fake, session, thread);
	free(session);
}

/*
 * Runs one pairing to its end as the daemon's loop does: the session's
 * packets, the agent's answers, the deadlines (the clock is moved on when
 * the controller has nothing more to say, so the 10 and 30 seconds pass at
 * once).
 */
static void
run_pair(
	struct btd_session *session,
	struct btd_pair *pair,
	struct hooks *hooks,
	uint8_t last,
	unsigned type)
{
	struct pollfd descriptor;
	uint8_t address[6];
	uint64_t clock;
	unsigned idle;
	unsigned rounds;
	int error;

	/* The device's address; nothing heard yet. */
	device_address(last, address);
	hooks->done = 0;
	hooks->end[0] = '\0';
	hooks->asked_kind = 0U;
	hooks->asked_number = 0U;
	clock = btd_now_ms();

	/* The start. */
	error = btd_pair_start(pair, address, type, 1);
	if (error != 0) {
		(void)snprintf(hooks->end, sizeof(hooks->end), "start %d", error);
		return;
	}

	/* Until the end, 4000 rounds at most. */
	idle = 0U;
	for (rounds = 0U; rounds < 4000U && !hooks->done; rounds++) {
		/* The agent's answer, unless it keeps silent. */
		if (pair->asked && !hooks->silent)
			btd_pair_answer(pair, hooks->answer);

		/* A packet, or a short wait for one. */
		error = btd_session_input(session);
		if (error == 0) {
			idle = 0U;
			continue;
		}

		/* Nothing now: a short wait for the controller (any other error ends the run). */
		if (error != EAGAIN)
			break;
		descriptor.fd = session->descriptor;
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		(void)poll(&descriptor, 1U, 10);
		if (descriptor.revents != 0)
			continue;

		/* Quiet for a while: the clock jumps to the next deadline. */
		idle++;
		if (idle >= 5U) {
			clock = btd_pair_deadline(pair);
			idle = 0U;
		}

		/* The deadlines at that clock. */
		btd_pair_tick(pair, clock);
	}

	/* A pairing that never ended. */
	if (!hooks->done)
		(void)snprintf(hooks->end, sizeof(hooks->end), "no end (state %u)", pair->state);
}

/* Starts the scripted controller on a socket pair, the session on the other end, and its pairing as the handler. */
static void
open_run(
	struct fake *fake,
	struct btd_session *session,
	struct btd_pair *pair,
	struct hooks *hooks,
	pthread_t *thread)
{
	int pair_ends[2];
	int status;

	/* A socket pair that keeps packets apart, as the node does. */
	status = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair_ends);
	if (status != 0) {
		perror("socketpair");
		exit(2);
	}

	/* The session, with short timing, and the pairing as its handler. */
	fake->descriptor = pair_ends[1];
	btd_session_init(session, pair_ends[0], NULL, NULL, "fake", "/nonexistent");
	session->timing.command_ms = 300U;
	memset(hooks, 0, sizeof(*hooks));
	btd_pair_init(pair, session, keys_folder, hook_ask, hook_done, hooks, test_random, NULL);
	session->handler = btd_pair_handle;
	session->handler_context = pair;

	/* The controller. */
	status = pthread_create(thread, NULL, fake_run, fake);
	if (status != 0) {
		perror("pthread_create");
		exit(2);
	}
}

/* Ends the scripted controller: the session's end closes, the controller sees it and returns. */
static void
close_run(
	struct fake *fake,
	struct btd_session *session,
	pthread_t thread)
{
	/* The session's end, then the controller. */
	(void)shutdown(session->descriptor, SHUT_RDWR);
	(void)pthread_join(thread, NULL);
	(void)close(session->descriptor);
	(void)close(fake->descriptor);
}

/* The scripted controller: answers each command and ACL packet until its socket closes. */
static void *
fake_run(
	void *argument)
{
	struct fake *fake;
	uint8_t packet[1200];
	ssize_t got;

	/* Plays until the session's end closes. */
	fake = argument;
	for (;;) {
		/* The next packet. */
		got = read(fake->descriptor, packet, sizeof(packet));
		if (got <= 0)
			return NULL;

		/* A command, or ACL data. */
		if (packet[0] == 0x01U && got >= 4)
			fake_command(fake, packet, (size_t)got);
		if (packet[0] == 0x02U && got >= 5)
			fake_acl(fake, packet, (size_t)got);
	}
}

/* Answers one command as the test's controller and the devices it plays. */
static void
fake_command(
	struct fake *fake,
	const uint8_t *packet,
	size_t length)
{
	static const uint8_t ok[1] = { 0x00U };
	static const uint8_t version[9] = { 0x00U, 0x0cU, 0x00U, 0x01U, 0x0cU, 0xffU, 0xffU, 0x01U, 0x00U };
	static const uint8_t own_address[7] = { 0x00U, 0x55U, 0x44U, 0x33U, 0x22U, 0x11U, 0x00U };
	static const uint8_t features[9] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x40U, 0x00U, 0x00U, 0x00U };
	uint8_t returned[80];
	uint8_t parameters[80];
	uint8_t key[64];
	uint8_t vendor[257];
	const uint8_t *body;
	uint16_t opcode;
	unsigned index;
	uint8_t last;
	uint8_t key_type;
	int differs;

	/* The command, recorded. */
	opcode = (uint16_t)(packet[1] | (packet[2] << 8));
	body = packet + 4;
	if (fake->opcode_count < 512U)
		fake->opcodes[fake->opcode_count++] = opcode;
	last = 0U;
	if (fake->device[5] == 0x0aU)
		last = fake->device[0];

	/* Each command's answer. */
	switch (opcode) {
	case 0x1001U:
		fake_complete(fake, opcode, version, sizeof(version));
		break;
	case 0x1009U:
		fake_complete(fake, opcode, own_address, sizeof(own_address));
		break;
	case 0x1002U:
		/* Inquiry, LE's event mask and scan, P-256 and DHKey. */
		memset(returned, 0, sizeof(returned));
		returned[1U + 0U] = 0x03U;
		returned[1U + 25U] = 0x01U;
		returned[1U + 26U] = 0x0cU;
		returned[1U + 34U] = 0x06U;
		fake_complete(fake, opcode, returned, 65U);
		break;
	case 0x1003U:
		fake_complete(fake, opcode, features, sizeof(features));
		break;
	case 0x1005U:
		/* ACL 1021 bytes, no SCO, the test's count of packets. */
		memset(returned, 0, sizeof(returned));
		returned[1] = 0xfdU;
		returned[2] = 0x03U;
		returned[4] = (uint8_t)fake->acl_total;
		fake_complete(fake, opcode, returned, 8U);
		break;
	case 0x2002U:
		/* LE's own pool, or 0 (shared). */
		fake->le_buffer_read = 1;
		returned[0] = 0x00U;
		returned[1] = (uint8_t)(fake->le_length & 0xffU);
		returned[2] = (uint8_t)(fake->le_length >> 8);
		returned[3] = (uint8_t)fake->le_total;
		fake_complete(fake, opcode, returned, 4U);
		break;
	case 0x0c01U:
		memcpy(fake->event_mask, body, 8U);
		fake_complete(fake, opcode, ok, sizeof(ok));
		break;
	case 0x2001U:
		memcpy(fake->le_mask, body, 8U);
		fake_complete(fake, opcode, ok, sizeof(ok));
		break;
	case 0x0c56U:
		/* Simple Pairing on, or Unknown HCI Command. */
		returned[0] = 0x00U;
		if (fake->refuse_ssp)
			returned[0] = 0x01U;
		else
			fake->ssp_mode = body[0];
		fake_complete(fake, opcode, returned, 1U);
		break;
	case 0x0c7aU:
		fake->sc_host = body[0];
		fake_complete(fake, opcode, ok, sizeof(ok));
		break;
	case 0x0c6dU:
		fake->le_host = body[0];
		fake_complete(fake, opcode, ok, sizeof(ok));
		break;
	case 0xfc77U:
		/* The flood: vendor events, then the answer. */
		for (index = 0U; index < fake->flood; index++) {
			memset(vendor, 0x5a, sizeof(vendor));
			fake_event(fake, 0xffU, vendor, 255U);
		}

		/* The answer after them. */
		fake_complete(fake, opcode, ok, sizeof(ok));
		break;
	case 0x0405U:
		/* Create Connection: the device, or a page timeout. */
		memcpy(fake->device, body, 6U);
		fake_status(fake, opcode, 0x00U);
		last = fake->device[0];
		memset(parameters, 0, sizeof(parameters));
		parameters[1] = (uint8_t)(HANDLE_BREDR & 0xffU);
		parameters[2] = (uint8_t)(HANDLE_BREDR >> 8);
		memcpy(parameters + 3, body, 6U);
		parameters[9] = 0x01U;
		if (last == DEVICE_AWAY) {
			parameters[0] = 0x04U;
		} else {
			fake->connected = 1;
			fake->confirmed = 0;
		}

		/* The connection's end of the page. */
		fake_event(fake, 0x03U, parameters, 11U);
		break;
	case 0x0411U:
		/* Authentication Requested: the Link Key Request comes before the Command Status (review B3). */
		fake_address_event(fake, 0x17U, fake->device, NULL, 0U);
		fake_status(fake, opcode, 0x00U);
		break;
	case 0x040bU:
		/* Link Key Request Reply: the device's key or not. */
		fake_complete(fake, opcode, (const uint8_t *)"\x00\x00\x00\x00\x00\x00\x00", 7U);
		parameters[0] = 0x00U;
		if (last == DEVICE_FORGOT || body[6] != last)
			parameters[0] = 0x06U;
		parameters[1] = (uint8_t)(HANDLE_BREDR & 0xffU);
		parameters[2] = (uint8_t)(HANDLE_BREDR >> 8);
		fake_event(fake, 0x06U, parameters, 3U);
		break;
	case 0x040cU:
		/* Link Key Request Negative Reply: Simple Pairing, or a PIN without it. */
		fake_complete(fake, opcode, (const uint8_t *)"\x00\x00\x00\x00\x00\x00\x00", 7U);
		if (fake->ssp_mode)
			fake_address_event(fake, 0x31U, fake->device, NULL, 0U);
		else
			fake_address_event(fake, 0x16U, fake->device, NULL, 0U);
		break;
	case 0x040eU:
		/* PIN Code Request Negative Reply: the authentication fails. */
		fake_complete(fake, opcode, (const uint8_t *)"\x00\x00\x00\x00\x00\x00\x00", 7U);
		fake_event(fake, 0x06U, (const uint8_t *)"\x06\x40\x00", 3U);
		break;
	case 0x042bU:
		/* IO Capability Request Reply: the device's IO, then the number (a device that goes leaves here). */
		fake_complete(fake, opcode, (const uint8_t *)"\x00\x00\x00\x00\x00\x00\x00", 7U);
		fake->host_io = body[6];
		parameters[0] = 0x01U;
		if (last == DEVICE_JUST_WORKS)
			parameters[0] = 0x03U;
		if (last == DEVICE_KEYBOARD)
			parameters[0] = 0x02U;
		parameters[1] = 0x00U;
		parameters[2] = 0x03U;
		fake_address_event(fake, 0x32U, fake->device, parameters, 3U);
		if (last == DEVICE_GOES) {
			fake->connected = 0;
			fake_event(fake, 0x05U, (const uint8_t *)"\x00\x40\x00\x08", 4U);
			break;
		}

		/* A keyboard types the passkey bluetoothd shows: Passkey Entry, then paired as the confirmation's reply goes on. */
		if (last == DEVICE_KEYBOARD) {
			fake_address_event(fake, 0x3bU, fake->device, (const uint8_t *)"\xf1\xfb\x09\x00", 4U);
			fake_command(fake, (const uint8_t *)"\x01\x2c\x04\x06\x0c\x0e\x0d\x0c\x0b\x0a", 10U);
			break;
		}

		/* Numeric Comparison or Just Works: the number 123456. */
		fake_address_event(fake, 0x33U, fake->device, (const uint8_t *)"\x40\xe2\x01\x00", 4U);
		break;
	case 0x042cU:
		/* User Confirmation Request Reply: paired, the link key, the authentication's end. */
		fake_complete(fake, opcode, (const uint8_t *)"\x00\x00\x00\x00\x00\x00\x00", 7U);
		if (!fake->connected)
			break;
		fake->confirmed = 1;
		parameters[0] = 0x00U;
		memcpy(parameters + 1, fake->device, 6U);
		fake_event(fake, 0x36U, parameters, 7U);
		memset(key, last, 16U);
		key_type = 0x08U;
		if (last == DEVICE_JUST_WORKS || fake->host_io == 0x03U)
			key_type = 0x07U;
		if (last == DEVICE_DEBUG)
			key_type = 0x03U;
		key[16] = key_type;
		fake_address_event(fake, 0x18U, fake->device, key, 17U);
		fake_event(fake, 0x06U, (const uint8_t *)"\x00\x40\x00", 3U);
		break;
	case 0x042dU:
		/* User Confirmation Request Negative Reply: the pairing fails (or a refused stranger). */
		fake_complete(fake, opcode, (const uint8_t *)"\x00\x00\x00\x00\x00\x00\x00", 7U);
		if (!fake->connected || body[0] != fake->device[0])
			break;
		parameters[0] = 0x05U;
		memcpy(parameters + 1, fake->device, 6U);
		fake_event(fake, 0x36U, parameters, 7U);
		fake_event(fake, 0x06U, (const uint8_t *)"\x05\x40\x00", 3U);
		break;
	case 0x0413U:
		/* Set Connection Encryption: on, with AES-CCM as a Secure Connections link has it (0x02, review S-a). */
		fake_status(fake, opcode, 0x00U);
		fake_event(fake, 0x08U, (const uint8_t *)"\x00\x40\x00\x02", 4U);
		break;
	case 0x1408U:
		/* Read Encryption Key Size: 16, or 7 for the short one. */
		returned[0] = 0x00U;
		returned[1] = body[0];
		returned[2] = body[1];
		returned[3] = 16U;
		if (last == DEVICE_SHORT)
			returned[3] = 7U;
		fake_complete(fake, opcode, returned, 4U);
		break;
	case 0x0406U:
		/* Disconnect: done, its buffers back. */
		fake_status(fake, opcode, 0x00U);
		parameters[0] = 0x00U;
		parameters[1] = body[0];
		parameters[2] = body[1];
		parameters[3] = 0x16U;
		if (body[0] == (uint8_t)HANDLE_BREDR) {
			fake->connected = 0;
			fake->bredr_outstanding = 0U;
		} else {
			fake->le_connected = 0;
			fake->le_outstanding = 0U;
		}

		/* The disconnection's end. */
		fake_event(fake, 0x05U, parameters, 4U);
		break;
	case 0x200dU:
		/* LE Create Connection: the mouse connects, any other never does. */
		fake_status(fake, opcode, 0x00U);
		memcpy(fake->device, body + 6, 6U);
		if (fake->device[0] != DEVICE_MOUSE_LE)
			break;
		memset(parameters, 0, sizeof(parameters));
		parameters[0] = 0x01U;
		parameters[1] = 0x00U;
		parameters[2] = (uint8_t)(HANDLE_LE & 0xffU);
		parameters[3] = (uint8_t)(HANDLE_LE >> 8);
		parameters[4] = 0x00U;
		parameters[5] = body[5];
		memcpy(parameters + 6, body + 6, 6U);
		fake->le_connected = 1;
		fake->le_step = 0U;
		fake_event(fake, 0x3eU, parameters, 19U);
		break;
	case 0x200eU:
		/* LE Create Connection Cancel: the connection's end, Unknown Connection Identifier. */
		fake_complete(fake, opcode, ok, sizeof(ok));
		memset(parameters, 0, sizeof(parameters));
		parameters[0] = 0x01U;
		parameters[1] = 0x02U;
		fake_event(fake, 0x3eU, parameters, 19U);
		break;
	case 0x2025U:
		/* LE Read Local P-256 Public Key: key A. */
		fake_status(fake, opcode, 0x00U);
		hex(key_a_x, returned);
		hex(key_a_y, returned + 32);
		parameters[0] = 0x08U;
		parameters[1] = 0x00U;
		reverse(parameters + 2, returned, 32U);
		reverse(parameters + 34, returned + 32, 32U);
		fake_event(fake, 0x3eU, parameters, 66U);
		break;
	case 0x2026U:
		/* LE Generate DHKey: the device's key must be B; the DHKey of A and B. */
		fake_status(fake, opcode, 0x00U);
		hex(key_b_x, returned);
		reverse(key, returned, 32U);
		differs = memcmp(key, body, 32U);
		if (differs != 0)
			fake->smp_errors++;
		hex(key_dh, returned);
		parameters[0] = 0x09U;
		parameters[1] = 0x00U;
		reverse(parameters + 2, returned, 32U);
		fake_event(fake, 0x3eU, parameters, 34U);
		break;
	case 0x2019U:
		/* LE Enable Encryption: the LTK must be f5's; then on, and the device's identity. */
		fake_status(fake, opcode, 0x00U);
		reverse(key, fake->ltk, 16U);
		differs = memcmp(key, body + 12, 16U);
		if (differs != 0)
			fake->smp_errors++;
		fake_event(fake, 0x08U, (const uint8_t *)"\x00\x41\x00\x01", 4U);
		memset(parameters, 0x11U, 17U);
		parameters[0] = 0x08U;
		fake_frame(fake, HANDLE_LE, 0x0006U, parameters, 17U);
		parameters[0] = 0x09U;
		parameters[1] = 0x01U;
		memcpy(parameters + 2, identity_address, 6U);
		fake_frame(fake, HANDLE_LE, 0x0006U, parameters, 8U);
		fake->le_step = 4U;
		break;
	default:
		/* Done, nothing to say (replies to strangers among them). */
		returned[0] = 0x00U;
		memset(returned + 1, 0, 6U);
		fake_complete(fake, opcode, returned, 7U);
		break;
	}

	/* The packet's length was checked by the reader. */
	(void)length;
}

/*
 * Takes an ACL packet: counts the controller's buffers (a packet past them
 * is a breach), puts frames together, and answers the signalling channel's
 * Information Request and the Security Manager's PDUs.  The buffers come
 * back when the pool is full or a frame is whole, so a host that does not
 * wait for them is caught.
 */
static void
fake_acl(
	struct fake *fake,
	const uint8_t *packet,
	size_t length)
{
	static const uint8_t information[12] = { 0x0bU, 0x00U, 0x08U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x80U, 0x00U, 0x00U, 0x00U };
	uint8_t response[12];
	struct btd_reassembly *frames;
	struct btd_acl acl;
	unsigned *outstanding;
	unsigned total;
	uint16_t cid;
	int whole;
	int error;

	/* The packet, and its connection's pool. */
	error = btd_acl_parse(packet, length, &acl);
	if (error != 0)
		return;
	frames = &fake->bredr_frames;
	outstanding = &fake->bredr_outstanding;
	total = fake->acl_total;
	if (acl.handle == HANDLE_LE) {
		fake->le_packets++;
		frames = &fake->le_frames;
		outstanding = &fake->le_outstanding;
		if (fake->le_total != 0U)
			total = fake->le_total;
	}

	/* A packet past the buffers, and a packet longer than LE's 27 bytes. */
	if (*outstanding >= total)
		fake->credit_breaches++;
	if (acl.handle == HANDLE_LE && fake->le_length != 0U && acl.length > fake->le_length)
		fake->credit_breaches++;
	*outstanding += 1U;

	/* The frame. */
	whole = btd_reassembly_feed(frames, &acl);

	/* The buffers back when the pool is full or the frame whole. */
	if (*outstanding >= total || whole != 0) {
		fake_completed(fake, acl.handle, *outstanding);
		*outstanding = 0U;
	}

	/* Only a whole frame is read. */
	if (whole <= 0)
		return;

	/* The frame's channel. */
	cid = (uint16_t)(frames->frame[2] | (frames->frame[3] << 8));

	/* BR/EDR's Information Request: the features. */
	if (acl.handle == HANDLE_BREDR && cid == 0x0001U && frames->frame[4] == 0x0aU) {
		fake->information_requests++;
		memcpy(response, information, sizeof(response));
		response[1] = frames->frame[5];
		fake_frame(fake, HANDLE_BREDR, 0x0001U, response, sizeof(response));
		return;
	}

	/* LE's Security Manager. */
	if (acl.handle == HANDLE_LE && cid == 0x0006U)
		fake_smp(fake, frames->frame + 4, frames->expected - 4U);
}

/*
 * Plays the LE device's Security Manager: Secure Connections' Just Works,
 * the device's IO NoInputNoOutput, its identity given.
 */
static void
fake_smp(
	struct fake *fake,
	const uint8_t *pdu,
	size_t length)
{
	uint8_t a_x[32];
	uint8_t b_x[32];
	uint8_t b_y[32];
	uint8_t dh[32];
	uint8_t a[7];
	uint8_t b[7];
	uint8_t r[16];
	uint8_t io[3];
	uint8_t na_msb[16];
	uint8_t nb_msb[16];
	uint8_t value[16];
	uint8_t expected[16];
	uint8_t out[65];
	int differs;

	/* Each PDU of the initiator. */
	switch (pdu[0]) {
	case 0x01U:
		/* Pairing Request: NoInputNoOutput, bonding and SC, 16 bytes, the identity offered. */
		if (length != 7U) {
			fake->smp_errors++;
			return;
		}

		/* The request kept, the response sent. */
		memcpy(fake->preq, pdu, 7U);
		fake->pres[0] = 0x02U;
		fake->pres[1] = 0x03U;
		fake->pres[2] = 0x00U;
		fake->pres[3] = 0x09U;
		fake->pres[4] = 16U;
		fake->pres[5] = 0x00U;
		fake->pres[6] = 0x02U;
		fake_frame(fake, HANDLE_LE, 0x0006U, fake->pres, 7U);
		fake->le_step = 1U;
		break;
	case 0x0cU:
		/* The initiator's public key must be A; then B, and Cb = f4(PKbx, PKax, Nb, 0). */
		hex(key_a_x, a_x);
		reverse(value, pdu + 1, 16U);
		differs = memcmp(a_x + 16, value, 16U);
		if (length != 65U || differs != 0)
			fake->smp_errors++;
		hex(key_b_x, b_x);
		hex(key_b_y, b_y);
		out[0] = 0x0cU;
		reverse(out + 1, b_x, 32U);
		reverse(out + 33, b_y, 32U);
		fake_frame(fake, HANDLE_LE, 0x0006U, out, 65U);
		memset(fake->nb, 0x5bU, sizeof(fake->nb));
		btd_smp_f4(b_x, a_x, fake->nb, 0U, value);
		out[0] = 0x03U;
		reverse(out + 1, value, 16U);
		fake_frame(fake, HANDLE_LE, 0x0006U, out, 17U);
		fake->le_step = 2U;
		break;
	case 0x04U:
		/* Na: Nb back, and f5's MacKey and LTK. */
		reverse(fake->na, pdu + 1, 16U);
		out[0] = 0x04U;
		reverse(out + 1, fake->nb, 16U);
		fake_frame(fake, HANDLE_LE, 0x0006U, out, 17U);
		hex(key_dh, dh);
		a[0] = 0x00U;
		reverse(a + 1, (const uint8_t *)"\x55\x44\x33\x22\x11\x00", 6U);
		b[0] = 0x00U;
		reverse(b + 1, fake->device, 6U);
		btd_smp_f5(dh, fake->na, fake->nb, a, b, fake->mac_key, fake->ltk);
		fake->le_step = 3U;
		break;
	case 0x0dU:
		/* Ea = f6(MacKey, Na, Nb, 0, IOcapA, A, B) must hold; Eb = f6(MacKey, Nb, Na, 0, IOcapB, B, A) goes back. */
		memset(r, 0, sizeof(r));
		a[0] = 0x00U;
		reverse(a + 1, (const uint8_t *)"\x55\x44\x33\x22\x11\x00", 6U);
		b[0] = 0x00U;
		reverse(b + 1, fake->device, 6U);
		memcpy(na_msb, fake->na, 16U);
		memcpy(nb_msb, fake->nb, 16U);
		io[0] = fake->preq[3];
		io[1] = fake->preq[2];
		io[2] = fake->preq[1];
		btd_smp_f6(fake->mac_key, na_msb, nb_msb, r, io, a, b, expected);
		reverse(value, pdu + 1, 16U);
		differs = memcmp(expected, value, 16U);
		if (length != 17U || differs != 0)
			fake->smp_errors++;
		io[0] = fake->pres[3];
		io[1] = fake->pres[2];
		io[2] = fake->pres[1];
		btd_smp_f6(fake->mac_key, nb_msb, na_msb, r, io, b, a, value);
		out[0] = 0x0dU;
		reverse(out + 1, value, 16U);
		fake_frame(fake, HANDLE_LE, 0x0006U, out, 17U);
		break;
	default:
		fake->smp_errors++;
		break;
	}
}

/* Writes an H4 packet to the session. */
static void
fake_send(
	struct fake *fake,
	const uint8_t *packet,
	size_t length)
{
	/* One packet. */
	(void)write(fake->descriptor, packet, length);
}

/* Writes an event with its parameters. */
static void
fake_event(
	struct fake *fake,
	uint8_t code,
	const uint8_t *parameters,
	size_t length)
{
	uint8_t packet[260];

	/* Type, code, length, parameters. */
	packet[0] = 0x04U;
	packet[1] = code;
	packet[2] = (uint8_t)length;
	memcpy(packet + 3, parameters, length);
	fake_send(fake, packet, 3U + length);
}

/* Writes a Command Complete for an opcode with its return parameters. */
static void
fake_complete(
	struct fake *fake,
	uint16_t opcode,
	const uint8_t *returned,
	size_t count)
{
	uint8_t parameters[260];

	/* Window, opcode, return parameters. */
	parameters[0] = 1U;
	parameters[1] = (uint8_t)(opcode & 0xffU);
	parameters[2] = (uint8_t)(opcode >> 8);
	memcpy(parameters + 3, returned, count);
	fake_event(fake, 0x0eU, parameters, 3U + count);
}

/* Writes a Command Status for an opcode. */
static void
fake_status(
	struct fake *fake,
	uint16_t opcode,
	uint8_t status)
{
	uint8_t parameters[4];

	/* Status, window, opcode. */
	parameters[0] = status;
	parameters[1] = 1U;
	parameters[2] = (uint8_t)(opcode & 0xffU);
	parameters[3] = (uint8_t)(opcode >> 8);
	fake_event(fake, 0x0fU, parameters, sizeof(parameters));
}

/* Writes an L2CAP frame to the host in one ACL packet. */
static void
fake_frame(
	struct fake *fake,
	uint16_t handle,
	uint16_t cid,
	const uint8_t *payload,
	size_t length)
{
	uint8_t frame[128];
	uint8_t packet[140];
	size_t frame_length;
	size_t packet_length;

	/* The frame in one packet. */
	frame_length = btd_l2cap_frame(frame, sizeof(frame), cid, payload, length);
	packet_length = btd_acl_build(packet, sizeof(packet), handle, 0x02U, frame, frame_length);
	fake_send(fake, packet, packet_length);
}

/* Writes Number Of Completed Packets for one handle. */
static void
fake_completed(
	struct fake *fake,
	uint16_t handle,
	unsigned count)
{
	uint8_t parameters[5];

	/* One handle and its count. */
	parameters[0] = 1U;
	parameters[1] = (uint8_t)(handle & 0xffU);
	parameters[2] = (uint8_t)(handle >> 8);
	parameters[3] = (uint8_t)(count & 0xffU);
	parameters[4] = (uint8_t)(count >> 8);
	fake_event(fake, 0x13U, parameters, sizeof(parameters));
}

/* Writes an event led by an address, with more parameters. */
static void
fake_address_event(
	struct fake *fake,
	uint8_t code,
	const uint8_t *address,
	const uint8_t *more,
	size_t more_length)
{
	uint8_t parameters[64];

	/* The address, then the rest. */
	memcpy(parameters, address, 6U);
	if (more_length != 0U)
		memcpy(parameters + 6, more, more_length);
	fake_event(fake, code, parameters, 6U + more_length);
}

/* Tells whether the controller saw a command. */
static int
fake_saw(
	const struct fake *fake,
	uint16_t opcode)
{
	unsigned index;

	/* Each command seen. */
	for (index = 0U; index < fake->opcode_count; index++) {
		if (fake->opcodes[index] == opcode)
			return 1;
	}

	/* Not seen. */
	return 0;
}

/* Writes the address 0A:0B:0C:0D:0E:last, least significant byte first. */
static void
device_address(
	uint8_t last,
	uint8_t *address)
{
	/* The six bytes. */
	address[0] = last;
	address[1] = 0x0eU;
	address[2] = 0x0dU;
	address[3] = 0x0cU;
	address[4] = 0x0bU;
	address[5] = 0x0aU;
}
