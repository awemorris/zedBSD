/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The raw HID probe (ws161-p002): tries a security key's node
 * /dev/input/hidrawN, the test kernel's loopback key in QEMU or a real
 * key on the machine.
 *
 *   hidraw-probe [-f DEVICE]
 *
 * Without -f it takes the first /dev/input/hidrawN whose top collection
 * is FIDO's (usage page 0xF1D0).  It prints the information, the name,
 * the place and the report descriptor's size, then, on two opens of the
 * node: a read that does not wait answers EAGAIN; CTAPHID INIT on the
 * broadcast channel (the nonce comes back, with a new channel, and the
 * second open sees the same answer); a PING of 300 bytes over several
 * packets comes back whole; WINK answers; a write of the wrong length is
 * refused with EINVAL; an unknown command answers ERROR (1).  Each line is
 * "HIDRAW ..." on standard output; the last is "HIDRAW PASS" (status 0) or
 * "HIDRAW FAIL step=<what> error=<errno>" (status 1).  A real key answers
 * the unknown command as the loopback does, and WINK blinks it.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/hidraw.h>

/* The nodes looked at, the report's size, and what an initialization and a continuation packet carry. */
#define PROBE_NODES		16U
#define PROBE_REPORT		64U
#define PROBE_INIT_DATA		(PROBE_REPORT - 7U)
#define PROBE_CONT_DATA		(PROBE_REPORT - 5U)

/* The CTAPHID commands used, the broadcast channel, and how long an answer may take. */
#define PROBE_CMD_PING		0x81U
#define PROBE_CMD_INIT		0x86U
#define PROBE_CMD_WINK		0x88U
#define PROBE_CMD_ERROR		0xbfU
#define PROBE_CMD_UNKNOWN	0x99U
#define PROBE_BROADCAST		0xffffffffU
#define PROBE_WAIT_MS		3000

/* The PING's size, and the largest message read back. */
#define PROBE_PING_SIZE		300U
#define PROBE_MESSAGE_MAX	1024U

static int probe_open(const char *named, char *path, size_t size);
static int probe_send(int descriptor, uint32_t channel, uint8_t command, const uint8_t *data, size_t length);
static int probe_receive(int descriptor, uint32_t channel, uint8_t *command, uint8_t *data, size_t capacity, size_t *length);
static int probe_report(int descriptor, uint8_t *report);
static int probe_fail(const char *step, int error);
static uint32_t probe_be32(const uint8_t *bytes);
static void probe_put_be32(uint8_t *bytes, uint32_t value);

/*
 * Tries the node; the exit status says whether every step held.
 */
int
main(
	int argc,
	char **argv)
{
	struct hidraw_descriptor *descriptor;
	struct hidraw_info info;
	struct hidraw_text name;
	struct hidraw_text place;
	uint8_t nonce[8];
	uint8_t ping[PROBE_PING_SIZE];
	uint8_t message[PROBE_MESSAGE_MAX];
	uint8_t report[PROBE_REPORT + 1U];
	uint8_t command;
	char path[64];
	const char *named;
	uint32_t channel;
	size_t length;
	size_t index;
	ssize_t count;
	int first;
	int second;
	int same;
	int grab;
	int error;

	/* The node: -f names one, else the first FIDO one. */
	named = NULL;
	if (argc == 3 && strcmp(argv[1], "-f") == 0)
		named = argv[2];
	first = probe_open(named, path, sizeof(path));
	if (first < 0)
		return probe_fail("open", errno);

	/* What it is. */
	error = ioctl(first, HIDRAW_GET_INFO, &info);
	if (error != 0)
		return probe_fail("info", errno);
	printf("HIDRAW info path=%s bus=%u vendor=%04x product=%04x usage=%04x:%04x input=%u output=%u flags=%x\n",
	    path, info.bus, info.vendor, info.product, info.usage_page, info.usage,
	    info.input_size, info.output_size, info.flags);

	/* Its name and its place. */
	error = ioctl(first, HIDRAW_GET_NAME, &name);
	if (error != 0)
		return probe_fail("name", errno);
	error = ioctl(first, HIDRAW_GET_PHYS, &place);
	if (error != 0)
		return probe_fail("phys", errno);
	printf("HIDRAW name=\"%s\" phys=%s\n", name.value, place.value);

	/* Its report descriptor. */
	descriptor = calloc(1U, sizeof(*descriptor));
	if (descriptor == NULL)
		return probe_fail("descriptor-memory", ENOMEM);
	error = ioctl(first, HIDRAW_GET_DESCRIPTOR, descriptor);
	if (error != 0)
		return probe_fail("descriptor", errno);
	printf("HIDRAW descriptor size=%u first=%02x%02x%02x\n", descriptor->size,
	    descriptor->value[0], descriptor->value[1], descriptor->value[2]);
	free(descriptor);
	if (info.input_size != PROBE_REPORT || info.output_size != PROBE_REPORT)
		return probe_fail("report-size", EINVAL);

	/* A second open, which sees every report too. */
	second = open(path, O_RDWR);
	if (second < 0)
		return probe_fail("open-second", errno);

	/* A read that does not wait finds nothing yet. */
	(void)fcntl(second, F_SETFL, O_NONBLOCK);
	count = read(second, report, sizeof(report));
	if (count >= 0 || errno != EAGAIN)
		return probe_fail("nonblock", count >= 0 ? EEXIST : errno);
	(void)fcntl(second, F_SETFL, 0);
	printf("HIDRAW nonblock ok\n");

	/* INIT on the broadcast channel: the nonce back and a new channel. */
	for (index = 0U; index < sizeof(nonce); index++)
		nonce[index] = (uint8_t)(0xa0U + index);
	error = probe_send(first, PROBE_BROADCAST, PROBE_CMD_INIT, nonce, sizeof(nonce));
	if (error != 0)
		return probe_fail("init-send", error);
	error = probe_receive(first, PROBE_BROADCAST, &command, message, sizeof(message), &length);
	if (error != 0)
		return probe_fail("init-receive", error);
	same = memcmp(message, nonce, sizeof(nonce));
	if (command != PROBE_CMD_INIT || length < 17U || same != 0)
		return probe_fail("init-answer", EPROTO);
	channel = probe_be32(message + 8U);
	printf("HIDRAW init ok channel=%08x version=%u capabilities=%x\n", channel, message[12], message[16]);

	/* The second open saw the same answer. */
	error = probe_receive(second, PROBE_BROADCAST, &command, message, sizeof(message), &length);
	if (error != 0 || command != PROBE_CMD_INIT)
		return probe_fail("init-second", error != 0 ? error : EPROTO);
	printf("HIDRAW second-open ok\n");

	/* A PING of several packets comes back whole. */
	for (index = 0U; index < sizeof(ping); index++)
		ping[index] = (uint8_t)(index * 7U);
	error = probe_send(first, channel, PROBE_CMD_PING, ping, sizeof(ping));
	if (error != 0)
		return probe_fail("ping-send", error);
	error = probe_receive(first, channel, &command, message, sizeof(message), &length);
	if (error != 0)
		return probe_fail("ping-receive", error);
	same = memcmp(message, ping, sizeof(ping));
	if (command != PROBE_CMD_PING || length != sizeof(ping) || same != 0)
		return probe_fail("ping-answer", EPROTO);
	printf("HIDRAW ping ok bytes=%u\n", (unsigned)length);

	/* WINK answers. */
	error = probe_send(first, channel, PROBE_CMD_WINK, NULL, 0U);
	if (error != 0)
		return probe_fail("wink-send", error);
	error = probe_receive(first, channel, &command, message, sizeof(message), &length);
	if (error != 0 || command != PROBE_CMD_WINK)
		return probe_fail("wink", error != 0 ? error : EPROTO);
	printf("HIDRAW wink ok\n");

	/* A report of the wrong length is refused. */
	memset(report, 0, sizeof(report));
	count = write(first, report, 10U);
	if (count >= 0 || errno != EINVAL)
		return probe_fail("short-write", count >= 0 ? EEXIST : errno);
	printf("HIDRAW short-write refused ok\n");

	/* The grab: the first open alone; the second cannot write and hears nothing. */
	grab = 1;
	error = ioctl(first, HIDRAW_GRAB, &grab);
	if (error != 0)
		return probe_fail("grab", errno);
	error = ioctl(second, HIDRAW_GRAB, &grab);
	if (error == 0 || errno != EBUSY)
		return probe_fail("grab-second", error == 0 ? EEXIST : errno);
	memset(report, 0, sizeof(report));
	count = write(second, report, sizeof(report));
	if (count >= 0 || errno != EBUSY)
		return probe_fail("grabbed-write", count >= 0 ? EEXIST : errno);
	error = probe_send(first, channel, PROBE_CMD_WINK, NULL, 0U);
	if (error == 0)
		error = probe_receive(first, channel, &command, message, sizeof(message), &length);
	if (error != 0 || command != PROBE_CMD_WINK)
		return probe_fail("grabbed-wink", error != 0 ? error : EPROTO);
	(void)fcntl(second, F_SETFL, O_NONBLOCK);
	count = read(second, report, sizeof(report));
	if (count >= 0 || errno != EAGAIN)
		return probe_fail("grabbed-read", count >= 0 ? EEXIST : errno);
	(void)fcntl(second, F_SETFL, 0);
	grab = 0;
	error = ioctl(first, HIDRAW_GRAB, &grab);
	if (error != 0)
		return probe_fail("ungrab", errno);
	printf("HIDRAW grab ok\n");

	/* An unknown command answers ERROR with ERR_INVALID_CMD. */
	error = probe_send(first, channel, PROBE_CMD_UNKNOWN, NULL, 0U);
	if (error != 0)
		return probe_fail("unknown-send", error);
	error = probe_receive(first, channel, &command, message, sizeof(message), &length);
	if (error != 0 || command != PROBE_CMD_ERROR || length != 1U || message[0] != 1U)
		return probe_fail("unknown", error != 0 ? error : EPROTO);
	printf("HIDRAW unknown-command ok\n");

	/* Both opens closed. */
	(void)close(second);
	(void)close(first);
	printf("HIDRAW PASS\n");
	return 0;
}

/*
 * Opens the named node, or the first FIDO one, read and write; returns the
 * descriptor with the path, or -1 with errno.
 */
static int
probe_open(
	const char *named,
	char *path,
	size_t size)
{
	struct hidraw_info info;
	unsigned number;
	int descriptor;
	int error;

	/* A named node. */
	if (named != NULL) {
		snprintf(path, size, "%s", named);
		descriptor = open(path, O_RDWR);
		return descriptor;
	}

	/* The first FIDO one. */
	for (number = 0U; number < PROBE_NODES; number++) {
		snprintf(path, size, "/dev/input/hidraw%u", number);
		descriptor = open(path, O_RDWR);
		if (descriptor < 0)
			continue;
		error = ioctl(descriptor, HIDRAW_GET_INFO, &info);
		if (error == 0 && info.usage_page == HIDRAW_USAGE_PAGE_FIDO)
			return descriptor;
		(void)close(descriptor);
	}

	/* None. */
	errno = ENOENT;
	return -1;
}

/* Sends a message: an initialization packet and as many continuations as it takes. */
static int
probe_send(
	int descriptor,
	uint32_t channel,
	uint8_t command,
	const uint8_t *data,
	size_t length)
{
	uint8_t report[PROBE_REPORT + 1U];
	ssize_t count;
	size_t sent;
	size_t take;
	uint8_t sequence;

	/* The initialization packet, after the report ID's byte (0). */
	memset(report, 0, sizeof(report));
	probe_put_be32(report + 1U, channel);
	report[5] = command;
	report[6] = (uint8_t)(length >> 8U);
	report[7] = (uint8_t)length;
	take = length;
	if (take > PROBE_INIT_DATA)
		take = PROBE_INIT_DATA;
	if (take != 0U)
		memcpy(report + 8U, data, take);
	count = write(descriptor, report, sizeof(report));
	if (count != (ssize_t)sizeof(report))
		return count < 0 ? errno : EIO;
	sent = take;

	/* The continuations. */
	sequence = 0U;
	while (sent < length) {
		memset(report, 0, sizeof(report));
		probe_put_be32(report + 1U, channel);
		report[5] = sequence;
		take = length - sent;
		if (take > PROBE_CONT_DATA)
			take = PROBE_CONT_DATA;
		memcpy(report + 6U, data + sent, take);
		count = write(descriptor, report, sizeof(report));
		if (count != (ssize_t)sizeof(report))
			return count < 0 ? errno : EIO;
		sent += take;
		sequence++;
	}

	/* Succeeded: the message went. */
	return 0;
}

/*
 * Receives a message of a channel (other channels' reports are skipped):
 * its command and its data.
 */
static int
probe_receive(
	int descriptor,
	uint32_t channel,
	uint8_t *command,
	uint8_t *data,
	size_t capacity,
	size_t *length)
{
	uint8_t report[PROBE_REPORT];
	size_t received;
	size_t total;
	size_t take;
	uint8_t sequence;
	int error;

	/* The initialization packet of the channel. */
	for (;;) {
		error = probe_report(descriptor, report);
		if (error != 0)
			return error;
		if (probe_be32(report) == channel && (report[4] & 0x80U) != 0U)
			break;
	}
	*command = report[4];
	total = ((size_t)report[5] << 8U) | report[6];
	if (total > capacity)
		return EMSGSIZE;
	take = total;
	if (take > PROBE_INIT_DATA)
		take = PROBE_INIT_DATA;
	memcpy(data, report + 7U, take);
	received = take;

	/* The continuations, in order. */
	sequence = 0U;
	while (received < total) {
		error = probe_report(descriptor, report);
		if (error != 0)
			return error;
		if (probe_be32(report) != channel)
			continue;
		if (report[4] != sequence)
			return EPROTO;
		take = total - received;
		if (take > PROBE_CONT_DATA)
			take = PROBE_CONT_DATA;
		memcpy(data + received, report + 5U, take);
		received += take;
		sequence++;
	}

	/* Succeeded: the whole message. */
	*length = total;
	return 0;
}

/* Reads one report, waiting at most PROBE_WAIT_MS for it. */
static int
probe_report(
	int descriptor,
	uint8_t *report)
{
	struct pollfd wait;
	ssize_t count;
	int ready;

	/* Waits for it. */
	wait.fd = descriptor;
	wait.events = POLLIN;
	wait.revents = 0;
	ready = poll(&wait, 1, PROBE_WAIT_MS);
	if (ready < 0)
		return errno;
	if (ready == 0)
		return ETIMEDOUT;

	/* A whole report. */
	count = read(descriptor, report, PROBE_REPORT);
	if (count < 0)
		return errno;
	if (count != (ssize_t)PROBE_REPORT)
		return EIO;

	/* Succeeded: the report. */
	return 0;
}

/* Says which step failed and why; the exit status is 1. */
static int
probe_fail(
	const char *step,
	int error)
{
	/* The line the test reads. */
	printf("HIDRAW FAIL step=%s error=%d reason=%s\n", step, error, strerror(error));
	return 1;
}

/* Reads a 32-bit big-endian number (a channel). */
static uint32_t
probe_be32(
	const uint8_t *bytes)
{
	/* The most significant byte first. */
	return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
	    ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

/* Writes a 32-bit big-endian number (a channel). */
static void
probe_put_be32(
	uint8_t *bytes,
	uint32_t value)
{
	/* The most significant byte first. */
	bytes[0] = (uint8_t)(value >> 24U);
	bytes[1] = (uint8_t)(value >> 16U);
	bytes[2] = (uint8_t)(value >> 8U);
	bytes[3] = (uint8_t)value;
}
