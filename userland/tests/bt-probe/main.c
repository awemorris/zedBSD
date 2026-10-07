/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Bluetooth HCI node probe (ws143-p002): tries a controller's
 * /dev/btN.
 *
 *   bt-probe [-f DEVICE] [-r]
 *   bt-probe -L [-f DEVICE]      the class's test against the test kernel's
 *                                loopback controller (ws143-p002)
 *
 * Without -f it takes the first /dev/btN that opens.  It prints the node's
 * information, then sends Intel's Read Version (an Intel controller only,
 * in its TLV form), HCI_Reset, Read Local Version Information and Read
 * BD_ADDR, each waiting for its Command Complete, and prints what came
 * back; with -r it also resets the controller through the kernel
 * (BT_IOC_RESET) and waits for the reset's notice.  Last come the node's
 * counts.  Each line is "BT ..." on standard output; the last is "BT PASS"
 * (status 0) or "BT FAIL steps=<count>" (status 1), each failed step having
 * said "BT FAIL step=<what> error=<errno>" before.
 *
 * The class's test (-L) needs the loopback controller (vendor 1209, product
 * B7E5) and checks: one open at a time (EBUSY), a read of nothing, the two
 * queues read in the order the packets came, EMSGSIZE for a short buffer
 * (the packet stays), an ACL packet sent and back, the backpressure of a
 * flood larger than the queue (counted stalls, nothing dropped, the order
 * kept), the reset's notice queued into a full queue after everything from
 * before and before anything after, a SCO packet refused, and the
 * controller withdrawn while a read waits (ENODEV, POLLHUP, the information
 * still given, the node published again under the same name).  Its last
 * line is "BT LOOPBACK PASS" or "BT FAIL steps=<count>".
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/bluetooth.h>

/* The nodes looked at, and how long an answer may take (milliseconds). */
#define PROBE_NODES		16U
#define PROBE_WAIT_MS		2000

/* The HCI events a command is answered with: Command Complete and Command Status. */
#define PROBE_EVENT_COMPLETE	0x0eU
#define PROBE_EVENT_STATUS	0x0fU

/* The commands of the probe (OGF and OCF together). */
#define PROBE_OP_RESET		0x0c03U
#define PROBE_OP_LOCAL_VERSION	0x1001U
#define PROBE_OP_BD_ADDR	0x1009U
#define PROBE_OP_INTEL_VERSION	0xfc05U

/* Intel's USB vendor, whose controllers answer Read Version. */
#define PROBE_VENDOR_INTEL	0x8087U

/* The most bytes of an answer printed in hexadecimal. */
#define PROBE_HEX_MAX		96U

/* The loopback controller's identity and test commands (src/drivers/generic/bt-hci-loopback.c). */
#define LOOPBACK_VENDOR		0x1209U
#define LOOPBACK_PRODUCT	0xb7e5U
#define LOOPBACK_OP_ORDER	0xfc01U
#define LOOPBACK_OP_FLOOD	0xfc02U
#define LOOPBACK_OP_WITHDRAW	0xfc03U
#define LOOPBACK_EVENT_VENDOR	0xffU

/* A flood larger than the events' queue (16 KiB, about 61 of its events), and how long the test leaves it unread. */
#define LOOPBACK_FLOOD		300U
#define LOOPBACK_PAUSE_MS	300U

/* How long the test waits for the withdrawn controller's node to come back. */
#define LOOPBACK_RETURN_MS	3000U

static int probe_open(const char *named, char *path, size_t size);
static int probe_command(int descriptor, uint16_t opcode, const uint8_t *parameters, size_t count, uint8_t *answer, size_t capacity, size_t *length);
static int probe_read(int descriptor, uint8_t *packet, size_t capacity, size_t *length);
static void probe_hex(const char *what, const uint8_t *bytes, size_t length);
static void probe_fail(const char *step, int error, unsigned *failures);
static int probe_loopback(const char *path);
static void loopback_order(int descriptor, unsigned *failures);
static void loopback_short_buffer(int descriptor, unsigned *failures);
static void loopback_acl(int descriptor, unsigned *failures);
static void loopback_flood(int descriptor, unsigned *failures);
static void loopback_reset_full(int descriptor, unsigned *failures);
static int loopback_withdraw(int descriptor, const char *path, unsigned *failures);
static int loopback_send_command(int descriptor, uint16_t opcode, const uint8_t *parameters, size_t count);
static int loopback_flood_start(int descriptor, unsigned count);
static void probe_pause_ms(unsigned milliseconds);

/*
 * Tries the node; the exit status says whether every step held.
 */
int
main(
	int argc,
	char **argv)
{
	static const uint8_t intel_tlv[] = { 0xff };
	struct bt_info info;
	struct bt_stats stats;
	uint8_t answer[BT_EVENT_PACKET_MAX];
	uint8_t packet[BT_ACL_PACKET_MAX];
	char path[64];
	const char *named;
	unsigned failures;
	size_t length;
	int descriptor;
	int reset_too;
	int loopback_test;
	int option;
	int error;
	int noticed;

	/* The options: a node named, the kernel's reset too, or the class's test. */
	named = NULL;
	reset_too = 0;
	loopback_test = 0;
	option = getopt(argc, argv, "f:rL");
	while (option != -1) {
		if (option == 'f') {
			named = optarg;
		} else if (option == 'r') {
			reset_too = 1;
		} else if (option == 'L') {
			loopback_test = 1;
		} else {
			fprintf(stderr, "usage: bt-probe [-f DEVICE] [-r] | bt-probe -L [-f DEVICE]\n");
			return 2;
		}

		/* The next option. */
		option = getopt(argc, argv, "f:rL");
	}

	/* The class's test against the loopback controller. */
	if (loopback_test) {
		if (named == NULL)
			named = "/dev/bt0";
		error = probe_loopback(named);
		return error;
	}

	/* The node. */
	failures = 0U;
	descriptor = probe_open(named, path, sizeof(path));
	if (descriptor < 0) {
		printf("BT FAIL step=open error=%d\n", errno);
		return 1;
	}

	/* The node that opened. */
	printf("BT open path=%s\n", path);

	/* What the node says of its controller. */
	memset(&info, 0, sizeof(info));
	error = ioctl(descriptor, BT_IOC_GET_INFO, &info);
	if (error != 0) {
		probe_fail("info", errno, &failures);
	} else {
		info.name[BT_TEXT_MAX - 1U] = '\0';
		info.physical_path[BT_TEXT_MAX - 1U] = '\0';
		printf("BT info vendor=%04x product=%04x version=%04x bus=%u flags=0x%x acl=%u name=\"%s\" place=%s\n",
		       (unsigned)info.vendor,
		       (unsigned)info.product,
		       (unsigned)info.version,
		       (unsigned)info.bus,
		       (unsigned)info.flags,
		       (unsigned)info.acl_data_max,
		       info.name,
		       info.physical_path);
	}

	/* Intel's Read Version, in its TLV form (an Intel controller answers it in the bootloader too). */
	if (info.vendor == PROBE_VENDOR_INTEL) {
		error = probe_command(descriptor, PROBE_OP_INTEL_VERSION, intel_tlv, sizeof(intel_tlv), answer, sizeof(answer), &length);
		if (error != 0) {
			probe_fail("intel-version", error, &failures);
		} else {
			probe_hex("intel-version", answer, length);
		}
	}

	/* HCI_Reset: its status. */
	error = probe_command(descriptor, PROBE_OP_RESET, NULL, 0U, answer, sizeof(answer), &length);
	if (error != 0) {
		probe_fail("reset", error, &failures);
	} else {
		printf("BT reset status=0x%02x\n", (unsigned)answer[0]);
	}

	/* Read Local Version Information: the versions and the manufacturer. */
	error = probe_command(descriptor, PROBE_OP_LOCAL_VERSION, NULL, 0U, answer, sizeof(answer), &length);
	if (error != 0) {
		probe_fail("local-version", error, &failures);
	} else if (length < 9U) {
		probe_hex("local-version-short", answer, length);
		probe_fail("local-version", EPROTO, &failures);
	} else {
		printf("BT local-version status=0x%02x hci=%u revision=0x%04x lmp=%u manufacturer=0x%04x subversion=0x%04x\n",
		       (unsigned)answer[0],
		       (unsigned)answer[1],
		       (unsigned)(answer[2] | (answer[3] << 8)),
		       (unsigned)answer[4],
		       (unsigned)(answer[5] | (answer[6] << 8)),
		       (unsigned)(answer[7] | (answer[8] << 8)));
	}

	/* Read BD_ADDR: the controller's address, most significant byte first. */
	error = probe_command(descriptor, PROBE_OP_BD_ADDR, NULL, 0U, answer, sizeof(answer), &length);
	if (error != 0) {
		probe_fail("bd-addr", error, &failures);
	} else if (length < 7U) {
		probe_hex("bd-addr-short", answer, length);
		probe_fail("bd-addr", EPROTO, &failures);
	} else {
		printf("BT bd-addr status=0x%02x address=%02x:%02x:%02x:%02x:%02x:%02x\n",
		       (unsigned)answer[0],
		       (unsigned)answer[6],
		       (unsigned)answer[5],
		       (unsigned)answer[4],
		       (unsigned)answer[3],
		       (unsigned)answer[2],
		       (unsigned)answer[1]);
	}

	/* The kernel's reset, and the notice that it ended. */
	if (reset_too) {
		error = ioctl(descriptor, BT_IOC_RESET);
		if (error != 0) {
			probe_fail("ioctl-reset", errno, &failures);
		} else {
			noticed = 0;
			error = probe_read(descriptor, packet, sizeof(packet), &length);
			while (error == 0 && !noticed) {
				if (packet[0] == BT_PACKET_NOTICE_RESET) {
					noticed = 1;
				} else {
					error = probe_read(descriptor, packet, sizeof(packet), &length);
				}
			}

			/* The notice came, or the wait ended without it. */
			if (noticed) {
				printf("BT ioctl-reset notice=1\n");
			} else {
				probe_fail("ioctl-reset-notice", error, &failures);
			}
		}
	}

	/* The node's counts. */
	memset(&stats, 0, sizeof(stats));
	error = ioctl(descriptor, BT_IOC_GET_STATS, &stats);
	if (error != 0) {
		probe_fail("stats", errno, &failures);
	} else {
		printf("BT stats events=%llu acl-in=%llu commands=%llu acl-out=%llu malformed=%llu stalls=%llu\n",
		       (unsigned long long)stats.events_in,
		       (unsigned long long)stats.acl_in,
		       (unsigned long long)stats.commands_out,
		       (unsigned long long)stats.acl_out,
		       (unsigned long long)stats.malformed,
		       (unsigned long long)stats.stalls);
	}

	/* The node goes. */
	(void)close(descriptor);

	/* A failed step fails the probe. */
	if (failures != 0U) {
		printf("BT FAIL steps=%u\n", failures);
		return 1;
	}

	/* Succeeded: every step held. */
	printf("BT PASS\n");
	return 0;
}

/* Opens the node named, or the first /dev/btN that opens; gives its path. */
static int
probe_open(
	const char *named,
	char *path,
	size_t size)
{
	unsigned index;
	int descriptor;

	/* The node named. */
	if (named != NULL) {
		snprintf(path, size, "%s", named);
		descriptor = open(path, O_RDWR | O_NONBLOCK);
		return descriptor;
	}

	/* Each node in turn until one opens. */
	for (index = 0U; index < PROBE_NODES; index++) {
		snprintf(path, size, "/dev/bt%u", index);
		descriptor = open(path, O_RDWR | O_NONBLOCK);
		if (descriptor >= 0)
			return descriptor;
	}

	/* No node opened (errno says why the last did not). */
	return -1;
}

/*
 * Sends a command and waits for its Command Complete: gives its return
 * parameters (the status first).  Returns 0, an errno value, or EPROTO for
 * a Command Status that refused the command.
 */
static int
probe_command(
	int descriptor,
	uint16_t opcode,
	const uint8_t *parameters,
	size_t count,
	uint8_t *answer,
	size_t capacity,
	size_t *length)
{
	uint8_t packet[BT_ACL_PACKET_MAX];
	uint8_t command[BT_COMMAND_PACKET_MAX];
	uint16_t answered;
	size_t received;
	ssize_t written;
	int error;

	/* The H4 packet: the type, the opcode, the parameters' length and the parameters. */
	command[0] = BT_PACKET_COMMAND;
	command[1] = (uint8_t)(opcode & 0xffU);
	command[2] = (uint8_t)(opcode >> 8);
	command[3] = (uint8_t)count;
	if (count != 0U)
		memcpy(command + 4, parameters, count);

	/* One write, one packet. */
	written = write(descriptor, command, 4U + count);
	if (written < 0)
		return errno;

	/* The events until this command's answer. */
	for (;;) {
		error = probe_read(descriptor, packet, sizeof(packet), &received);
		if (error != 0)
			return error;

		/* Another kind of packet, or an event too short to be an answer, is passed over. */
		if (packet[0] != BT_PACKET_EVENT || received < 3U)
			continue;

		/* Command Complete: [event][code][length][credits][opcode][return parameters]. */
		if (packet[1] == PROBE_EVENT_COMPLETE && received >= 6U) {
			answered = (uint16_t)(packet[4] | (packet[5] << 8));
			if (answered != opcode)
				continue;
			*length = received - 6U;
			if (*length > capacity)
				*length = capacity;
			memcpy(answer, packet + 6, *length);
			return 0;
		}

		/* Command Status: [event][code][length][status][credits][opcode]; a refusal ends the wait. */
		if (packet[1] == PROBE_EVENT_STATUS && received >= 7U) {
			answered = (uint16_t)(packet[5] | (packet[6] << 8));
			if (answered == opcode && packet[3] != 0U) {
				printf("BT command-status opcode=0x%04x status=0x%02x\n", (unsigned)opcode, (unsigned)packet[3]);
				return EPROTO;
			}
		}
	}
}

/* Reads one packet, waiting up to PROBE_WAIT_MS; ETIMEDOUT when none came. */
static int
probe_read(
	int descriptor,
	uint8_t *packet,
	size_t capacity,
	size_t *length)
{
	struct pollfd wait;
	ssize_t count;
	int ready;

	/* Something to read, in time. */
	memset(&wait, 0, sizeof(wait));
	wait.fd = descriptor;
	wait.events = POLLIN;
	ready = poll(&wait, 1, PROBE_WAIT_MS);
	if (ready < 0)
		return errno;
	if (ready == 0)
		return ETIMEDOUT;

	/* One packet. */
	count = read(descriptor, packet, capacity);
	if (count < 0)
		return errno;
	if (count == 0)
		return EIO;

	/* Succeeded: the packet's length. */
	*length = (size_t)count;
	return 0;
}

/* Prints bytes in hexadecimal on one line (at most PROBE_HEX_MAX). */
static void
probe_hex(
	const char *what,
	const uint8_t *bytes,
	size_t length)
{
	size_t index;
	size_t shown;

	/* The start of the line, and how many bytes it shows. */
	printf("BT %s length=%u bytes=", what, (unsigned)length);
	shown = length;
	if (shown > PROBE_HEX_MAX)
		shown = PROBE_HEX_MAX;

	/* Each byte. */
	for (index = 0U; index < shown; index++)
		printf("%02x", (unsigned)bytes[index]);
	printf("\n");
}

/* Says a step failed and counts it. */
static void
probe_fail(
	const char *step,
	int error,
	unsigned *failures)
{
	/* The line, and one more failure. */
	printf("BT FAIL step=%s error=%d\n", step, error);
	(*failures)++;
}

/*
 * Runs the class's test against the loopback controller at path; returns
 * the exit status (0 when every step held).
 */
static int
probe_loopback(
	const char *path)
{
	struct bt_info info;
	unsigned failures;
	char opened[64];
	ssize_t count;
	uint8_t byte;
	int descriptor;
	int second;
	int error;

	/* The node, the loopback controller's. */
	failures = 0U;
	descriptor = probe_open(path, opened, sizeof(opened));
	if (descriptor < 0) {
		printf("BT FAIL step=open error=%d\n", errno);
		return 1;
	}

	/* Its information names the loopback controller. */
	memset(&info, 0, sizeof(info));
	error = ioctl(descriptor, BT_IOC_GET_INFO, &info);
	if (error != 0) {
		probe_fail("loopback-info", errno, &failures);
	} else if (info.vendor != LOOPBACK_VENDOR || info.product != LOOPBACK_PRODUCT) {
		printf("BT loopback-info vendor=%04x product=%04x\n", (unsigned)info.vendor, (unsigned)info.product);
		probe_fail("loopback-info", ENODEV, &failures);
	} else {
		printf("BT loopback-info ok\n");
	}

	/* A second open while this one holds the node. */
	second = open(path, O_RDWR | O_NONBLOCK);
	if (second >= 0) {
		(void)close(second);
		probe_fail("ebusy", 0, &failures);
	} else if (errno != EBUSY) {
		probe_fail("ebusy", errno, &failures);
	} else {
		printf("BT ebusy ok\n");
	}

	/* A read of nothing reads nothing. */
	count = read(descriptor, &byte, 0U);
	if (count != 0) {
		probe_fail("read-zero", errno, &failures);
	} else {
		printf("BT read-zero ok\n");
	}

	/* Each part of the test. */
	loopback_order(descriptor, &failures);
	loopback_short_buffer(descriptor, &failures);
	loopback_acl(descriptor, &failures);
	loopback_flood(descriptor, &failures);
	loopback_reset_full(descriptor, &failures);
	descriptor = loopback_withdraw(descriptor, path, &failures);

	/* The node goes. */
	if (descriptor >= 0)
		(void)close(descriptor);

	/* A failed step fails the test. */
	if (failures != 0U) {
		printf("BT FAIL steps=%u\n", failures);
		return 1;
	}

	/* Succeeded: every step held. */
	printf("BT LOOPBACK PASS\n");
	return 0;
}

/* The two queues read in the order the packets came: E1, A2, E3, A4, then the answer. */
static void
loopback_order(
	int descriptor,
	unsigned *failures)
{
	static const uint8_t expected_types[4] = { BT_PACKET_EVENT, BT_PACKET_ACL, BT_PACKET_EVENT, BT_PACKET_ACL };
	uint8_t packet[BT_ACL_PACKET_MAX];
	size_t length;
	unsigned index;
	int error;

	/* The command. */
	error = loopback_send_command(descriptor, LOOPBACK_OP_ORDER, NULL, 0U);
	if (error != 0) {
		probe_fail("order-send", error, failures);
		return;
	}

	/* Each packet in turn: its type, and its place in its last byte. */
	for (index = 0U; index < 4U; index++) {
		error = probe_read(descriptor, packet, sizeof(packet), &length);
		if (error != 0) {
			probe_fail("order-read", error, failures);
			return;
		}

		/* The type and the mark of its place. */
		if (packet[0] != expected_types[index] || packet[length - 1U] != (uint8_t)(index + 1U)) {
			printf("BT order index=%u type=%u mark=%u\n", index, (unsigned)packet[0], (unsigned)packet[length - 1U]);
			probe_fail("order", EPROTO, failures);
			return;
		}
	}

	/* The answer last: Command Complete of the command. */
	error = probe_read(descriptor, packet, sizeof(packet), &length);
	if (error != 0) {
		probe_fail("order-answer", error, failures);
		return;
	}

	/* An event, Command Complete, of 0xFC01. */
	if (packet[0] != BT_PACKET_EVENT ||
	    packet[1] != PROBE_EVENT_COMPLETE ||
	    packet[4] != 0x01U ||
	    packet[5] != 0xfcU) {
		probe_fail("order-answer", EPROTO, failures);
		return;
	}

	/* Succeeded. */
	printf("BT order ok\n");
}

/* A buffer shorter than the packet: EMSGSIZE, and the packet stays for a larger one. */
static void
loopback_short_buffer(
	int descriptor,
	unsigned *failures)
{
	uint8_t packet[BT_ACL_PACKET_MAX];
	uint8_t small[3];
	struct pollfd wait;
	size_t length;
	ssize_t count;
	int error;

	/* Read BD_ADDR, whose answer is longer than 3 bytes. */
	error = loopback_send_command(descriptor, PROBE_OP_BD_ADDR, NULL, 0U);
	if (error != 0) {
		probe_fail("emsgsize-send", error, failures);
		return;
	}

	/* The answer is there. */
	memset(&wait, 0, sizeof(wait));
	wait.fd = descriptor;
	wait.events = POLLIN;
	(void)poll(&wait, 1, PROBE_WAIT_MS);

	/* Too short a buffer. */
	count = read(descriptor, small, sizeof(small));
	if (count >= 0 || errno != EMSGSIZE) {
		probe_fail("emsgsize", errno, failures);
		return;
	}

	/* The whole answer with a buffer large enough. */
	error = probe_read(descriptor, packet, sizeof(packet), &length);
	if (error != 0) {
		probe_fail("emsgsize-after", error, failures);
		return;
	}

	/* Command Complete of Read BD_ADDR, whole (its last byte at 12). */
	if (packet[1] != PROBE_EVENT_COMPLETE ||
	    packet[4] != 0x09U ||
	    packet[5] != 0x10U ||
	    length != 13U) {
		probe_fail("emsgsize-after", EPROTO, failures);
		return;
	}

	/* Succeeded. */
	printf("BT emsgsize ok\n");
}

/* An ACL packet sent comes back unchanged; a SCO packet is refused. */
static void
loopback_acl(
	int descriptor,
	unsigned *failures)
{
	static const uint8_t acl[9] = { BT_PACKET_ACL, 0x01U, 0x20U, 0x04U, 0x00U, 0xdeU, 0xadU, 0xbeU, 0xefU };
	static const uint8_t sco[4] = { BT_PACKET_SCO, 0x01U, 0x00U, 0x00U };
	uint8_t packet[BT_ACL_PACKET_MAX];
	size_t length;
	ssize_t written;
	int differs;
	int error;

	/* The ACL packet. */
	written = write(descriptor, acl, sizeof(acl));
	if (written != (ssize_t)sizeof(acl)) {
		probe_fail("acl-write", errno, failures);
		return;
	}

	/* Back as it went. */
	error = probe_read(descriptor, packet, sizeof(packet), &length);
	if (error != 0) {
		probe_fail("acl-read", error, failures);
		return;
	}

	/* The same bytes. */
	differs = 1;
	if (length == sizeof(acl))
		differs = memcmp(packet, acl, sizeof(acl));
	if (differs != 0) {
		probe_fail("acl-echo", EPROTO, failures);
		return;
	}

	/* SCO is not carried. */
	written = write(descriptor, sco, sizeof(sco));
	if (written >= 0 || errno != EINVAL) {
		probe_fail("sco-refused", errno, failures);
		return;
	}

	/* Succeeded. */
	printf("BT acl ok\n");
}

/*
 * A flood larger than the events' queue, left unread a while: the class
 * counts a stall and drops nothing; every event comes, in order.
 */
static void
loopback_flood(
	int descriptor,
	unsigned *failures)
{
	uint8_t packet[BT_ACL_PACKET_MAX];
	struct bt_stats stats;
	unsigned expected;
	unsigned index;
	size_t length;
	int error;

	/* The flood, unread for a while. */
	error = loopback_flood_start(descriptor, LOOPBACK_FLOOD);
	if (error != 0) {
		probe_fail("flood-start", error, failures);
		return;
	}

	/* Not read for a while: the queue fills. */
	probe_pause_ms(LOOPBACK_PAUSE_MS);

	/* The queue filled, and the transport was held back. */
	memset(&stats, 0, sizeof(stats));
	error = ioctl(descriptor, BT_IOC_GET_STATS, &stats);
	if (error != 0 || stats.stalls == 0U) {
		printf("BT flood stalls=%llu\n", (unsigned long long)stats.stalls);
		probe_fail("flood-stall", EPROTO, failures);
		return;
	}

	/* Every event, numbered from 0. */
	for (expected = 0U; expected < LOOPBACK_FLOOD; expected++) {
		error = probe_read(descriptor, packet, sizeof(packet), &length);
		if (error != 0) {
			printf("BT flood read=%u\n", expected);
			probe_fail("flood-read", error, failures);
			return;
		}

		/* A vendor event carrying the next index. */
		index = (unsigned)(packet[3] | (packet[4] << 8));
		if (packet[1] != LOOPBACK_EVENT_VENDOR || index != expected) {
			printf("BT flood expected=%u index=%u code=%u\n", expected, index, (unsigned)packet[1]);
			probe_fail("flood-order", EPROTO, failures);
			return;
		}
	}

	/* Succeeded. */
	printf("BT flood ok events=%u stalls=%llu\n", LOOPBACK_FLOOD, (unsigned long long)stats.stalls);
}

/*
 * The reset with the events' queue full: its notice still comes (the
 * notices' reserve), after the events from before and before anything
 * after, and nothing of the flood follows it.
 */
static void
loopback_reset_full(
	int descriptor,
	unsigned *failures)
{
	uint8_t packet[BT_ACL_PACKET_MAX];
	unsigned before;
	unsigned index;
	size_t length;
	int noticed;
	int error;

	/* A flood left unread fills the queue. */
	error = loopback_flood_start(descriptor, LOOPBACK_FLOOD);
	if (error != 0) {
		probe_fail("reset-flood", error, failures);
		return;
	}

	/* Not read for a while: the queue fills. */
	probe_pause_ms(LOOPBACK_PAUSE_MS);

	/* The reset. */
	error = ioctl(descriptor, BT_IOC_RESET);
	if (error != 0) {
		probe_fail("reset", errno, failures);
		return;
	}

	/* The flood's events from before, in order, then the notice. */
	before = 0U;
	noticed = 0;
	while (!noticed) {
		error = probe_read(descriptor, packet, sizeof(packet), &length);
		if (error != 0) {
			printf("BT reset events-before=%u\n", before);
			probe_fail("reset-notice", error, failures);
			return;
		}

		/* The notice ends the wait; an event before it carries the next index. */
		if (packet[0] == BT_PACKET_NOTICE_RESET) {
			noticed = 1;
			continue;
		}

		/* The event's index. */
		index = (unsigned)(packet[3] | (packet[4] << 8));
		if (index != before) {
			probe_fail("reset-order", EPROTO, failures);
			return;
		}

		/* One more event from before. */
		before++;
	}

	/* Nothing of the flood after the notice: the next packet is the next command's answer. */
	error = loopback_send_command(descriptor, PROBE_OP_LOCAL_VERSION, NULL, 0U);
	if (error != 0) {
		probe_fail("reset-after-send", error, failures);
		return;
	}

	/* Read. */
	error = probe_read(descriptor, packet, sizeof(packet), &length);
	if (error != 0) {
		probe_fail("reset-after", error, failures);
		return;
	}

	/* Command Complete of Read Local Version. */
	if (packet[1] != PROBE_EVENT_COMPLETE ||
	    packet[4] != 0x01U ||
	    packet[5] != 0x10U) {
		probe_fail("reset-after", EPROTO, failures);
		return;
	}

	/* Succeeded: the notice came in a full queue, at its place. */
	printf("BT reset-full ok events-before=%u\n", before);
}

/*
 * The controller withdrawn while a read waits: the read answers ENODEV,
 * poll says POLLHUP, a write ENODEV, the information is still given; the
 * node comes back under the same name.  Returns the open descriptor of the
 * node that came back (or -1).
 */
static int
loopback_withdraw(
	int descriptor,
	const char *path,
	unsigned *failures)
{
	uint8_t packet[BT_ACL_PACKET_MAX];
	struct bt_info info;
	struct pollfd wait;
	size_t length;
	ssize_t count;
	unsigned waited;
	int flags;
	int error;
	int again;

	/* The withdrawal, asked for. */
	error = loopback_send_command(descriptor, LOOPBACK_OP_WITHDRAW, NULL, 0U);
	if (error != 0) {
		probe_fail("withdraw-send", error, failures);
		return descriptor;
	}

	/* Its answer first. */
	error = probe_read(descriptor, packet, sizeof(packet), &length);
	if (error != 0) {
		probe_fail("withdraw-answer", error, failures);
		return descriptor;
	}

	/* A read that waits, until the controller goes. */
	flags = fcntl(descriptor, F_GETFL);
	(void)fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK);
	count = read(descriptor, packet, sizeof(packet));
	if (count >= 0 || errno != ENODEV) {
		probe_fail("withdraw-read", errno, failures);
		return descriptor;
	}

	/* poll: the controller is gone. */
	memset(&wait, 0, sizeof(wait));
	wait.fd = descriptor;
	wait.events = POLLIN | POLLOUT;
	(void)poll(&wait, 1, 0);
	if ((wait.revents & POLLHUP) == 0) {
		probe_fail("withdraw-pollhup", EPROTO, failures);
		return descriptor;
	}

	/* A write cannot reach it. */
	error = loopback_send_command(descriptor, PROBE_OP_RESET, NULL, 0U);
	if (error != ENODEV) {
		probe_fail("withdraw-write", error, failures);
		return descriptor;
	}

	/* The information is still given, as it was last. */
	memset(&info, 0, sizeof(info));
	error = ioctl(descriptor, BT_IOC_GET_INFO, &info);
	if (error != 0 || info.vendor != LOOPBACK_VENDOR) {
		probe_fail("withdraw-info", errno, failures);
		return descriptor;
	}

	/* The withdrawn controller's open goes. */
	(void)close(descriptor);

	/* The node comes back under the same name. */
	again = -1;
	for (waited = 0U; waited < LOOPBACK_RETURN_MS && again < 0; waited += 100U) {
		probe_pause_ms(100U);
		again = open(path, O_RDWR | O_NONBLOCK);
	}

	/* It did not in time. */
	if (again < 0) {
		probe_fail("withdraw-return", errno, failures);
		return -1;
	}

	/* And answers. */
	error = loopback_send_command(again, PROBE_OP_LOCAL_VERSION, NULL, 0U);
	if (error != 0) {
		probe_fail("withdraw-return-send", error, failures);
		return again;
	}

	/* Its answer. */
	error = probe_read(again, packet, sizeof(packet), &length);
	if (error != 0) {
		probe_fail("withdraw-return-answer", error, failures);
		return again;
	}

	/* Succeeded. */
	printf("BT withdraw ok\n");
	return again;
}

/* Writes one command; returns 0 or the errno of the write. */
static int
loopback_send_command(
	int descriptor,
	uint16_t opcode,
	const uint8_t *parameters,
	size_t count)
{
	uint8_t command[BT_COMMAND_PACKET_MAX];
	ssize_t written;

	/* The H4 packet. */
	command[0] = BT_PACKET_COMMAND;
	command[1] = (uint8_t)(opcode & 0xffU);
	command[2] = (uint8_t)(opcode >> 8);
	command[3] = (uint8_t)count;
	if (count != 0U)
		memcpy(command + 4, parameters, count);

	/* One write. */
	written = write(descriptor, command, 4U + count);
	if (written < 0)
		return errno;

	/* Succeeded: sent. */
	return 0;
}

/* Starts a flood of count events and reads its Command Complete; returns 0 or an errno value. */
static int
loopback_flood_start(
	int descriptor,
	unsigned count)
{
	uint8_t packet[BT_ACL_PACKET_MAX];
	uint8_t parameters[2];
	size_t length;
	int error;

	/* The command with the count. */
	parameters[0] = (uint8_t)(count & 0xffU);
	parameters[1] = (uint8_t)(count >> 8);
	error = loopback_send_command(descriptor, LOOPBACK_OP_FLOOD, parameters, sizeof(parameters));
	if (error != 0)
		return error;

	/* Its answer comes before the flood. */
	error = probe_read(descriptor, packet, sizeof(packet), &length);
	if (error != 0)
		return error;
	if (packet[1] != PROBE_EVENT_COMPLETE)
		return EPROTO;

	/* Succeeded: the flood is under way. */
	return 0;
}

/* Waits about the milliseconds given. */
static void
probe_pause_ms(
	unsigned milliseconds)
{
	struct timespec pause;

	/* Seconds and nanoseconds. */
	pause.tv_sec = (time_t)(milliseconds / 1000U);
	pause.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	(void)nanosleep(&pause, NULL);
}

