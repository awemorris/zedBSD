/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SCO loopback test (ws197-p007a2 L2, L3): streams SCO packets through
 * a controller's HCI Local Loopback and measures what comes back.
 *
 *   bt-sco-loopback [-f DEVICE] [-t SECONDS] [-T]
 *
 * bluetoothd must be stopped (the node has one open at a time).  The test
 * resets the controller, reads its SCO buffer size, sets the Voice Setting
 * (16-bit CVSD, 0x0060; with -T transparent, 0x0063), and turns Local
 * Loopback on: the controller then reports loopback connections with
 * Connection Complete events, one of SCO among them.  The SCO link is set
 * on the node (BT_IOC_SET_SCO: 48 data bytes a packet, 60 with -T), and
 * for SECONDS (30 by default) a packet goes every 3 ms (3.75 ms with -T),
 * numbered in its first four data bytes and the rest a pattern of the
 * number; each packet that comes back is matched by its number.  At the
 * end the link is stopped, Loopback turned off and the controller reset.
 *
 * Each line is "BTSCO ..." on standard output: the controller's
 * information, the SCO buffer, the connections, the alternate chosen, and
 * at the end the counts (sent, back, missing, out of order, damaged, the
 * latency's median and largest in milliseconds) and the node's counts.  The
 * last line is "BTSCO PASS" (back at least 99%, none out of order or
 * damaged, the largest latency at most 100 ms; status 0) or "BTSCO FAIL"
 * with the reason (status 1).
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <uapi/bluetooth.h>

/* The commands (OGF and OCF together) and events used. */
#define LOOP_OP_RESET		0x0c03U
#define LOOP_OP_WRITE_VOICE	0x0c26U
#define LOOP_OP_BUFFER_SIZE	0x1005U
#define LOOP_OP_WRITE_LOOPBACK	0x1802U
#define LOOP_EVENT_CONNECTED	0x03U
#define LOOP_EVENT_COMPLETE	0x0eU
#define LOOP_EVENT_STATUS	0x0fU
#define LOOP_EVENT_SYNCHRONOUS	0x2cU

/* The Voice Settings, the SCO data a packet, the packet period (microseconds). */
#define LOOP_VOICE_CVSD		0x0060U
#define LOOP_VOICE_TRANSPARENT	0x0063U
#define LOOP_DATA_CVSD		48U
#define LOOP_DATA_TRANSPARENT	60U
#define LOOP_PERIOD_CVSD_US	3000U
#define LOOP_PERIOD_TRANSPARENT_US	3750U

/* How long a command, and the loopback's connections, may take (milliseconds). */
#define LOOP_WAIT_MS		2000

/* The send times kept by number, and the most connections noted. */
#define LOOP_TIMES		8192U
#define LOOP_CONNECTIONS	8U

/* The pass's limits: back at least 99%, the largest latency 100 ms. */
#define LOOP_BACK_PERCENT	99U
#define LOOP_LATENCY_MAX_MS	100U

/* What the test measures. */
struct loop_counts {
	uint32_t sent;
	uint32_t back;
	uint32_t disorder;
	uint32_t damaged;
	uint32_t last;
	int any;
	uint32_t latency_ms[LOOP_TIMES];
	uint32_t latencies;
};

static int64_t loop_now_us(void);
static int loop_read(int descriptor, uint8_t *packet, size_t capacity, size_t *length, int timeout_ms);
static int loop_command(int descriptor, uint16_t opcode, const uint8_t *parameters, size_t count, uint8_t *answer, size_t capacity, size_t *length, uint16_t *sco);
static void loop_note_connection(const uint8_t *packet, size_t length, uint16_t *sco);
static void loop_fill(uint8_t *packet, uint16_t handle, unsigned data, uint32_t number);
static void loop_take(const uint8_t *packet, size_t length, unsigned data, const int64_t *times, struct loop_counts *counts);
static int loop_compare(const void *left, const void *right);

/* Runs the test; the exit status says whether it passed. */
int
main(
	int argc,
	char **argv)
{
	static int64_t times[LOOP_TIMES];
	static struct loop_counts counts;
	struct bt_info info;
	struct bt_stats stats;
	struct bt_sco links;
	uint8_t answer[BT_EVENT_PACKET_MAX];
	uint8_t packet[BT_ACL_PACKET_MAX];
	uint8_t parameter[2];
	const char *path;
	int64_t started;
	int64_t next;
	int64_t now;
	uint32_t number;
	uint32_t median;
	uint32_t largest;
	uint16_t voice;
	uint16_t sco;
	unsigned data;
	unsigned period;
	unsigned seconds;
	unsigned percent;
	size_t length;
	ssize_t written;
	int descriptor;
	int transparent;
	int option;
	int error;

	/* The options. */
	path = "/dev/bluetooth0";
	seconds = 30U;
	transparent = 0;
	option = getopt(argc, argv, "f:t:T");
	while (option != -1) {
		if (option == 'f') {
			path = optarg;
		} else if (option == 't') {
			seconds = (unsigned)strtoul(optarg, NULL, 10);
		} else if (option == 'T') {
			transparent = 1;
		} else {
			fprintf(stderr, "usage: bt-sco-loopback [-f DEVICE] [-t SECONDS] [-T]\n");
			return 2;
		}

		/* The next option. */
		option = getopt(argc, argv, "f:t:T");
	}

	/* The coding. */
	voice = LOOP_VOICE_CVSD;
	data = LOOP_DATA_CVSD;
	period = LOOP_PERIOD_CVSD_US;
	if (transparent) {
		voice = LOOP_VOICE_TRANSPARENT;
		data = LOOP_DATA_TRANSPARENT;
		period = LOOP_PERIOD_TRANSPARENT_US;
	}

	/* The node (bluetoothd stopped). */
	descriptor = open(path, O_RDWR | O_CLOEXEC);
	if (descriptor < 0) {
		printf("BTSCO FAIL open error=%d (is bluetoothd stopped?)\n", errno);
		return 1;
	}

	/* A controller with SCO. */
	memset(&info, 0, sizeof(info));
	error = ioctl(descriptor, BT_IOC_GET_INFO, &info);
	if (error != 0 || (info.flags & BT_INFO_SCO) == 0U) {
		printf("BTSCO FAIL info flags=0x%x (no SCO, or no answer)\n", (unsigned)info.flags);
		return 1;
	}

	/* Its name. */
	info.name[BT_TEXT_MAX - 1U] = '\0';
	printf("BTSCO info vendor=%04x product=%04x name=\"%s\"\n", (unsigned)info.vendor, (unsigned)info.product, info.name);

	/* Reset, the SCO buffer, the Voice Setting. */
	sco = 0xffffU;
	error = loop_command(descriptor, LOOP_OP_RESET, NULL, 0U, answer, sizeof(answer), &length, &sco);
	if (error == 0)
		error = loop_command(descriptor, LOOP_OP_BUFFER_SIZE, NULL, 0U, answer, sizeof(answer), &length, &sco);
	if (error != 0) {
		printf("BTSCO FAIL reset-or-buffer error=%d\n", error);
		return 1;
	}

	/* The SCO buffer, then the Voice Setting. */
	if (length >= 8U)
		printf("BTSCO buffer sco_length=%u sco_count=%u\n", (unsigned)answer[3], (unsigned)(answer[6] | (answer[7] << 8)));
	parameter[0] = (uint8_t)(voice & 0xffU);
	parameter[1] = (uint8_t)(voice >> 8);
	error = loop_command(descriptor, LOOP_OP_WRITE_VOICE, parameter, 2U, answer, sizeof(answer), &length, &sco);
	if (error != 0) {
		printf("BTSCO FAIL voice-setting error=%d\n", error);
		return 1;
	}

	/* Local Loopback on; its connections come as events. */
	parameter[0] = 1U;
	error = loop_command(descriptor, LOOP_OP_WRITE_LOOPBACK, parameter, 1U, answer, sizeof(answer), &length, &sco);
	if (error != 0) {
		printf("BTSCO FAIL loopback-on error=%d\n", error);
		return 1;
	}

	/* The connections, until an SCO one or the time is up. */
	started = loop_now_us();
	now = started;
	while (sco == 0xffffU && now - started < (int64_t)LOOP_WAIT_MS * 1000) {
		error = loop_read(descriptor, packet, sizeof(packet), &length, 100);
		if (error == 0 && packet[0] == BT_PACKET_EVENT)
			loop_note_connection(packet, length, &sco);
		now = loop_now_us();
	}

	/* None of SCO. */
	if (sco == 0xffffU) {
		printf("BTSCO FAIL no-sco-connection\n");
		return 1;
	}

	/* The SCO link. */
	printf("BTSCO connection sco=0x%04x\n", (unsigned)sco);

	/* The link on the node. */
	memset(&links, 0, sizeof(links));
	links.count = 1U;
	links.handle[0] = sco;
	links.voice_setting[0] = voice;
	links.packet_length[0] = (uint16_t)data;
	error = ioctl(descriptor, BT_IOC_SET_SCO, &links);
	if (error != 0) {
		printf("BTSCO FAIL set-sco error=%d\n", errno);
		return 1;
	}

	/* The alternate the kernel chose. */
	printf("BTSCO streaming alternate=%u mtu=%u\n", (unsigned)links.alternate, (unsigned)links.mtu);

	/* Send a packet a period, take what comes back, for the seconds asked. */
	memset(&counts, 0, sizeof(counts));
	number = 0U;
	started = loop_now_us();
	next = started;
	for (;;) {
		now = loop_now_us();
		if (now - started >= (int64_t)seconds * 1000000)
			break;

		/* The packets due. */
		while (now >= next) {
			loop_fill(packet, sco, data, number);
			written = write(descriptor, packet, 1U + BT_SCO_HEADER + data);
			if (written < 0 && errno == EAGAIN)
				break;
			if (written < 0) {
				printf("BTSCO FAIL write error=%d\n", errno);
				return 1;
			}

			/* Sent: its time kept by its number. */
			times[number % LOOP_TIMES] = now;
			number++;
			counts.sent++;
			next += period;
		}

		/* What comes back meanwhile. */
		error = loop_read(descriptor, packet, sizeof(packet), &length, 1);
		if (error == 0 && packet[0] == BT_PACKET_SCO)
			loop_take(packet, length, data, times, &counts);
	}

	/* The last ones in flight. */
	started = loop_now_us();
	now = started;
	while (now - started < 300000) {
		error = loop_read(descriptor, packet, sizeof(packet), &length, 10);
		if (error == 0 && packet[0] == BT_PACKET_SCO)
			loop_take(packet, length, data, times, &counts);
		now = loop_now_us();
	}

	/* The link stopped, Loopback off, the controller reset. */
	memset(&links, 0, sizeof(links));
	(void)ioctl(descriptor, BT_IOC_SET_SCO, &links);
	parameter[0] = 0U;
	(void)loop_command(descriptor, LOOP_OP_WRITE_LOOPBACK, parameter, 1U, answer, sizeof(answer), &length, &sco);
	(void)loop_command(descriptor, LOOP_OP_RESET, NULL, 0U, answer, sizeof(answer), &length, &sco);

	/* The counts. */
	qsort(counts.latency_ms, counts.latencies, sizeof(counts.latency_ms[0]), loop_compare);
	percent = 0U;
	if (counts.sent != 0U)
		percent = (unsigned)((uint64_t)counts.back * 100U / counts.sent);
	median = 0U;
	largest = 0U;
	if (counts.latencies != 0U) {
		median = counts.latency_ms[counts.latencies / 2U];
		largest = counts.latency_ms[counts.latencies - 1U];
	}

	/* The line. */
	printf("BTSCO sent=%u back=%u missing=%u disorder=%u damaged=%u latency_median_ms=%u latency_max_ms=%u\n",
	    (unsigned)counts.sent, (unsigned)counts.back, (unsigned)(counts.sent - counts.back), (unsigned)counts.disorder,
	    (unsigned)counts.damaged, (unsigned)median, (unsigned)largest);

	/* The node's counts. */
	memset(&stats, 0, sizeof(stats));
	error = ioctl(descriptor, BT_IOC_GET_STATS, &stats);
	if (error == 0) {
		printf("BTSCO stats sco_in=%llu sco_out=%llu sco_dropped=%llu\n", (unsigned long long)stats.sco_in,
		    (unsigned long long)stats.sco_out, (unsigned long long)stats.sco_dropped);
	}

	/* The node is done with. */
	(void)close(descriptor);

	/* The verdict. */
	if (percent < LOOP_BACK_PERCENT || counts.disorder != 0U || counts.damaged != 0U || largest > LOOP_LATENCY_MAX_MS) {
		printf("BTSCO FAIL back=%u%%\n", percent);
		return 1;
	}

	/* Passed. */
	printf("BTSCO PASS\n");
	return 0;
}

/* The monotonic clock in microseconds. */
static int64_t
loop_now_us(
	void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: microseconds. */
	return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

/* Reads one packet, waiting up to timeout_ms; 0, ETIMEDOUT, or an errno value. */
static int
loop_read(
	int descriptor,
	uint8_t *packet,
	size_t capacity,
	size_t *length,
	int timeout_ms)
{
	struct pollfd wait;
	ssize_t got;
	int ready;

	/* Something to read. */
	wait.fd = descriptor;
	wait.events = POLLIN;
	wait.revents = 0;
	ready = poll(&wait, 1, timeout_ms);
	if (ready <= 0)
		return ETIMEDOUT;

	/* One packet. */
	got = read(descriptor, packet, capacity);
	if (got <= 0)
		return EIO;

	/* Succeeded: its length. */
	*length = (size_t)got;
	return 0;
}

/*
 * Sends a command and waits for its Command Complete (its parameters in
 * answer, status first) or a failed Command Status; the connections that
 * come meanwhile are noted.  Returns 0, the controller's status as EIO, or
 * ETIMEDOUT.
 */
static int
loop_command(
	int descriptor,
	uint16_t opcode,
	const uint8_t *parameters,
	size_t count,
	uint8_t *answer,
	size_t capacity,
	size_t *length,
	uint16_t *sco)
{
	uint8_t command[BT_COMMAND_PACKET_MAX];
	uint8_t packet[BT_ACL_PACKET_MAX];
	int64_t started;
	int64_t now;
	ssize_t written;
	size_t got;
	uint16_t answered;
	int error;

	/* The command. */
	command[0] = BT_PACKET_COMMAND;
	command[1] = (uint8_t)(opcode & 0xffU);
	command[2] = (uint8_t)(opcode >> 8);
	command[3] = (uint8_t)count;
	if (count != 0U)
		memcpy(command + 4, parameters, count);
	written = write(descriptor, command, 4U + count);
	if (written < 0)
		return errno;

	/* Its answer. */
	started = loop_now_us();
	now = started;
	while (now - started < (int64_t)LOOP_WAIT_MS * 1000) {
		error = loop_read(descriptor, packet, sizeof(packet), &got, 100);
		now = loop_now_us();
		if (error != 0 || packet[0] != BT_PACKET_EVENT)
			continue;
		loop_note_connection(packet, got, sco);

		/* Command Complete: its opcode and its parameters. */
		if (packet[1] == LOOP_EVENT_COMPLETE && got >= 6U) {
			answered = (uint16_t)(packet[4] | (packet[5] << 8));
			if (answered != opcode)
				continue;
			*length = got - 6U;
			if (*length > capacity)
				*length = capacity;
			memcpy(answer, packet + 6, *length);
			if (*length != 0U && answer[0] != 0U)
				return EIO;
			return 0;
		}

		/* Command Status: a failure ends it. */
		if (packet[1] == LOOP_EVENT_STATUS && got >= 7U) {
			answered = (uint16_t)(packet[5] | (packet[6] << 8));
			if (answered == opcode && packet[3] != 0U)
				return EIO;
		}
	}

	/* No answer. */
	return ETIMEDOUT;
}

/* Notes an SCO connection a Connection Complete or Synchronous Connection Complete event reports. */
static void
loop_note_connection(
	const uint8_t *packet,
	size_t length,
	uint16_t *sco)
{
	uint16_t handle;

	/* Connection Complete: status, handle, address, link type 0 (SCO). */
	if (packet[1] == LOOP_EVENT_CONNECTED && length >= 3U + 11U && packet[3] == 0U) {
		handle = (uint16_t)((packet[4] | (packet[5] << 8)) & 0x0fffU);
		printf("BTSCO connected handle=0x%04x type=%u\n", (unsigned)handle, (unsigned)packet[12]);
		if (packet[12] == 0U && *sco == 0xffffU)
			*sco = handle;
		return;
	}

	/* Synchronous Connection Complete: status, handle, address, link type (0 SCO, 2 eSCO). */
	if (packet[1] == LOOP_EVENT_SYNCHRONOUS && length >= 3U + 9U && packet[3] == 0U) {
		handle = (uint16_t)((packet[4] | (packet[5] << 8)) & 0x0fffU);
		printf("BTSCO synchronous handle=0x%04x type=%u\n", (unsigned)handle, (unsigned)packet[12]);
		if (*sco == 0xffffU)
			*sco = handle;
	}
}

/* Builds a numbered SCO packet: the number in the first four data bytes, the rest from it. */
static void
loop_fill(
	uint8_t *packet,
	uint16_t handle,
	unsigned data,
	uint32_t number)
{
	unsigned index;

	/* The type and the header. */
	packet[0] = BT_PACKET_SCO;
	packet[1] = (uint8_t)(handle & 0xffU);
	packet[2] = (uint8_t)(handle >> 8);
	packet[3] = (uint8_t)data;

	/* The number, then the pattern. */
	packet[4] = (uint8_t)(number >> 24);
	packet[5] = (uint8_t)(number >> 16);
	packet[6] = (uint8_t)(number >> 8);
	packet[7] = (uint8_t)number;
	for (index = 4U; index < data; index++)
		packet[4U + index] = (uint8_t)(number + index);
}

/* Matches a packet that came back: its number, its order, its pattern and its latency. */
static void
loop_take(
	const uint8_t *packet,
	size_t length,
	unsigned data,
	const int64_t *times,
	struct loop_counts *counts)
{
	uint32_t number;
	unsigned index;
	int64_t latency;

	/* A whole packet of the length sent. */
	if (length != 1U + BT_SCO_HEADER + data) {
		counts->damaged++;
		return;
	}

	/* Its number and pattern. */
	number = ((uint32_t)packet[4] << 24) | ((uint32_t)packet[5] << 16) | ((uint32_t)packet[6] << 8) | packet[7];
	for (index = 4U; index < data; index++) {
		if (packet[4U + index] != (uint8_t)(number + index)) {
			counts->damaged++;
			return;
		}
	}

	/* Its order. */
	if (counts->any && number <= counts->last)
		counts->disorder++;
	counts->last = number;
	counts->any = 1;
	counts->back++;

	/* Its latency, while its send time is kept. */
	if (number + LOOP_TIMES > counts->sent && counts->latencies < LOOP_TIMES) {
		latency = loop_now_us() - times[number % LOOP_TIMES];
		counts->latency_ms[counts->latencies] = (uint32_t)(latency / 1000);
		counts->latencies++;
	}
}

/* Orders latencies for the median. */
static int
loop_compare(
	const void *left,
	const void *right)
{
	uint32_t a;
	uint32_t b;

	/* Smaller first. */
	a = *(const uint32_t *)left;
	b = *(const uint32_t *)right;
	if (a < b)
		return -1;
	if (a > b)
		return 1;

	/* Equal. */
	return 0;
}
