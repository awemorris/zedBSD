/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of bluetoothd's HID host over BR/EDR (ws143-p005 i02c,
 * plan/ws143/phase005/phase.md section 7.4): hid.c with the router, the
 * session, L2CAP and SDP, built with the host's compiler, against a
 * scripted controller on a socket pair that plays one HID device (its
 * page, its security, its L2CAP channels, its SDP records, its HIDP).  The
 * input bridge is the other end of a socket pair; its INPUT_BRIDGE_GET_DEVICE
 * is this file's ioctl.
 *
 *   connect     CONNECT through the page, the security, SDP over
 *               continuations, the channels, SET_PROTOCOL; the setup's
 *               bytes, the reports kept until it and passed after, a report
 *               too long, a request of the device answered unsupported,
 *               DISCONNECT, CONNECT again
 *   refusals    not bonded, busy, held, the key missing, a short key, the
 *               page failing, no HID record, a descriptor too long, the
 *               input devices full, the setup written again after EINVAL
 *               (and given up), the page's time out, the handshake not
 *               coming
 *   inbound     a device that connects by itself, asking for its channels
 *               before the security (Pending) and after it, its record
 *               kept; its own disconnection; refused after DISCONNECT
 *   handoff     a paired connection taken over (a peripheral's class) and
 *               not (a computer's)
 *   lifecycle   the pages again (5, 10, 20, 40, 60 seconds, paused after
 *               10, started again by CONNECT), the table full, the
 *               controller lost, the device's unplug, FORGET's unplug, a
 *               device let go for its pairing again
 *
 *   plan/ws143/tests/bt-daemon-host-test.sh
 */

#include "userland/base/bluetoothd/acl.h"
#include "userland/base/bluetoothd/hci.h"
#include "userland/base/bluetoothd/hid.h"
#include "userland/base/bluetoothd/hidcache.h"
#include "userland/base/bluetoothd/hidp.h"
#include "userland/base/bluetoothd/keys.h"
#include "userland/base/bluetoothd/l2cap.h"
#include "userland/base/bluetoothd/pair.h"
#include "userland/base/bluetoothd/router.h"
#include "userland/base/bluetoothd/sdp.h"
#include "userland/base/bluetoothd/session.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <uapi/input-bridge.h>
#include <unistd.h>

/* The device's connection handle, and the input node the stand-in kernel names. */
#define FAKE_HANDLE		0x0040U
#define FAKE_EVENT		7

/* How many channels the device holds, the first of its CIDs, and the first of those it asks for itself. */
#define FAKE_CHANNELS		4U
#define FAKE_CID_FIRST		0x0050U
#define FAKE_CID_ASKED		0x0070U

/* How the device answers SDP: its records, none for HID, a descriptor of 4097 bytes. */
#define SDP_NORMAL		0U
#define SDP_NO_HID		1U
#define SDP_BIG			2U

/* The most a built record holds, and the stack of open sequences. */
#define BUILD_MAX		12288U
#define BUILD_DEPTH		16U

/* A record being built: its bytes, and where each open sequence's length goes. */
struct build {
	uint8_t bytes[BUILD_MAX];
	size_t used;
	size_t open[BUILD_DEPTH];
	unsigned depth;
};

/*
 * One L2CAP channel of the device: its PSM, its CID and the host's, and
 * which configurations are done (ours answered by the host, the host's
 * answered by us).
 */
struct fake_channel {
	int used;
	uint16_t psm;
	uint16_t local;
	uint16_t remote;
	int ours_done;
	int theirs_done;
	int open;
};

/*
 * The scripted controller and its one device: how it behaves, its
 * connection, its channels, and what it saw.  The controller's thread and
 * the test's thread share it under lock; the test sets the behaviour
 * before it acts.
 */
struct fake {
	int descriptor;
	pthread_mutex_t lock;

	/* The device's behaviour. */
	uint8_t device[6];
	int page_fails;
	int page_silent;
	uint8_t auth_status;
	uint8_t key_size;
	unsigned sdp_kind;
	size_t chunk;
	int boot_device;
	int reconnect_initiate;
	int normally_connectable;
	int answer_handshake;
	int ask_first;
	unsigned reports_on_open;

	/* The connection: up, the host authenticating, encrypted; the frames put together. */
	int connected;
	int host_authenticates;
	int encrypted;
	struct btd_reassembly frames;

	/* The device's channels and its next identifier. */
	struct fake_channel channels[FAKE_CHANNELS];
	uint16_t next_cid;
	uint8_t next_identifier;

	/* What it saw. */
	uint16_t opcodes[512];
	unsigned opcode_count;
	uint8_t accept_role;
	uint8_t reject_reason;
	uint8_t scan_enable;
	unsigned sdp_requests;
	unsigned pending_answers;
	uint16_t last_result;
	unsigned set_protocols;
	unsigned unplugs;
	unsigned handshakes;
	uint8_t last_handshake;
	unsigned host_closes;
};

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/* The folder the script made, the run's folder of bonds in it, and the controller's address (00:11:22:33:44:55). */
static const char *base_folder;
static char part_folder[512];
static const char *keys_folder = part_folder;
static const uint8_t controller[6] = { 0x55U, 0x44U, 0x33U, 0x22U, 0x11U, 0x00U };

/*
 * The run's parts: the session, the pairing (the router's), the router,
 * the HID host, the controller and its thread.  One run at a time, on the
 * test's thread.
 */
static struct btd_session *session;
static struct btd_pair pairing;
static struct btd_router router;
static struct btd_hid host;
static struct fake fake;
static pthread_t fake_thread;

/*
 * The input bridge's stand-in: what the next opens answer (errors given
 * once each, then a socket pair), the test's end of the last pair, and how
 * many were opened.
 */
static int bridge_errors[8];
static unsigned bridge_error_count;
static int bridge_peer = -1;
static unsigned bridge_opens;

/* The last answer a client was given, and how many came. */
static char answer_line[BTD_HID_ANSWER_MAX];
static unsigned answers;

/* A connection the test claims for the handoff (the stand-in of a pairing's), or none. */
static int claim_handoff;

/* The boot keyboard's report descriptor (HID 1.11 Appendix E.6, 63 bytes). */
static const uint8_t keyboard_descriptor[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
	0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
	0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0,
};

static void expect(int condition, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void test_connect(void);
static void test_refusals(void);
static void test_inbound(void);
static void test_handoff(void);
static void test_lifecycle(void);
static void run_open(const char *part);
static void run_close(void);
static void settle(void);
static void tick_after(uint64_t milliseconds);
static void fake_reset(uint8_t last);
static void bond(uint8_t last, const char *name);
static void address_of(uint8_t last, uint8_t *address);
static int connect_device(uint8_t last);
static int bridge_read(uint8_t *bytes, size_t size, size_t *length);
static int bridge_closed(void);
static void status_of(uint8_t last, char *line, size_t size);
static int hook_bridge(void *context, int *descriptor);
static void hook_answer(void *context, const uint8_t *address, const char *line);
static int claims_hook(void *context, const uint8_t *address);
static void *fake_run(void *argument);
static void fake_command(const uint8_t *packet, size_t length);
static void fake_acl(const uint8_t *packet, size_t length);
static void fake_signal(const uint8_t *payload, size_t length);
static void fake_sdp(struct fake_channel *channel, const uint8_t *pdu, size_t length);
static void fake_control(struct fake_channel *channel, const uint8_t *frame, size_t length);
static void fake_channel_ready(struct fake_channel *channel);
static void fake_ask_channel(uint16_t psm);
static void fake_configure(struct fake_channel *channel);
static void fake_report(const uint8_t *report, size_t length);
static void fake_hidp(uint16_t psm, const uint8_t *message, size_t length);
static void fake_connect_request(void);
static void fake_hang_up(uint8_t reason);
static void fake_signal_send(uint8_t code, uint8_t identifier, const uint8_t *data, size_t length);
static void fake_event(uint8_t code, const uint8_t *parameters, size_t length);
static void fake_complete(uint16_t opcode, const uint8_t *returned, size_t count);
static void fake_status(uint16_t opcode);
static void fake_frame(uint16_t cid, const uint8_t *payload, size_t length);
static void fake_connected(uint8_t status);
static int fake_saw(uint16_t opcode);
static unsigned fake_count(uint16_t opcode);
static struct fake_channel *fake_by_local(uint16_t cid);
static struct fake_channel *fake_by_psm(uint16_t psm);
static void build_lists(struct build *build, uint16_t uuid);
static void put(struct build *build, const uint8_t *bytes, size_t length);
static void put_byte(struct build *build, uint8_t byte);
static void put_uint16(struct build *build, uint16_t value);
static void put_attribute(struct build *build, uint16_t id);
static void put_uuid(struct build *build, uint16_t uuid);
static void put_bool(struct build *build, int value);
static void put_text(struct build *build, const uint8_t *bytes, size_t length);
static void open_sequence(struct build *build);
static void close_sequence(struct build *build);
static void put16(uint8_t *bytes, uint16_t value);
static uint16_t get16(const uint8_t *bytes);

/*
 * Runs every part; the exit status says whether every check held.
 */
int
main(
	int argc,
	char **argv)
{
	/* The bonds' folder the script made; a closed bridge's write must not end the test. */
	base_folder = "build/tmp";
	if (argc > 1)
		base_folder = argv[1];
	(void)signal(SIGPIPE, SIG_IGN);

	/* The parts. */
	test_connect();
	test_refusals();
	test_inbound();
	test_handoff();
	test_lifecycle();

	/* The verdict. */
	if (failures != 0U) {
		printf("bt-hidhost-host-test: FAIL (%u of %u checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("bt-hidhost-host-test: PASS (%u checks)\n", checks);
	return 0;
}

/*
 * Answers INPUT_BRIDGE_GET_DEVICE as the kernel would (the node
 * FAKE_EVENT, no touch device) in place of the C library's ioctl, which
 * hid.c alone calls in this program.
 */
int
ioctl(
	int descriptor,
	unsigned long request,
	...)
{
	struct input_bridge_device *made;
	va_list arguments;

	/* Only the bridge's question, on an open bridge. */
	if (request != (unsigned long)INPUT_BRIDGE_GET_DEVICE || descriptor < 0) {
		errno = ENOTTY;
		return -1;
	}

	/* Succeeded: the nodes. */
	va_start(arguments, request);
	made = va_arg(arguments, struct input_bridge_device *);
	va_end(arguments);
	memset(made, 0, sizeof(*made));
	made->event = FAKE_EVENT;
	made->touch_event = -1;
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

	/* Counted. */
	checks++;
	if (condition)
		return;

	/* A failure, told. */
	failures++;
	va_start(arguments, format);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, format, arguments);
	fprintf(stderr, "\n");
	va_end(arguments);
}

/* CONNECT to the end and back: the setup, the reports, DISCONNECT, CONNECT again. */
static void
test_connect(void)
{
	static struct input_bridge_setup setup;
	static const uint8_t press[9] = { 0xa1U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
	static const uint8_t release[9] = { 0xa1U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
	static uint8_t long_report[601];
	struct btd_hidcache record;
	uint8_t address[6];
	uint8_t bytes[5000];
	char line[512];
	size_t length;
	int answered;
	int error;

	/* A bonded keyboard with the boot protocol, its records over continuations, its handshake answered. */
	run_open("connect");
	fake_reset(0x21U);
	bond(0x21U, "Test Keyboard");
	fake.boot_device = 1;
	fake.answer_handshake = 1;
	fake.reports_on_open = 2U;
	fake.chunk = 60U;

	/* CONNECT: the page, the security, the records, the channels, the handshake, the setup. */
	answered = connect_device(0x21U);
	expect(answered == 0, "connect: started (%d)", answered);
	settle();
	expect(answers == 1U && strstr(answer_line, "CONNECTED address=0A:0B:0C:0D:0E:21 type=bredr transport=hid input=/dev/input/event7") != NULL,
	       "connect: CONNECTED (%s)", answer_line);
	expect(strstr(answer_line, "name=\"Test Keyboard\"") != NULL && strstr(answer_line, "vendor=1209 product=4B42") != NULL,
	       "connect: its name and PnP numbers (%s)", answer_line);
	expect(fake_saw(0x0405U) && fake_saw(0x0411U) && fake_saw(0x040bU) && fake_saw(0x0413U) && fake_saw(0x1408U),
	       "connect: page, authentication, the key, encryption, the key's size");
	expect(fake.sdp_requests >= 4U, "connect: the HID and PnP records over continuations (%u requests)", fake.sdp_requests);
	expect(fake.set_protocols == 1U, "connect: SET_PROTOCOL to a boot device");

	/* The setup: Bluetooth, the PnP numbers, the names, the descriptor. */
	error = bridge_read((uint8_t *)&setup, sizeof(setup), &length);
	expect(error == 0 && length == sizeof(setup), "connect: the setup written whole (%zu)", length);
	expect(setup.magic == INPUT_BRIDGE_MAGIC && setup.version == INPUT_BRIDGE_VERSION && setup.bus == 0x05U,
	       "connect: the setup's magic, version and bus");
	expect(setup.vendor == 0x1209U && setup.product == 0x4b42U && setup.release == 0x0100U, "connect: the setup's PnP numbers");
	expect(strcmp(setup.name, "Test Keyboard") == 0, "connect: the setup's name (%s)", setup.name);
	expect(strcmp(setup.physical_path, "bluetooth/00:11:22:33:44:55/0A:0B:0C:0D:0E:21") == 0, "connect: the setup's path (%s)", setup.physical_path);
	expect(strcmp(setup.unique_id, "0A:0B:0C:0D:0E:21") == 0, "connect: the setup's unique ID (%s)", setup.unique_id);
	expect(setup.descriptor_size == sizeof(keyboard_descriptor) && memcmp(setup.descriptor, keyboard_descriptor, sizeof(keyboard_descriptor)) == 0,
	       "connect: the setup's descriptor");

	/* The reports that came with the channels, kept until the setup, then passed in order. */
	error = bridge_read(bytes, sizeof(bytes), &length);
	expect(error == 0 && length == 8U && bytes[2] == 0x04U, "connect: the first report kept and passed (%zu)", length);
	error = bridge_read(bytes, sizeof(bytes), &length);
	expect(error == 0 && length == 8U && bytes[2] == 0x00U, "connect: the second report kept and passed (%zu)", length);

	/* The record: confirmed, with the descriptor. */
	address_of(0x21U, address);
	error = btd_hidcache_read(keys_folder, controller, address, BTD_ADDRESS_BREDR, &record);
	expect(error == 0 && record.confirmed && record.descriptor_size == sizeof(keyboard_descriptor) && record.boot_device && record.vendor == 0x1209U,
	       "connect: the record confirmed (%d)", error);

	/* A report now goes straight on; one too long is dropped and counted. */
	fake_report(press, sizeof(press));
	settle();
	error = bridge_read(bytes, sizeof(bytes), &length);
	expect(error == 0 && length == 8U && bytes[2] == 0x04U, "connect: a report passed (%zu)", length);
	memset(long_report, 0x11, sizeof(long_report));
	long_report[0] = 0xa1U;
	fake_report(long_report, sizeof(long_report));
	fake_report(release, sizeof(release));
	settle();
	error = bridge_read(bytes, sizeof(bytes), &length);
	expect(error == 0 && length == 8U && bytes[2] == 0x00U, "connect: the long report dropped, the next passed (%zu)", length);
	status_of(0x21U, line, sizeof(line));
	expect(strstr(line, "state=open input=/dev/input/event7") != NULL && strstr(line, "reconnect=auto") != NULL &&
		       strstr(line, "reports=4 malformed=0 oversize=1") != NULL,
	       "connect: STATUS (%s)", line);

	/* A request of the device the host does not serve is answered unsupported. */
	bytes[0] = btd_hidp_header(BTD_HIDP_GET_REPORT, 0x1U);
	fake_hidp(BTD_SDP_PSM_CONTROL, bytes, 1U);
	settle();
	expect(fake.handshakes == 1U && fake.last_handshake == BTD_HIDP_UNSUPPORTED, "connect: GET_REPORT answered unsupported");

	/* CONNECT of an open device answers at once. */
	answered = connect_device(0x21U);
	expect(answered == 1 && strstr(answer_line, "CONNECTED") != NULL, "connect: CONNECT of an open device (%s)", answer_line);

	/* DISCONNECT: the channels (interrupt first), the link; the input device goes; not wanted back. */
	fake.opcode_count = 0U;
	fake.host_closes = 0U;
	error = btd_hid_disconnect(&host, address);
	settle();
	expect(error == 0 && fake.host_closes == 2U && fake_saw(0x0406U) && !fake.connected, "connect: DISCONNECT (%d, %u closes)", error, fake.host_closes);
	expect(bridge_closed(), "connect: the input device gone");
	status_of(0x21U, line, sizeof(line));
	expect(strstr(line, "state=idle input=-") != NULL && strstr(line, "reconnect=off") != NULL && strstr(line, "last=user") != NULL,
	       "connect: STATUS after DISCONNECT (%s)", line);
	error = btd_hid_disconnect(&host, address);
	expect(error == ENOTCONN, "connect: DISCONNECT of a device not connected (%d)", error);

	/* CONNECT again: open again, wanted back. */
	answers = 0U;
	answered = connect_device(0x21U);
	settle();
	expect(answered == 0 && answers == 1U && strstr(answer_line, "CONNECTED") != NULL, "connect: CONNECT again (%s)", answer_line);
	status_of(0x21U, line, sizeof(line));
	expect(strstr(line, "state=open") != NULL && strstr(line, "reconnect=auto") != NULL, "connect: open again (%s)", line);

	/* The run's end. */
	run_close();
}

/* CONNECT's refusals, each with its reason. */
static void
test_refusals(void)
{
	unsigned kept;
	int error;
	uint8_t address[6];
	uint8_t bytes[5000];
	char line[512];
	size_t length;
	int answered;
	int busy;

	/* Not bonded. */
	run_open("refusals");
	fake_reset(0x30U);
	answered = connect_device(0x30U);
	expect(answered == 1 && strcmp(answer_line, "ERROR not-bonded") == 0, "refusals: not bonded (%s)", answer_line);

	/* Busy: a second CONNECT while the first runs, a pairing asked to wait; held during a pairing or a scan. */
	bond(0x30U, "Busy");
	answered = connect_device(0x30U);
	address_of(0x30U, address);
	busy = btd_hid_busy(&host, address);
	expect(answered == 0 && busy == 1, "refusals: a connection under way makes a pairing wait");
	answered = connect_device(0x30U);
	expect(answered == 1 && strcmp(answer_line, "ERROR busy") == 0, "refusals: CONNECT while connecting (%s)", answer_line);
	settle();
	(void)bridge_read(bytes, sizeof(bytes), &length);
	(void)btd_hid_disconnect(&host, address);
	settle();
	btd_hid_hold(&host, 1);
	answered = connect_device(0x30U);
	expect(answered == 1 && strcmp(answer_line, "ERROR busy") == 0, "refusals: CONNECT while held (%s)", answer_line);
	btd_hid_hold(&host, 0);
	run_close();

	/* The key the device lost: the link ended. */
	run_open("refusals");
	fake_reset(0x31U);
	bond(0x31U, "Lost Key");
	fake.auth_status = 0x06U;
	answered = connect_device(0x31U);
	settle();
	expect(answered == 0 && strcmp(answer_line, "ERROR key-missing") == 0 && fake_saw(0x0406U), "refusals: the key missing (%s)", answer_line);

	/* A key shorter than 16 bytes (KNOB). */
	fake_reset(0x31U);
	fake.key_size = 7U;
	answered = connect_device(0x31U);
	settle();
	expect(answered == 0 && strcmp(answer_line, "ERROR key-size") == 0 && fake_saw(0x0406U), "refusals: a short key (%s)", answer_line);

	/* A page that fails: tried again later. */
	fake_reset(0x31U);
	fake.page_fails = 1;
	answered = connect_device(0x31U);
	settle();
	status_of(0x31U, line, sizeof(line));
	expect(answered == 0 && strcmp(answer_line, "ERROR unreachable") == 0 && strstr(line, "state=waiting") != NULL,
	       "refusals: unreachable, waiting (%s; %s)", answer_line, line);

	/* No HID record: the record goes. */
	fake_reset(0x31U);
	fake.sdp_kind = SDP_NO_HID;
	answered = connect_device(0x31U);
	settle();
	expect(answered == 0 && strcmp(answer_line, "ERROR no-hid") == 0 && fake_saw(0x0406U), "refusals: no HID record (%s)", answer_line);

	/* A descriptor past 4096 bytes. */
	fake_reset(0x31U);
	fake.sdp_kind = SDP_BIG;
	answered = connect_device(0x31U);
	settle();
	expect(answered == 0 && strcmp(answer_line, "ERROR descriptor") == 0, "refusals: a descriptor too long (%s)", answer_line);

	/* The input devices full (the kernel's ENOSPC). */
	fake_reset(0x31U);
	bridge_errors[0] = ENOSPC;
	bridge_error_count = 1U;
	answered = connect_device(0x31U);
	settle();
	expect(answered == 0 && strcmp(answer_line, "ERROR input-full") == 0, "refusals: the input devices full (%s)", answer_line);

	/* The setup refused with EINVAL twice: written again from the tick, then open. */
	fake_reset(0x31U);
	bridge_errors[0] = EINVAL;
	bridge_errors[1] = EINVAL;
	bridge_error_count = 2U;
	bridge_opens = 0U;
	answered = connect_device(0x31U);
	settle();
	status_of(0x31U, line, sizeof(line));
	expect(answered == 0 && answers == 0U && strstr(line, "state=connecting") != NULL, "refusals: the setup to be written again (%s)", line);
	tick_after(200U);
	tick_after(200U);
	expect(answers == 1U && strstr(answer_line, "CONNECTED") != NULL && bridge_opens == 3U, "refusals: the setup written the third time (%s, %u)",
	       answer_line, bridge_opens);
	(void)bridge_read(bytes, sizeof(bytes), &length);
	address_of(0x31U, address);
	(void)btd_hid_disconnect(&host, address);
	settle();

	/* Refused with EINVAL four times: given up. */
	fake_reset(0x31U);
	bridge_errors[0] = EINVAL;
	bridge_errors[1] = EINVAL;
	bridge_errors[2] = EINVAL;
	bridge_errors[3] = EINVAL;
	bridge_error_count = 4U;
	answered = connect_device(0x31U);
	settle();
	tick_after(200U);
	tick_after(200U);
	tick_after(200U);
	expect(answers == 1U && strcmp(answer_line, "ERROR descriptor") == 0, "refusals: the setup given up (%s)", answer_line);
	bridge_error_count = 0U;
	run_close();

	/* A page that never ends: its time runs out. */
	run_open("refusals");
	fake_reset(0x32U);
	bond(0x32U, "Silent");
	fake.page_silent = 1;
	answered = connect_device(0x32U);
	settle();
	tick_after(BTD_HID_PAGE_MS + 1000U);
	expect(answered == 0 && answers == 1U && strcmp(answer_line, "ERROR timeout") == 0, "refusals: the page's time out (%s)", answer_line);

	/* A handshake that does not come: the input device is made all the same; of 40 reports meanwhile, 32 are kept. */
	fake_reset(0x32U);
	fake.boot_device = 1;
	fake.reports_on_open = 40U;
	answered = connect_device(0x32U);
	settle();
	expect(answered == 0 && answers == 0U && fake.set_protocols == 1U, "refusals: waiting for the handshake");
	tick_after(BTD_HID_HANDSHAKE_MS + 500U);
	expect(answers == 1U && strstr(answer_line, "CONNECTED") != NULL, "refusals: open without the handshake (%s)", answer_line);
	(void)bridge_read(bytes, sizeof(bytes), &length);
	for (kept = 0U; kept < 40U; kept++) {
		error = bridge_read(bytes, sizeof(bytes), &length);
		if (error != 0)
			break;
	}

	/* The queue held 32; the rest were dropped and counted. */
	expect(kept == BTD_HID_QUEUE && host.devices[0].dropped == 40U - BTD_HID_QUEUE, "refusals: %u reports kept, %u dropped", kept,
	       host.devices[0].dropped);
	run_close();
}

/* A device that connects by itself, before and after its security, then goes; refused after DISCONNECT. */
static void
test_inbound(void)
{
	static struct input_bridge_setup setup;
	static const uint8_t move[4] = { 0xa1U, 0x00U, 0x05U, 0x00U };
	struct btd_hidcache record;
	uint8_t address[6];
	uint8_t bytes[5000];
	char line[512];
	size_t length;
	int error;

	/* A bonded mouse that reconnects by itself and is not connectable, its record kept with its descriptor. */
	run_open("inbound");
	fake_reset(0x22U);
	bond(0x22U, "Test Mouse");
	address_of(0x22U, address);
	memset(&record, 0, sizeof(record));
	memcpy(record.address, address, 6U);
	record.type = BTD_ADDRESS_BREDR;
	record.confirmed = 1;
	record.reconnect_initiate = 1;
	record.normally_connectable = 0;
	record.vendor = 0x1209U;
	record.product = 0x4d53U;
	(void)snprintf(record.name, sizeof(record.name), "%s", "Test Mouse");
	record.descriptor_size = sizeof(keyboard_descriptor);
	memcpy(record.descriptor, keyboard_descriptor, sizeof(keyboard_descriptor));
	error = btd_hidcache_write(keys_folder, controller, &record);
	expect(error == 0, "inbound: the record written (%d)", error);

	/* The controller ready: page scan on, no page (the device comes by itself). */
	btd_hid_refresh(&host);
	settle();
	tick_after(1000U);
	status_of(0x22U, line, sizeof(line));
	expect(fake.scan_enable == 0x02U && host.page_scan && !fake_saw(0x0405U) && strstr(line, "state=waiting") != NULL,
	       "inbound: page scan, waiting (%s)", line);

	/* It connects and asks for its channels before the security: Pending, then the security, then accepted. */
	fake.ask_first = 1;
	fake.reports_on_open = 1U;
	fake_connect_request();
	settle();
	status_of(0x22U, line, sizeof(line));
	expect(fake_saw(0x0409U) && fake.accept_role == 0x00U, "inbound: accepted as central (role %u)", fake.accept_role);
	expect(fake.pending_answers >= 1U && fake.last_result == BTD_L2CAP_SUCCESS, "inbound: Pending, then accepted (%u)", fake.pending_answers);
	expect(fake_saw(0x0411U) && fake_saw(0x0413U) && fake_saw(0x1408U), "inbound: the host authenticated and encrypted the link");
	expect(fake.sdp_requests == 0U && strstr(line, "state=open input=/dev/input/event7") != NULL, "inbound: open from the kept record (%s)", line);
	error = bridge_read((uint8_t *)&setup, sizeof(setup), &length);
	expect(error == 0 && length == sizeof(setup) && setup.product == 0x4d53U && setup.descriptor_size == sizeof(keyboard_descriptor),
	       "inbound: the setup from the record");
	error = bridge_read(bytes, sizeof(bytes), &length);
	expect(error == 0 && length == 8U, "inbound: the report that came with the channels (%zu)", length);

	/* It goes by itself: the input device goes, it is waited for (not paged). */
	fake.opcode_count = 0U;
	(void)pthread_mutex_lock(&fake.lock);
	fake_hang_up(0x08U);
	(void)pthread_mutex_unlock(&fake.lock);
	settle();
	tick_after(6000U);
	status_of(0x22U, line, sizeof(line));
	expect(bridge_closed() && strstr(line, "state=waiting") != NULL && strstr(line, "last=lost") != NULL && !fake_saw(0x0405U),
	       "inbound: gone, waited for (%s)", line);

	/* It comes again and authenticates itself first: its channels after the encryption. */
	fake.ask_first = 0;
	fake.reports_on_open = 0U;
	fake.pending_answers = 0U;
	fake_connect_request();
	settle();
	status_of(0x22U, line, sizeof(line));
	expect(strstr(line, "state=open") != NULL && fake.pending_answers == 0U && !fake_saw(0x0411U), "inbound: open again, no Pending (%s)", line);
	(void)bridge_read((uint8_t *)&setup, sizeof(setup), &length);
	fake_report(move, sizeof(move));
	settle();
	error = bridge_read(bytes, sizeof(bytes), &length);
	expect(error == 0 && length == 3U && bytes[1] == 0x05U, "inbound: its report (%zu)", length);

	/* DISCONNECT: its next connection is refused (the user's decision Q5). */
	(void)btd_hid_disconnect(&host, address);
	settle();
	fake.opcode_count = 0U;
	fake_connect_request();
	settle();
	expect(fake_saw(0x040aU) && !fake_saw(0x0409U), "inbound: refused after DISCONNECT");
	run_close();
}

/* A paired connection handed to the HID host, or left to the pairing. */
static void
test_handoff(void)
{
	static struct input_bridge_setup setup;
	struct btd_hidcache record;
	struct btd_device *seen;
	struct btd_bond paired;
	uint8_t address[6];
	char line[512];
	size_t length;
	int taken;
	int error;

	/* A keyboard the last scan saw (a peripheral's class), paired: its connection made, owned by nobody yet. */
	run_open("handoff");
	fake_reset(0x23U);
	bond(0x23U, "Paired Keyboard");
	address_of(0x23U, address);
	seen = &session->devices.entries[0];
	memset(seen, 0, sizeof(*seen));
	memcpy(seen->address, address, 6U);
	seen->type = BTD_ADDRESS_BREDR;
	seen->has_class = 1;
	seen->class_of_device = 0x002540U;
	session->devices.count = 1U;
	claim_handoff = 1;
	(void)pthread_mutex_lock(&fake.lock);
	fake_connected(0x00U);
	(void)pthread_mutex_unlock(&fake.lock);
	settle();
	claim_handoff = 0;

	/* Handed over: on the same link, its records, its channels, open; the record a candidate, then confirmed. */
	error = btd_keys_read(keys_folder, controller, address, BTD_ADDRESS_BREDR, &paired);
	taken = btd_hid_handoff(&host, address, BTD_ADDRESS_BREDR, FAKE_HANDLE, &paired);
	expect(error == 0 && taken == 1 && btd_router_owner(&router, FAKE_HANDLE) == BTD_OWNER_HID, "handoff: taken");
	settle();
	status_of(0x23U, line, sizeof(line));
	expect(strstr(line, "state=open input=/dev/input/event7") != NULL && strstr(line, "name=\"Paired Keyboard\"") != NULL, "handoff: open (%s)", line);
	expect(!fake_saw(0x0405U) && !fake_saw(0x0411U) && fake.sdp_requests >= 2U, "handoff: no page nor authentication, SDP on the link");
	expect(fake.scan_enable == 0x02U && host.page_scan, "handoff: page scan on, the device may come back by itself");
	error = bridge_read((uint8_t *)&setup, sizeof(setup), &length);
	expect(error == 0 && length == sizeof(setup) && strcmp(setup.name, "Paired Keyboard") == 0, "handoff: the setup");
	error = btd_hidcache_read(keys_folder, controller, address, BTD_ADDRESS_BREDR, &record);
	expect(error == 0 && record.confirmed && record.device_class == 0x002540U, "handoff: the record confirmed (%d)", error);

	/* A computer's class: not taken, no record. */
	address_of(0x24U, address);
	seen->class_of_device = 0x000104U;
	memcpy(seen->address, address, 6U);
	bond(0x24U, "Computer");
	error = btd_keys_read(keys_folder, controller, address, BTD_ADDRESS_BREDR, &paired);
	taken = btd_hid_handoff(&host, address, BTD_ADDRESS_BREDR, 0x0041U, &paired);
	expect(error == 0 && taken == 0, "handoff: a computer's connection left to the pairing");
	error = btd_hidcache_read(keys_folder, controller, address, BTD_ADDRESS_BREDR, &record);
	expect(error != 0, "handoff: no record for it (%d)", error);
	run_close();
}

/* The pages again and their pause, the table full, the controller lost, the unplugs. */
static void
test_lifecycle(void)
{
	static const uint64_t waits[5] = { 5000U, 10000U, 20000U, 40000U, 60000U };
	static struct btd_hidcache record;
	struct btd_bond gone;
	uint8_t address[6];
	uint8_t bytes[5000];
	uint8_t message[1];
	char line[512];
	uint64_t deadline;
	uint64_t wait;
	uint64_t now;
	unsigned index;
	unsigned pages;
	unsigned paged;
	size_t length;
	int answered;
	int error;
	int close_enough;

	/* A keyboard whose page always fails. */
	run_open("lifecycle");
	fake_reset(0x40U);
	bond(0x40U, "Away");
	fake.page_fails = 1;
	answered = connect_device(0x40U);
	settle();
	expect(answered == 0 && strcmp(answer_line, "ERROR unreachable") == 0, "lifecycle: unreachable (%s)", answer_line);

	/* The pages again: 5, 10, 20, 40, then 60 seconds apart. */
	close_enough = 1;
	for (index = 0U; index < BTD_HID_RETRIES; index++) {
		deadline = btd_hid_deadline(&host);
		wait = waits[4];
		if (index < 5U)
			wait = waits[index];
		now = btd_now_ms();
		if (deadline + 1000U < now + wait || deadline > now + wait + 1000U)
			close_enough = 0;

		/* The page at that time, one more Create Connection. */
		pages = fake_count(0x0405U);
		btd_hid_tick(&host, deadline);
		settle();
		paged = fake_count(0x0405U);
		if (paged != pages + 1U)
			close_enough = 0;
	}

	/* Paused after so many: no time left to page. */
	status_of(0x40U, line, sizeof(line));
	expect(close_enough, "lifecycle: paged again at 5, 10, 20, 40, 60 seconds");
	expect(strstr(line, "reconnect=paused") != NULL && btd_hid_deadline(&host) == 0U, "lifecycle: paused after %u pages (%s)", BTD_HID_RETRIES, line);

	/* CONNECT starts them again. */
	pages = fake_count(0x0405U);
	answered = connect_device(0x40U);
	settle();
	status_of(0x40U, line, sizeof(line));
	expect(answered == 0 && fake_count(0x0405U) == pages + 1U && strstr(line, "reconnect=auto") != NULL, "lifecycle: CONNECT pages again (%s)", line);
	run_close();

	/* The table full: a seventh device is refused. */
	run_open("lifecycle");
	for (index = 0U; index < BTD_HID_MAX + 1U; index++)
		bond((uint8_t)(0x50U + index), "Many");
	for (index = 0U; index < BTD_HID_MAX; index++) {
		fake_reset((uint8_t)(0x50U + index));
		fake.page_fails = 1;
		(void)connect_device((uint8_t)(0x50U + index));
		settle();
	}

	/* The seventh. */
	answered = connect_device((uint8_t)(0x50U + BTD_HID_MAX));
	expect(answered == 1 && strcmp(answer_line, "ERROR limit") == 0, "lifecycle: the table full (%s)", answer_line);
	run_close();

	/* The controller lost: the input device goes, the device stays wanted. */
	run_open("lifecycle");
	fake_reset(0x41U);
	bond(0x41U, "Lost");
	(void)connect_device(0x41U);
	settle();
	(void)bridge_read(bytes, sizeof(bytes), &length);
	btd_hid_lost(&host);
	status_of(0x41U, line, sizeof(line));
	expect(bridge_closed() && strstr(line, "state=waiting") != NULL && strstr(line, "last=lost") != NULL && !host.page_scan,
	       "lifecycle: the controller lost (%s)", line);
	run_close();

	/* The device's unplug: its bond and record go, no unplug sent back, the link ended. */
	run_open("lifecycle");
	fake_reset(0x42U);
	bond(0x42U, "Unplug");
	(void)connect_device(0x42U);
	settle();
	(void)bridge_read(bytes, sizeof(bytes), &length);
	fake.opcode_count = 0U;
	message[0] = btd_hidp_header(BTD_HIDP_CONTROL, BTD_HIDP_VIRTUAL_CABLE_UNPLUG);
	fake_hidp(BTD_SDP_PSM_CONTROL, message, 1U);
	settle();
	address_of(0x42U, address);
	error = btd_keys_read(keys_folder, controller, address, BTD_ADDRESS_BREDR, &gone);
	status_of(0x42U, line, sizeof(line));
	expect(error != 0 && line[0] == '\0' && fake.unplugs == 0U && fake_saw(0x0406U) && bridge_closed(), "lifecycle: the device's unplug (%d)", error);

	/* FORGET of a connected device: it hears the unplug, the link ends, the record goes. */
	fake_reset(0x43U);
	bond(0x43U, "Forget");
	(void)connect_device(0x43U);
	settle();
	(void)bridge_read(bytes, sizeof(bytes), &length);
	fake.opcode_count = 0U;
	address_of(0x43U, address);
	btd_hid_forget(&host, address);
	settle();
	status_of(0x43U, line, sizeof(line));
	expect(fake.unplugs == 1U && fake_saw(0x0406U) && line[0] == '\0' && bridge_closed(), "lifecycle: FORGET's unplug");

	/* PAIR of an open device lets it go first: no unplug, the link ended, the record gone, the bond kept. */
	fake_reset(0x44U);
	bond(0x44U, "Paired Again");
	(void)connect_device(0x44U);
	settle();
	(void)bridge_read(bytes, sizeof(bytes), &length);
	fake.opcode_count = 0U;
	address_of(0x44U, address);
	btd_hid_release(&host, address);
	settle();
	status_of(0x44U, line, sizeof(line));
	error = btd_keys_read(keys_folder, controller, address, BTD_ADDRESS_BREDR, &gone);
	expect(fake.unplugs == 0U && fake_saw(0x0406U) && line[0] == '\0' && bridge_closed() && error == 0, "lifecycle: released for a pairing (%d)", error);
	error = btd_hidcache_read(keys_folder, controller, address, BTD_ADDRESS_BREDR, &record);
	expect(error != 0, "lifecycle: the released device's record gone (%d)", error);
	run_close();
}

/* Starts a run of a part (its bonds in a folder of its own): the controller on a socket pair, the session ready, the router, the HID host. */
static void
run_open(
	const char *part)
{
	struct btd_router_hid router_hooks;
	struct btd_hid_hooks hooks;
	int ends[2];
	int status;

	/* The part's folder of bonds (made once; a run of the same part keeps its bonds). */
	(void)snprintf(part_folder, sizeof(part_folder), "%s/%s", base_folder, part);
	status = mkdir(part_folder, 0700);
	if (status != 0 && errno != EEXIST) {
		perror("mkdir");
		exit(2);
	}

	/* A socket pair that keeps packets apart, as the node does. */
	status = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ends);
	if (status != 0) {
		perror("socketpair");
		exit(2);
	}

	/* The session, ready, with the controller's address and buffers (no start: the start is bt-link-host-test's). */
	session = calloc(1U, sizeof(*session));
	if (session == NULL) {
		perror("calloc");
		exit(2);
	}

	/* Its node is the socket, its controller started already. */
	btd_session_init(session, ends[0], NULL, NULL, "fake", "/nonexistent");
	session->timing.command_ms = 500U;
	session->state = BTD_STATE_READY;
	session->have_address = 1;
	memcpy(session->address, controller, 6U);
	session->acl_pool.length = 1021U;
	session->acl_pool.total = 8U;
	session->acl_pool.free = 8U;
	session->le_shared = 1;

	/* The router with the pairing, as the daemon has them. */
	btd_pair_init(&pairing, session, keys_folder, NULL, NULL, NULL, NULL, NULL);
	btd_router_init(&router, &pairing);
	session->handler = btd_router_handle;
	session->handler_context = &router;

	/* The HID host, the router's owner of its connections (claims through the test's hook, for the handoff). */
	memset(&hooks, 0, sizeof(hooks));
	hooks.open_bridge = hook_bridge;
	hooks.answer = hook_answer;
	btd_hid_init(&host, session, keys_folder, &router, &hooks);
	router_hooks.context = &host;
	router_hooks.wants = btd_hid_wants;
	router_hooks.claims = claims_hook;
	router_hooks.handle = btd_hid_handle;
	btd_router_set_hid(&router, &router_hooks);

	/* The controller. */
	memset(&fake, 0, sizeof(fake));
	fake.descriptor = ends[1];
	(void)pthread_mutex_init(&fake.lock, NULL);
	status = pthread_create(&fake_thread, NULL, fake_run, NULL);
	if (status != 0) {
		perror("pthread_create");
		exit(2);
	}

	/* No answer yet, no bridge. */
	answers = 0U;
	answer_line[0] = '\0';
	bridge_error_count = 0U;
	bridge_opens = 0U;
	bridge_peer = -1;
}

/* Ends a run: the session's end closes, the controller sees it and returns. */
static void
run_close(void)
{
	/* The session's end, then the controller. */
	(void)shutdown(session->descriptor, SHUT_RDWR);
	(void)pthread_join(fake_thread, NULL);
	(void)close(session->descriptor);
	(void)close(fake.descriptor);
	(void)pthread_mutex_destroy(&fake.lock);

	/* The bridge's end and the HID host's descriptors. */
	btd_hid_lost(&host);
	if (bridge_peer >= 0)
		(void)close(bridge_peer);
	bridge_peer = -1;
	free(session);
	session = NULL;
}

/* Hands every packet to the session until the controller is quiet for a while. */
static void
settle(void)
{
	struct pollfd descriptor;
	unsigned rounds;
	unsigned quiet;
	int error;

	/* Until quiet five times in a row, 4000 rounds at most. */
	quiet = 0U;
	for (rounds = 0U; rounds < 4000U && quiet < 5U; rounds++) {
		/* A packet, or a short wait for one. */
		error = btd_session_input(session);
		if (error == 0) {
			quiet = 0U;
			continue;
		}

		/* Nothing now (any other error ends it). */
		if (error != EAGAIN)
			break;
		descriptor.fd = session->descriptor;
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		(void)poll(&descriptor, 1U, 10);
		if (descriptor.revents != 0) {
			quiet = 0U;
			continue;
		}

		/* Quiet once more. */
		quiet++;
	}
}

/* Moves the HID host's clock on by some milliseconds (from now), then lets the controller answer. */
static void
tick_after(
	uint64_t milliseconds)
{
	/* The tick at that time. */
	btd_hid_tick(&host, btd_now_ms() + milliseconds);
	settle();
}

/* Sets the device 0A:0B:0C:0D:0E:last with the default behaviour: a keyboard that answers everything. */
static void
fake_reset(
	uint8_t last)
{
	/* The device and its behaviour. */
	(void)pthread_mutex_lock(&fake.lock);

	address_of(last, fake.device);
	fake.page_fails = 0;
	fake.page_silent = 0;
	fake.auth_status = 0x00U;
	fake.key_size = 16U;
	fake.sdp_kind = SDP_NORMAL;
	fake.chunk = 640U;
	fake.boot_device = 0;
	fake.reconnect_initiate = 0;
	fake.normally_connectable = 1;
	fake.answer_handshake = 0;
	fake.ask_first = 0;
	fake.reports_on_open = 0U;
	fake.opcode_count = 0U;
	fake.sdp_requests = 0U;
	fake.pending_answers = 0U;
	fake.set_protocols = 0U;
	fake.unplugs = 0U;
	fake.handshakes = 0U;
	fake.host_closes = 0U;

	(void)pthread_mutex_unlock(&fake.lock);

	/* No answer yet. */
	answers = 0U;
	answer_line[0] = '\0';
}

/* Writes a bond with a link key for the device 0A:0B:0C:0D:0E:last. */
static void
bond(
	uint8_t last,
	const char *name)
{
	struct btd_bond made;
	int error;

	/* The bond. */
	memset(&made, 0, sizeof(made));
	address_of(last, made.address);
	made.type = BTD_ADDRESS_BREDR;
	(void)snprintf(made.name, sizeof(made.name), "%s", name);
	made.have_link_key = 1;
	memset(made.link_key, 0x5a, sizeof(made.link_key));
	made.link_key_type = 0x07U;
	made.key_size = 16U;
	made.secure = 1;
	error = btd_keys_write(keys_folder, controller, &made);
	if (error != 0) {
		fprintf(stderr, "bt-hidhost-host-test: a bond not written (%d)\n", error);
		exit(2);
	}
}

/* Writes the address 0A:0B:0C:0D:0E:last, least significant byte first. */
static void
address_of(
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

/* CONNECTs the device 0A:0B:0C:0D:0E:last; an answer given now is the last answer. */
static int
connect_device(
	uint8_t last)
{
	uint8_t address[6];
	char answer[BTD_HID_ANSWER_MAX];
	int answered;

	/* The request. */
	address_of(last, address);
	answer[0] = '\0';
	answered = btd_hid_connect(&host, address, answer, sizeof(answer));
	if (answered)
		(void)snprintf(answer_line, sizeof(answer_line), "%s", answer);

	/* Succeeded: answered now (1) or started (0). */
	return answered;
}

/* Reads one write of the bridge (waiting a little); returns 0, or EAGAIN when none came. */
static int
bridge_read(
	uint8_t *bytes,
	size_t size,
	size_t *length)
{
	struct pollfd descriptor;
	ssize_t got;

	/* Nothing without a bridge. */
	*length = 0U;
	if (bridge_peer < 0)
		return EAGAIN;

	/* A write, within a moment. */
	descriptor.fd = bridge_peer;
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	(void)poll(&descriptor, 1U, 100);
	if (descriptor.revents == 0)
		return EAGAIN;
	got = recv(bridge_peer, bytes, size, MSG_DONTWAIT);
	if (got <= 0)
		return EAGAIN;

	/* Succeeded: one write. */
	*length = (size_t)got;
	return 0;
}

/* Tells whether the HID host closed the bridge (its end reads the end). */
static int
bridge_closed(void)
{
	uint8_t byte;
	ssize_t got;

	/* The end of the pair, after any write left unread. */
	if (bridge_peer < 0)
		return 0;
	for (;;) {
		got = recv(bridge_peer, &byte, sizeof(byte), MSG_DONTWAIT | MSG_TRUNC);
		if (got <= 0)
			break;
	}

	/* Succeeded: closed when the read ends. */
	if (got == 0)
		return 1;
	return 0;
}

/* Writes the STATUS line of the device 0A:0B:0C:0D:0E:last, or an empty line. */
static void
status_of(
	uint8_t last,
	char *line,
	size_t size)
{
	uint8_t address[6];
	unsigned index;
	int differs;

	/* The device's slot. */
	line[0] = '\0';
	address_of(last, address);
	for (index = 0U; index < BTD_HID_MAX; index++) {
		if (!host.devices[index].used)
			continue;
		differs = memcmp(host.devices[index].address, address, 6U);
		if (differs != 0)
			continue;

		/* Succeeded: its line. */
		btd_hid_status(&host, index, line, size);
		return;
	}
}

/* The bridge's stand-in: an error asked for, or a new socket pair whose other end the test reads. */
static int
hook_bridge(
	void *context,
	int *descriptor)
{
	int ends[2];
	int status;
	unsigned index;

	/* An error asked for, given once. */
	(void)context;
	bridge_opens++;
	if (bridge_error_count != 0U) {
		status = bridge_errors[0];
		for (index = 1U; index < bridge_error_count; index++)
			bridge_errors[index - 1U] = bridge_errors[index];
		bridge_error_count--;
		return status;
	}

	/* A pair that keeps writes apart; the test's end of the last is kept. */
	status = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ends);
	if (status != 0)
		return errno;
	if (bridge_peer >= 0)
		(void)close(bridge_peer);
	bridge_peer = ends[1];

	/* Succeeded: the HID host's end. */
	*descriptor = ends[0];
	return 0;
}

/* The client's stand-in: keeps the answer. */
static void
hook_answer(
	void *context,
	const uint8_t *address,
	const char *line)
{
	/* The line, counted. */
	(void)context;
	(void)address;
	(void)snprintf(answer_line, sizeof(answer_line), "%s", line);
	answers++;
}

/* The router's claims: the HID host's, or the test's for a handed-over connection. */
static int
claims_hook(
	void *context,
	const uint8_t *address)
{
	int claimed;

	/* The handoff's connection is claimed while the test makes it. */
	if (claim_handoff)
		return 1;

	/* Succeeded: the HID host's answer. */
	claimed = btd_hid_claims(context, address);
	return claimed;
}

/* The scripted controller: answers each command and ACL packet until its socket closes. */
static void *
fake_run(
	void *argument)
{
	uint8_t packet[1200];
	ssize_t got;

	/* Plays until the session's end closes. */
	(void)argument;
	for (;;) {
		/* The next packet. */
		got = read(fake.descriptor, packet, sizeof(packet));
		if (got <= 0)
			return NULL;

		/* A command, or ACL data. */
		(void)pthread_mutex_lock(&fake.lock);

		if (packet[0] == 0x01U && got >= 4)
			fake_command(packet, (size_t)got);
		if (packet[0] == 0x02U && got >= 5)
			fake_acl(packet, (size_t)got);

		(void)pthread_mutex_unlock(&fake.lock);
	}
}

/* Answers one command as the controller and its device. */
static void
fake_command(
	const uint8_t *packet,
	size_t length)
{
	static const uint8_t ok[1] = { 0x00U };
	uint8_t returned[8];
	uint8_t parameters[16];
	const uint8_t *body;
	uint16_t opcode;

	/* The command, recorded. */
	(void)length;
	opcode = get16(packet + 1);
	body = packet + 4;
	if (fake.opcode_count < 512U)
		fake.opcodes[fake.opcode_count++] = opcode;

	/* Each command's answer. */
	switch (opcode) {
	case 0x0405U:
		/* Create Connection: the device answers its page, or fails it, or keeps silent. */
		fake_status(opcode);
		if (fake.page_silent)
			break;
		if (fake.page_fails) {
			fake_connected(0x04U);
			break;
		}

		/* The page answered: the host authenticates next. */
		fake.host_authenticates = 0;
		fake_connected(0x00U);
		break;
	case 0x0409U:
		/* Accept Connection Request: the connection, then the device's channel or its own security. */
		fake.accept_role = body[6];
		fake_status(opcode);
		fake_connected(0x00U);
		fake.host_authenticates = 0;
		if (fake.ask_first) {
			fake_ask_channel(BTD_SDP_PSM_CONTROL);
			break;
		}

		/* The device authenticates itself: the controller asks for the key. */
		fake_event(0x17U, fake.device, 6U);
		break;
	case 0x040aU:
		/* Reject Connection Request. */
		fake.reject_reason = body[6];
		fake_status(opcode);
		break;
	case 0x0411U:
		/* Authentication Requested: the controller asks for the key. */
		fake.host_authenticates = 1;
		fake_status(opcode);
		fake_event(0x17U, fake.device, 6U);
		break;
	case 0x040bU:
	case 0x040cU:
		/* The key (or none): the authentication's end, or the device's own encryption. */
		fake_complete(opcode, fake.device, 6U);
		if (fake.host_authenticates) {
			parameters[0] = fake.auth_status;
			if (opcode == 0x040cU)
				parameters[0] = 0x06U;
			put16(parameters + 1, FAKE_HANDLE);
			fake_event(0x06U, parameters, 3U);
			break;
		}

		/* The device encrypts the link itself and asks for its channels. */
		parameters[0] = 0x00U;
		put16(parameters + 1, FAKE_HANDLE);
		parameters[3] = 0x01U;
		fake.encrypted = 1;
		fake_event(0x08U, parameters, 4U);
		fake_ask_channel(BTD_SDP_PSM_CONTROL);
		break;
	case 0x0413U:
		/* Set Connection Encryption: on. */
		fake_status(opcode);
		parameters[0] = 0x00U;
		put16(parameters + 1, FAKE_HANDLE);
		parameters[3] = 0x01U;
		fake.encrypted = 1;
		fake_event(0x08U, parameters, 4U);
		break;
	case 0x1408U:
		/* Read Encryption Key Size. */
		returned[0] = 0x00U;
		put16(returned + 1, FAKE_HANDLE);
		returned[3] = fake.key_size;
		fake_complete(opcode, returned, 4U);
		break;
	case 0x0406U:
		/* Disconnect: the connection ends. */
		fake_status(opcode);
		fake_hang_up(0x16U);
		break;
	case 0x0c1aU:
		/* Write Scan Enable. */
		fake.scan_enable = body[0];
		fake_complete(opcode, ok, sizeof(ok));
		break;
	default:
		fake_complete(opcode, ok, sizeof(ok));
		break;
	}
}

/* Takes an ACL packet of the host: its buffer given back, a whole frame to its channel. */
static void
fake_acl(
	const uint8_t *packet,
	size_t length)
{
	struct fake_channel *channel;
	struct btd_acl acl;
	uint8_t completed[5];
	const uint8_t *payload;
	size_t payload_length;
	uint16_t cid;
	int whole;
	int error;

	/* The packet, its buffer given back at once. */
	error = btd_acl_parse(packet, length, &acl);
	if (error != 0)
		return;
	completed[0] = 1U;
	put16(completed + 1, acl.handle);
	put16(completed + 3, 1U);
	fake_event(0x13U, completed, sizeof(completed));

	/* The frame, once whole. */
	whole = btd_reassembly_feed(&fake.frames, &acl);
	if (whole <= 0)
		return;
	payload = fake.frames.frame + BTD_L2CAP_HEADER;
	payload_length = fake.frames.expected - BTD_L2CAP_HEADER;
	cid = get16(fake.frames.frame + 2);

	/* The signalling channel, or one of the device's. */
	if (cid == BTD_CID_SIGNALLING) {
		fake_signal(payload, payload_length);
		return;
	}

	/* The device's channel the frame is for. */
	channel = fake_by_local(cid);
	if (channel == NULL)
		return;

	/* SDP's or the control channel's (the interrupt channel takes nothing from the host). */
	if (channel->psm == BTD_SDP_PSM_SDP)
		fake_sdp(channel, payload, payload_length);
	if (channel->psm == BTD_SDP_PSM_CONTROL)
		fake_control(channel, payload, payload_length);
}

/* Takes the host's signalling commands as the device. */
static void
fake_signal(
	const uint8_t *payload,
	size_t length)
{
	struct fake_channel *channel;
	uint8_t answer[8];
	size_t offset;
	size_t data_length;
	uint8_t code;
	uint8_t identifier;
	const uint8_t *data;

	/* Each command in turn. */
	offset = 0U;
	while (offset + 4U <= length) {
		code = payload[offset];
		identifier = payload[offset + 1U];
		data_length = get16(payload + offset + 2U);
		data = payload + offset + 4U;
		offset += 4U + data_length;
		if (offset > length)
			return;

		/* Each code the device answers. */
		switch (code) {
		case 0x02U:
			/* Connection Request: accepted on a new channel of the device, its configuration next. */
			channel = fake_by_psm(0U);
			if (channel == NULL)
				break;
			memset(channel, 0, sizeof(*channel));
			channel->used = 1;
			channel->psm = get16(data);
			channel->remote = get16(data + 2);
			channel->local = fake.next_cid++;
			put16(answer, channel->local);
			put16(answer + 2, channel->remote);
			put16(answer + 4, 0U);
			put16(answer + 6, 0U);
			fake_signal_send(0x03U, identifier, answer, 8U);
			fake_configure(channel);
			break;
		case 0x03U:
			/* Connection Response to the device's request: pending, accepted (configured next), or refused. */
			channel = fake_by_local(get16(data + 2));
			if (channel == NULL)
				break;
			fake.last_result = get16(data + 4);
			if (fake.last_result == BTD_L2CAP_PENDING) {
				fake.pending_answers++;
				break;
			}

			/* A refusal frees the channel. */
			if (fake.last_result != BTD_L2CAP_SUCCESS) {
				memset(channel, 0, sizeof(*channel));
				break;
			}

			/* Accepted: the host's CID, the device's configuration next. */
			channel->remote = get16(data);
			fake_configure(channel);
			break;
		case 0x04U:
			/* Configure Request: accepted as it is. */
			channel = fake_by_local(get16(data));
			if (channel == NULL)
				break;
			put16(answer, channel->remote);
			put16(answer + 2, 0U);
			put16(answer + 4, 0U);
			fake_signal_send(0x05U, identifier, answer, 6U);
			channel->theirs_done = 1;
			fake_channel_ready(channel);
			break;
		case 0x05U:
			/* Configure Response: the device's configuration taken. */
			channel = fake_by_local(get16(data));
			if (channel == NULL)
				break;
			channel->ours_done = 1;
			fake_channel_ready(channel);
			break;
		case 0x06U:
			/* Disconnection Request: answered, the channel freed. */
			channel = fake_by_local(get16(data));
			fake_signal_send(0x07U, identifier, data, 4U);
			fake.host_closes++;
			if (channel != NULL)
				memset(channel, 0, sizeof(*channel));
			break;
		default:
			break;
		}
	}
}

/* Answers a ServiceSearchAttributeRequest with its records, a chunk at a time (the continuation is the offset). */
static void
fake_sdp(
	struct fake_channel *channel,
	const uint8_t *pdu,
	size_t length)
{
	static struct build lists;
	static uint8_t out[700];
	size_t offset;
	size_t count;
	size_t chunk;
	size_t parameters;
	size_t used;
	uint16_t uuid;

	/* A whole request of bluetoothd's form (section 4.3): the UUID, then the continuation. */
	if (length < 20U || pdu[0] != 0x06U)
		return;
	fake.sdp_requests++;
	uuid = (uint16_t)((pdu[8] << 8) | pdu[9]);
	offset = 0U;
	if (pdu[19] == 4U && length >= 24U)
		offset = ((size_t)pdu[20] << 24) | ((size_t)pdu[21] << 16) | ((size_t)pdu[22] << 8) | pdu[23];

	/* The lists of the records of that class. */
	build_lists(&lists, uuid);
	if (offset > lists.used)
		offset = lists.used;
	chunk = fake.chunk;
	if (chunk > 640U)
		chunk = 640U;
	count = lists.used - offset;
	if (count > chunk)
		count = chunk;

	/* The response: its header, the count, the fragment, the continuation (4 bytes of the next offset) unless the last. */
	out[0] = 0x07U;
	out[1] = pdu[1];
	out[2] = pdu[2];
	out[5] = (uint8_t)(count >> 8);
	out[6] = (uint8_t)count;
	memcpy(out + 7, lists.bytes + offset, count);
	used = 7U + count;
	if (offset + count < lists.used) {
		out[used] = 4U;
		out[used + 1U] = (uint8_t)((offset + count) >> 24);
		out[used + 2U] = (uint8_t)((offset + count) >> 16);
		out[used + 3U] = (uint8_t)((offset + count) >> 8);
		out[used + 4U] = (uint8_t)(offset + count);
		used += 5U;
	} else {
		out[used] = 0U;
		used++;
	}

	/* The parameters' length, then the frame. */
	parameters = used - 5U;
	out[3] = (uint8_t)(parameters >> 8);
	out[4] = (uint8_t)parameters;
	fake_frame(channel->remote, out, used);
}

/* Takes a HIDP message on the control channel: SET_PROTOCOL (its handshake), an unplug, a handshake. */
static void
fake_control(
	struct fake_channel *channel,
	const uint8_t *frame,
	size_t length)
{
	uint8_t answer[1];
	unsigned type;

	/* The message's type. */
	if (length < 1U)
		return;
	type = frame[0] >> 4;

	/* Each type the device knows. */
	switch (type) {
	case BTD_HIDP_SET_PROTOCOL:
		fake.set_protocols++;
		if (!fake.answer_handshake)
			break;
		answer[0] = btd_hidp_header(BTD_HIDP_HANDSHAKE, BTD_HIDP_SUCCESSFUL);
		fake_frame(channel->remote, answer, 1U);
		break;
	case BTD_HIDP_CONTROL:
		if ((frame[0] & 0x0fU) == BTD_HIDP_VIRTUAL_CABLE_UNPLUG)
			fake.unplugs++;
		break;
	case BTD_HIDP_HANDSHAKE:
		fake.handshakes++;
		fake.last_handshake = frame[0] & 0x0fU;
		break;
	default:
		break;
	}
}

/* Goes on when a channel's two configurations are done: the device asks for its interrupt channel, or sends its first reports. */
static void
fake_channel_ready(
	struct fake_channel *channel)
{
	static const uint8_t press[9] = { 0xa1U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
	static const uint8_t release[9] = { 0xa1U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
	struct fake_channel *interrupt;
	unsigned index;

	/* Open once. */
	if (!channel->ours_done || !channel->theirs_done || channel->open)
		return;
	channel->open = 1;

	/* The device's own control channel is followed by its interrupt channel. */
	interrupt = fake_by_psm(BTD_SDP_PSM_INTERRUPT);
	if (channel->psm == BTD_SDP_PSM_CONTROL && interrupt == NULL && channel->local >= FAKE_CID_ASKED) {
		fake_ask_channel(BTD_SDP_PSM_INTERRUPT);
		return;
	}

	/* The first reports with the interrupt channel (review S11): pressed, released. */
	if (channel->psm != BTD_SDP_PSM_INTERRUPT)
		return;
	for (index = 0U; index < fake.reports_on_open; index++) {
		if (index % 2U == 0U) {
			fake_frame(channel->remote, press, sizeof(press));
		} else {
			fake_frame(channel->remote, release, sizeof(release));
		}
	}
}

/* Asks the host for a channel as the device (its CIDs from FAKE_CID_ASKED, so they tell which side asked). */
static void
fake_ask_channel(
	uint16_t psm)
{
	struct fake_channel *channel;
	uint8_t request[4];

	/* A free channel. */
	channel = fake_by_psm(0U);
	if (channel == NULL)
		return;
	memset(channel, 0, sizeof(*channel));
	channel->used = 1;
	channel->psm = psm;
	channel->local = (uint16_t)(FAKE_CID_ASKED + psm);

	/* Succeeded: Connection Request. */
	put16(request, psm);
	put16(request + 2, channel->local);
	fake.next_identifier++;
	if (fake.next_identifier == 0U)
		fake.next_identifier = 1U;
	fake_signal_send(0x02U, (uint8_t)(0x80U + fake.next_identifier), request, sizeof(request));
}

/* Sends the device's Configure Request: MTU 672 and a Flush Timeout (the host takes it, phase005 Q10). */
static void
fake_configure(
	struct fake_channel *channel)
{
	uint8_t request[12];

	/* The host's CID, no flags, the two options. */
	put16(request, channel->remote);
	put16(request + 2, 0U);
	request[4] = 0x01U;
	request[5] = 0x02U;
	put16(request + 6, 672U);
	request[8] = 0x02U;
	request[9] = 0x02U;
	put16(request + 10, 0xffffU);
	fake_signal_send(0x04U, 0x60U, request, sizeof(request));
}

/* Sends an input report on the device's interrupt channel (DATA, the header first). */
static void
fake_report(
	const uint8_t *report,
	size_t length)
{
	/* The report on the interrupt channel. */
	fake_hidp(BTD_SDP_PSM_INTERRUPT, report, length);
}

/* Sends a HIDP message on the device's channel of a PSM. */
static void
fake_hidp(
	uint16_t psm,
	const uint8_t *message,
	size_t length)
{
	struct fake_channel *channel;

	/* The channel, open. */
	(void)pthread_mutex_lock(&fake.lock);

	channel = fake_by_psm(psm);
	if (channel != NULL && channel->open)
		fake_frame(channel->remote, message, length);

	(void)pthread_mutex_unlock(&fake.lock);
}

/* The device connects: Connection Request (its address, a keyboard's class, ACL). */
static void
fake_connect_request(void)
{
	uint8_t parameters[10];

	/* The event. */
	(void)pthread_mutex_lock(&fake.lock);

	memcpy(parameters, fake.device, 6U);
	parameters[6] = 0x80U;
	parameters[7] = 0x25U;
	parameters[8] = 0x00U;
	parameters[9] = 0x01U;
	fake_event(0x04U, parameters, sizeof(parameters));

	(void)pthread_mutex_unlock(&fake.lock);
}

/* Ends the connection with a reason (the host's Disconnect, or the device going by itself). */
static void
fake_hang_up(
	uint8_t reason)
{
	uint8_t parameters[4];

	/* The device's side forgotten (the caller holds the lock). */
	memset(fake.channels, 0, sizeof(fake.channels));
	memset(&fake.frames, 0, sizeof(fake.frames));
	fake.connected = 0;
	fake.encrypted = 0;
	fake.next_cid = FAKE_CID_FIRST;

	/* Disconnection Complete. */
	parameters[0] = 0x00U;
	put16(parameters + 1, FAKE_HANDLE);
	parameters[3] = reason;
	fake_event(0x05U, parameters, sizeof(parameters));
}

/* Sends one signalling command of the device. */
static void
fake_signal_send(
	uint8_t code,
	uint8_t identifier,
	const uint8_t *data,
	size_t length)
{
	uint8_t command[64];

	/* Code, identifier, length, data. */
	command[0] = code;
	command[1] = identifier;
	put16(command + 2, (uint16_t)length);
	memcpy(command + 4, data, length);
	fake_frame(BTD_CID_SIGNALLING, command, 4U + length);
}

/* Writes an event with its parameters. */
static void
fake_event(
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
	(void)write(fake.descriptor, packet, 3U + length);
}

/* Writes a Command Complete for an opcode with its return parameters. */
static void
fake_complete(
	uint16_t opcode,
	const uint8_t *returned,
	size_t count)
{
	uint8_t parameters[260];

	/* Window, opcode, return parameters. */
	parameters[0] = 1U;
	put16(parameters + 1, opcode);
	memcpy(parameters + 3, returned, count);
	fake_event(0x0eU, parameters, 3U + count);
}

/* Writes a Command Status of success for an opcode. */
static void
fake_status(
	uint16_t opcode)
{
	uint8_t parameters[4];

	/* Status, window, opcode. */
	parameters[0] = 0x00U;
	parameters[1] = 1U;
	put16(parameters + 2, opcode);
	fake_event(0x0fU, parameters, sizeof(parameters));
}

/* Writes an L2CAP frame to the host, cut into ACL packets of at most 1021 bytes. */
static void
fake_frame(
	uint16_t cid,
	const uint8_t *payload,
	size_t length)
{
	static uint8_t frame[1100];
	uint8_t packet[1100];
	size_t frame_length;
	size_t offset;
	size_t piece;
	size_t packet_length;
	uint8_t boundary;

	/* The frame, then its packets (the first starts it, the rest continue it). */
	frame_length = btd_l2cap_frame(frame, sizeof(frame), cid, payload, length);
	boundary = 0x02U;
	for (offset = 0U; offset < frame_length; offset += piece) {
		piece = frame_length - offset;
		if (piece > 1021U)
			piece = 1021U;
		packet_length = btd_acl_build(packet, sizeof(packet), FAKE_HANDLE, boundary, frame + offset, piece);
		(void)write(fake.descriptor, packet, packet_length);
		boundary = 0x01U;
	}
}

/* Writes Connection Complete for the device: a status, its handle, its address, ACL, not encrypted. */
static void
fake_connected(
	uint8_t status)
{
	uint8_t parameters[11];

	/* The device's side starts afresh (the caller holds the lock). */
	memset(fake.channels, 0, sizeof(fake.channels));
	memset(&fake.frames, 0, sizeof(fake.frames));
	fake.next_cid = FAKE_CID_FIRST;
	fake.encrypted = 0;
	if (status == 0x00U)
		fake.connected = 1;

	/* The event. */
	parameters[0] = status;
	put16(parameters + 1, FAKE_HANDLE);
	memcpy(parameters + 3, fake.device, 6U);
	parameters[9] = 0x01U;
	parameters[10] = 0x00U;
	fake_event(0x03U, parameters, sizeof(parameters));
}

/* Tells whether the controller saw a command. */
static int
fake_saw(
	uint16_t opcode)
{
	unsigned count;

	/* Seen at least once. */
	count = fake_count(opcode);
	if (count != 0U)
		return 1;

	/* Not seen. */
	return 0;
}

/* Counts the commands of an opcode the controller saw. */
static unsigned
fake_count(
	uint16_t opcode)
{
	unsigned index;
	unsigned count;

	/* Each command seen. */
	(void)pthread_mutex_lock(&fake.lock);

	count = 0U;
	for (index = 0U; index < fake.opcode_count; index++) {
		if (fake.opcodes[index] == opcode)
			count++;
	}

	(void)pthread_mutex_unlock(&fake.lock);

	/* Succeeded: the count. */
	return count;
}

/* Finds the device's channel of its CID, or NULL. */
static struct fake_channel *
fake_by_local(
	uint16_t cid)
{
	unsigned index;

	/* Each channel in use. */
	for (index = 0U; index < FAKE_CHANNELS; index++) {
		if (fake.channels[index].used && fake.channels[index].local == cid)
			return &fake.channels[index];
	}

	/* None. */
	return NULL;
}

/* Finds the device's channel of a PSM, or (PSM 0) a free one, or NULL. */
static struct fake_channel *
fake_by_psm(
	uint16_t psm)
{
	unsigned index;

	/* Each channel: a free one for PSM 0, else the one of that PSM. */
	for (index = 0U; index < FAKE_CHANNELS; index++) {
		if (psm == 0U && !fake.channels[index].used)
			return &fake.channels[index];
		if (psm != 0U && fake.channels[index].used && fake.channels[index].psm == psm)
			return &fake.channels[index];
	}

	/* None. */
	return NULL;
}

/*
 * Builds the attribute lists the device answers for a service class: its
 * HID record (the descriptor, the flags of its behaviour), none (no HID
 * record), or a descriptor of 4097 bytes; or its PnP record.
 */
static void
build_lists(
	struct build *build,
	uint16_t uuid)
{
	static const uint8_t name[] = "Fake Keyboard";
	static uint8_t big[4097];

	/* The lists, empty for a class the device does not have. */
	memset(build, 0, sizeof(*build));
	open_sequence(build);
	if (uuid == BTD_SDP_UUID_HID && fake.sdp_kind == SDP_NO_HID) {
		close_sequence(build);
		return;
	}

	/* The PnP record. */
	if (uuid == BTD_SDP_UUID_PNP) {
		open_sequence(build);
		put_attribute(build, 0x0001U);
		open_sequence(build);
		put_uuid(build, BTD_SDP_UUID_PNP);
		close_sequence(build);
		put_attribute(build, 0x0201U);
		put_byte(build, 0x09U);
		put_uint16(build, 0x1209U);
		put_attribute(build, 0x0202U);
		put_byte(build, 0x09U);
		put_uint16(build, 0x4b42U);
		put_attribute(build, 0x0203U);
		put_byte(build, 0x09U);
		put_uint16(build, 0x0100U);
		close_sequence(build);
		close_sequence(build);
		return;
	}

	/* The HID record: its class and channels. */
	open_sequence(build);
	put_attribute(build, 0x0001U);
	open_sequence(build);
	put_uuid(build, BTD_SDP_UUID_HID);
	close_sequence(build);
	put_attribute(build, 0x0004U);
	open_sequence(build);
	open_sequence(build);
	put_uuid(build, 0x0100U);
	put_byte(build, 0x09U);
	put_uint16(build, BTD_SDP_PSM_CONTROL);
	close_sequence(build);
	open_sequence(build);
	put_uuid(build, 0x0011U);
	close_sequence(build);
	close_sequence(build);
	put_attribute(build, 0x000dU);
	open_sequence(build);
	open_sequence(build);
	open_sequence(build);
	put_uuid(build, 0x0100U);
	put_byte(build, 0x09U);
	put_uint16(build, BTD_SDP_PSM_INTERRUPT);
	close_sequence(build);
	open_sequence(build);
	put_uuid(build, 0x0011U);
	close_sequence(build);
	close_sequence(build);
	close_sequence(build);

	/* Its name and flags. */
	put_attribute(build, 0x0100U);
	put_text(build, name, sizeof(name) - 1U);
	put_attribute(build, 0x0204U);
	put_bool(build, 1);
	put_attribute(build, 0x0205U);
	put_bool(build, fake.reconnect_initiate);
	put_attribute(build, 0x020dU);
	put_bool(build, fake.normally_connectable);
	put_attribute(build, 0x020eU);
	put_bool(build, fake.boot_device);

	/* Its descriptor (the keyboard's, or one too long). */
	put_attribute(build, 0x0206U);
	open_sequence(build);
	open_sequence(build);
	put_byte(build, 0x08U);
	put_byte(build, 0x22U);
	if (fake.sdp_kind == SDP_BIG) {
		memset(big, 0x00, sizeof(big));
		put_text(build, big, sizeof(big));
	} else {
		put_text(build, keyboard_descriptor, sizeof(keyboard_descriptor));
	}

	/* The descriptor's two sequences, the record, the lists. */
	close_sequence(build);
	close_sequence(build);
	close_sequence(build);
	close_sequence(build);
}

/* Adds bytes to a record being built. */
static void
put(
	struct build *build,
	const uint8_t *bytes,
	size_t length)
{
	/* Past the buffer is a mistake of the test. */
	if (length > BUILD_MAX - build->used) {
		fprintf(stderr, "bt-hidhost-host-test: a built record is too long\n");
		exit(2);
	}

	/* The bytes at the end. */
	memcpy(&build->bytes[build->used], bytes, length);
	build->used += length;
}

/* Adds one byte. */
static void
put_byte(
	struct build *build,
	uint8_t byte)
{
	/* The byte. */
	put(build, &byte, 1U);
}

/* Adds a big-endian 16-bit number. */
static void
put_uint16(
	struct build *build,
	uint16_t value)
{
	/* Most significant first. */
	put_byte(build, (uint8_t)(value >> 8));
	put_byte(build, (uint8_t)value);
}

/* Adds an attribute's ID (a 16-bit unsigned element). */
static void
put_attribute(
	struct build *build,
	uint16_t id)
{
	/* uint16. */
	put_byte(build, 0x09U);
	put_uint16(build, id);
}

/* Adds a 16-bit UUID element. */
static void
put_uuid(
	struct build *build,
	uint16_t uuid)
{
	/* uuid16. */
	put_byte(build, 0x19U);
	put_uint16(build, uuid);
}

/* Adds a boolean element. */
static void
put_bool(
	struct build *build,
	int value)
{
	/* bool. */
	put_byte(build, 0x28U);
	put_byte(build, (uint8_t)(value != 0));
}

/* Adds a text element with a 16-bit length. */
static void
put_text(
	struct build *build,
	const uint8_t *bytes,
	size_t length)
{
	/* text, 2-byte length. */
	put_byte(build, 0x26U);
	put_uint16(build, (uint16_t)length);
	put(build, bytes, length);
}

/* Opens a sequence with a 16-bit length, filled in when it closes. */
static void
open_sequence(
	struct build *build)
{
	/* The header, its length to come. */
	put_byte(build, 0x36U);
	build->open[build->depth] = build->used;
	build->depth++;
	put_uint16(build, 0U);
}

/* Closes the innermost open sequence, writing its length. */
static void
close_sequence(
	struct build *build)
{
	size_t at;
	size_t length;

	/* The length of what came after its header. */
	build->depth--;
	at = build->open[build->depth];
	length = build->used - at - 2U;
	build->bytes[at] = (uint8_t)(length >> 8);
	build->bytes[at + 1U] = (uint8_t)length;
}

/* Writes a 16-bit value least significant byte first. */
static void
put16(
	uint8_t *bytes,
	uint16_t value)
{
	/* The two bytes. */
	bytes[0] = (uint8_t)(value & 0xffU);
	bytes[1] = (uint8_t)(value >> 8);
}

/* Reads a 16-bit value least significant byte first. */
static uint16_t
get16(
	const uint8_t *bytes)
{
	/* The two bytes. */
	return (uint16_t)(bytes[0] | (bytes[1] << 8));
}
