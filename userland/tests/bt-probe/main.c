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
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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

static int probe_open(const char *named, char *path, size_t size);
static int probe_command(int descriptor, uint16_t opcode, const uint8_t *parameters, size_t count, uint8_t *answer, size_t capacity, size_t *length);
static int probe_read(int descriptor, uint8_t *packet, size_t capacity, size_t *length);
static void probe_hex(const char *what, const uint8_t *bytes, size_t length);
static void probe_fail(const char *step, int error, unsigned *failures);

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
	int option;
	int error;
	int noticed;

	/* The options: a node named, and the kernel's reset too. */
	named = NULL;
	reset_too = 0;
	option = getopt(argc, argv, "f:r");
	while (option != -1) {
		if (option == 'f') {
			named = optarg;
		} else if (option == 'r') {
			reset_too = 1;
		} else {
			fprintf(stderr, "usage: bt-probe [-f DEVICE] [-r]\n");
			return 2;
		}

		/* The next option. */
		option = getopt(argc, argv, "f:r");
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
