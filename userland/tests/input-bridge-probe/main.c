/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The input bridge probe (ws143-p005): tries /dev/input/bridge without the
 * Bluetooth daemon.
 *
 *   input-bridge-probe type [-w MS] [-h MS]
 *       makes a keyboard (a boot keyboard's descriptor, the Bluetooth bus,
 *       "Probe Keyboard"), prints "BRIDGE device event=N touch=M
 *       flags=F report_max=R", waits -w MS (2000) for a reader, presses and
 *       releases "a", presses "b", waits -h MS (1000) and closes the file
 *       with "b" still held: the input layer releases it.
 *   input-bridge-probe misuse
 *       tries the refusals: a report before the setup, a malformed setup
 *       and a declaration after it on the same open, a descriptor of 4097
 *       bytes, a FIDO descriptor (ENXIO), a report shorter than declared,
 *       a longer one (taken), one of 513 bytes, a read (EAGAIN), poll
 *       (writable only), INPUT_BRIDGE_GET_DEVICE before and after the setup,
 *       and INPUT_BRIDGE_OPENS_MAX keyboards at once with one more open
 *       refused (EBUSY).
 *   input-bridge-probe open
 *       only opens the node (for a user that must be refused).
 *
 * Each line is "BRIDGE ..."; the last is "BRIDGE PASS" (status 0) or
 * "BRIDGE FAIL step=<what> error=<errno>" (status 1).
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
#include <uapi/input-bridge.h>
#include <uapi/input.h>

/* The node. */
#define PROBE_NODE		"/dev/input/bridge"

/* A boot keyboard's report: modifiers, a reserved byte, six key slots. */
#define PROBE_REPORT_SIZE	8U

/* The HID usages of the keys typed. */
#define PROBE_USAGE_A		0x04U
#define PROBE_USAGE_B		0x05U

/* A boot keyboard's report descriptor: 8-byte reports, no report ID. */
static const uint8_t probe_keyboard[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
	0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
	0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
	0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
	0x81, 0x00, 0xc0
};

/* A security key's report descriptor (FIDO's usage page), which the node refuses. */
static const uint8_t probe_fido[] = {
	0x06, 0xd0, 0xf1, 0x09, 0x01, 0xa1, 0x01, 0x09, 0x20, 0x15, 0x00, 0x26,
	0xff, 0x00, 0x75, 0x08, 0x95, 0x40, 0x81, 0x02, 0x09, 0x21, 0x15, 0x00,
	0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x40, 0x91, 0x02, 0xc0
};

static int probe_type(int argc, char **argv);
static int probe_misuse(void);
static int probe_only_open(void);
static void probe_setup(struct input_bridge_setup *setup, const uint8_t *descriptor, size_t size, unsigned number);
static int probe_declare(int descriptor, unsigned number);
static int probe_key(int descriptor, uint8_t usage);
static int probe_device(int descriptor, struct input_bridge_device *device);
static int probe_expect_write(int descriptor, const void *bytes, size_t size, int wanted, const char *step);
static int probe_fail(const char *step, int error);
static void probe_sleep_ms(long ms);

/*
 * Runs the mode the first argument names.
 */
int
main(
	int argc,
	char **argv)
{
	int same;
	int status;

	/* Each line is written as it comes. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* The mode. */
	if (argc < 2) {
		fprintf(stderr, "usage: input-bridge-probe type [-w MS] [-h MS] | misuse | open\n");
		return 2;
	}

	/* Typing on a keyboard. */
	same = strcmp(argv[1], "type");
	if (same == 0) {
		status = probe_type(argc - 1, argv + 1);
		return status;
	}

	/* The refusals. */
	same = strcmp(argv[1], "misuse");
	if (same == 0) {
		status = probe_misuse();
		return status;
	}

	/* An open alone. */
	same = strcmp(argv[1], "open");
	if (same == 0) {
		status = probe_only_open();
		return status;
	}

	/* Neither. */
	fprintf(stderr, "input-bridge-probe: unknown mode %s\n", argv[1]);
	return 2;
}

/* Makes a keyboard and types "a", then holds "b" until the file is closed. */
static int
probe_type(
	int argc,
	char **argv)
{
	struct input_bridge_device device;
	long wait_ms;
	long hold_ms;
	int descriptor;
	int option;
	int error;

	/* The waits. */
	wait_ms = 2000;
	hold_ms = 1000;
	for (;;) {
		option = getopt(argc, argv, "w:h:");
		if (option == -1)
			break;

		/* What the option sets. */
		switch (option) {
		case 'w':
			wait_ms = strtol(optarg, NULL, 10);
			break;
		case 'h':
			hold_ms = strtol(optarg, NULL, 10);
			break;
		default:
			return 2;
		}
	}

	/* The node and the keyboard. */
	descriptor = open(PROBE_NODE, O_RDWR);
	if (descriptor < 0)
		return probe_fail("open", errno);
	error = probe_declare(descriptor, 0U);
	if (error != 0)
		return probe_fail("setup", error);

	/* What it made. */
	error = probe_device(descriptor, &device);
	if (error != 0)
		return probe_fail("get-device", error);
	printf("BRIDGE device event=%d touch=%d flags=%u report_max=%u malformed=%u\n",
	       (int)device.event,
	       (int)device.touch_event,
	       (unsigned)device.flags,
	       (unsigned)device.report_max,
	       (unsigned)device.malformed);

	/* A reader's time to find the node. */
	probe_sleep_ms(wait_ms);

	/* "a" pressed and released, then "b" pressed. */
	error = probe_key(descriptor, PROBE_USAGE_A);
	if (error != 0)
		return probe_fail("a-down", error);
	error = probe_key(descriptor, 0U);
	if (error != 0)
		return probe_fail("a-up", error);
	error = probe_key(descriptor, PROBE_USAGE_B);
	if (error != 0)
		return probe_fail("b-down", error);
	printf("BRIDGE typed a, holding b\n");

	/* "b" still held when the file closes. */
	probe_sleep_ms(hold_ms);
	(void)close(descriptor);
	printf("BRIDGE closed\n");
	printf("BRIDGE PASS\n");
	return 0;
}

/* Tries every refusal of the node; each must answer as <uapi/input-bridge.h> says. */
static int
probe_misuse(
	void)
{
	static struct input_bridge_setup setup;
	static uint8_t report[INPUT_BRIDGE_REPORT_MAX + 1U];
	struct input_bridge_device device;
	struct pollfd entry;
	int descriptors[INPUT_BRIDGE_OPENS_MAX];
	unsigned index;
	ssize_t count;
	int descriptor;
	int extra;
	int error;

	/* One open for the single-open checks. */
	descriptor = open(PROBE_NODE, O_RDWR);
	if (descriptor < 0)
		return probe_fail("open", errno);

	/* Before the setup: no device, and a report is refused. */
	error = probe_device(descriptor, &device);
	if (error != 0 || device.event != -1)
		return probe_fail("get-device-before", error);
	memset(report, 0, sizeof(report));
	error = probe_expect_write(descriptor, report, PROBE_REPORT_SIZE, EINVAL, "report-before-setup");
	if (error != 0)
		return 1;

	/* A malformed setup, a descriptor too long, and a security key's: each refused. */
	probe_setup(&setup, probe_keyboard, sizeof(probe_keyboard), 0U);
	setup.magic ^= 1U;
	error = probe_expect_write(descriptor, &setup, sizeof(setup), EINVAL, "bad-magic");
	if (error != 0)
		return 1;
	probe_setup(&setup, probe_keyboard, sizeof(probe_keyboard), 0U);
	setup.descriptor_size = INPUT_BRIDGE_DESCRIPTOR_MAX + 1U;
	error = probe_expect_write(descriptor, &setup, sizeof(setup), EINVAL, "descriptor-4097");
	if (error != 0)
		return 1;
	probe_setup(&setup, probe_fido, sizeof(probe_fido), 0U);
	error = probe_expect_write(descriptor, &setup, sizeof(setup), ENXIO, "fido");
	if (error != 0)
		return 1;
	error = probe_expect_write(descriptor, &setup, 100U, EINVAL, "setup-wrong-size");
	if (error != 0)
		return 1;

	/* The same open declares after the refusals. */
	error = probe_declare(descriptor, 0U);
	if (error != 0)
		return probe_fail("setup-after-refusals", error);
	error = probe_device(descriptor, &device);
	if (error != 0 || device.event < 0 || device.report_max != PROBE_REPORT_SIZE || device.flags != 0U)
		return probe_fail("get-device-after", error);
	printf("BRIDGE ok declared event=%d report_max=%u\n", (int)device.event, (unsigned)device.report_max);

	/* Reports: short refused, long taken, too long refused. */
	error = probe_expect_write(descriptor, report, PROBE_REPORT_SIZE - 1U, EINVAL, "report-short");
	if (error != 0)
		return 1;
	error = probe_expect_write(descriptor, report, PROBE_REPORT_SIZE + 1U, 0, "report-long");
	if (error != 0)
		return 1;
	error = probe_expect_write(descriptor, report, INPUT_BRIDGE_REPORT_MAX + 1U, EINVAL, "report-513");
	if (error != 0)
		return 1;

	/* A read never gives anything, and poll says writable only. */
	count = read(descriptor, report, sizeof(report));
	if (count >= 0 || errno != EAGAIN)
		return probe_fail("read", errno);
	printf("BRIDGE ok read EAGAIN\n");
	entry.fd = descriptor;
	entry.events = POLLIN | POLLOUT;
	entry.revents = 0;
	error = poll(&entry, 1, 0);
	if (error != 1 || entry.revents != POLLOUT)
		return probe_fail("poll", errno);
	printf("BRIDGE ok poll POLLOUT\n");
	(void)close(descriptor);

	/* INPUT_BRIDGE_OPENS_MAX keyboards at once, and one open more refused. */
	for (index = 0; index < INPUT_BRIDGE_OPENS_MAX; index++) {
		descriptors[index] = open(PROBE_NODE, O_RDWR);
		if (descriptors[index] < 0)
			return probe_fail("open-many", errno);
		error = probe_declare(descriptors[index], index + 1U);
		if (error != 0)
			return probe_fail("setup-many", error);
	}

	/* All made; one open more is refused. */
	printf("BRIDGE ok %u keyboards\n", INPUT_BRIDGE_OPENS_MAX);
	extra = open(PROBE_NODE, O_RDWR);
	if (extra >= 0 || errno != EBUSY)
		return probe_fail("open-one-more", errno);
	printf("BRIDGE ok one more open EBUSY\n");
	for (index = 0; index < INPUT_BRIDGE_OPENS_MAX; index++)
		(void)close(descriptors[index]);

	/* Succeeded: every refusal answered as it should. */
	printf("BRIDGE PASS\n");
	return 0;
}

/* Opens the node and closes it again; the status says whether the open was allowed. */
static int
probe_only_open(
	void)
{
	int descriptor;

	/* The open. */
	descriptor = open(PROBE_NODE, O_RDWR);
	if (descriptor < 0)
		return probe_fail("open", errno);
	(void)close(descriptor);

	/* Succeeded: the node was opened. */
	printf("BRIDGE PASS\n");
	return 0;
}

/* Fills a setup: the Bluetooth bus, a probe's names (numbered when number is not 0), and a descriptor. */
static void
probe_setup(
	struct input_bridge_setup *setup,
	const uint8_t *descriptor,
	size_t size,
	unsigned number)
{
	/* The form, the bus and the vendor's numbers. */
	memset(setup, 0, sizeof(*setup));
	setup->magic = INPUT_BRIDGE_MAGIC;
	setup->version = INPUT_BRIDGE_VERSION;
	setup->bus = BUS_BLUETOOTH;
	setup->vendor = 0x1234;
	setup->product = 0x5678;
	setup->release = 0x0100;

	/* The names. */
	if (number == 0U)
		snprintf(setup->name, sizeof(setup->name), "Probe Keyboard");
	else
		snprintf(setup->name, sizeof(setup->name), "Probe Keyboard %u", number);
	snprintf(setup->physical_path, sizeof(setup->physical_path), "bluetooth/probe/%u", number);
	snprintf(setup->unique_id, sizeof(setup->unique_id), "probe-%u", number);

	/* The descriptor. */
	setup->descriptor_size = (uint32_t)size;
	memcpy(setup->descriptor, descriptor, size);
}

/* Declares a probe keyboard on an open; an errno on a refusal. */
static int
probe_declare(
	int descriptor,
	unsigned number)
{
	static struct input_bridge_setup setup;
	ssize_t written;

	/* The setup, in one write. */
	probe_setup(&setup, probe_keyboard, sizeof(probe_keyboard), number);
	written = write(descriptor, &setup, sizeof(setup));
	if (written < 0)
		return errno;
	if ((size_t)written != sizeof(setup))
		return EIO;

	/* Succeeded: the keyboard is made. */
	return 0;
}

/* Writes one keyboard report holding one key (0: none); an errno on a refusal. */
static int
probe_key(
	int descriptor,
	uint8_t usage)
{
	uint8_t report[PROBE_REPORT_SIZE];
	ssize_t written;

	/* The key in the first slot. */
	memset(report, 0, sizeof(report));
	report[2] = usage;
	written = write(descriptor, report, sizeof(report));
	if (written < 0)
		return errno;
	if ((size_t)written != sizeof(report))
		return EIO;

	/* Succeeded: the report was taken. */
	return 0;
}

/* Asks the node what an open made; an errno on a refusal. */
static int
probe_device(
	int descriptor,
	struct input_bridge_device *device)
{
	int error;

	/* INPUT_BRIDGE_GET_DEVICE. */
	memset(device, 0, sizeof(*device));
	error = ioctl(descriptor, INPUT_BRIDGE_GET_DEVICE, device);
	if (error != 0)
		return errno;

	/* Succeeded: the answer is filled. */
	return 0;
}

/*
 * Writes bytes and checks the answer: wanted 0 means the whole write is
 * taken, otherwise the write must be refused with that errno.  Prints the
 * step's line; nonzero (after the FAIL line) when the answer differs.
 */
static int
probe_expect_write(
	int descriptor,
	const void *bytes,
	size_t size,
	int wanted,
	const char *step)
{
	ssize_t written;
	int error;

	/* The write and its answer. */
	written = write(descriptor, bytes, size);
	error = 0;
	if (written < 0)
		error = errno;

	/* A write that had to be taken. */
	if (wanted == 0) {
		if (written < 0 || (size_t)written != size) {
			(void)probe_fail(step, error);
			return 1;
		}

		/* Taken whole. */
		printf("BRIDGE ok %s taken\n", step);
		return 0;
	}

	/* A write that had to be refused with wanted. */
	if (written >= 0 || error != wanted) {
		(void)probe_fail(step, error);
		return 1;
	}

	/* Succeeded: refused as it should be. */
	printf("BRIDGE ok %s errno=%d\n", step, error);
	return 0;
}

/* Prints the failed step and reports the failure's status. */
static int
probe_fail(
	const char *step,
	int error)
{
	/* The last line. */
	printf("BRIDGE FAIL step=%s error=%d\n", step, error);
	return 1;
}

/*
 * Sleeps for a number of milliseconds (usleep refuses a second or more
 * with EINVAL, as POSIX allows, so the waits of type returned at once).
 */
static void
probe_sleep_ms(
	long ms)
{
	struct timespec request;

	/* Nothing to wait for. */
	if (ms <= 0)
		return;

	/* The whole wait, again after a signal. */
	request.tv_sec = (time_t)(ms / 1000);
	request.tv_nsec = (ms % 1000) * 1000000L;
	for (;;) {
		int result;

		/* Slept the whole wait, or failed other than by a signal. */
		result = nanosleep(&request, &request);
		if (result == 0 || errno != EINTR)
			break;
	}
}
