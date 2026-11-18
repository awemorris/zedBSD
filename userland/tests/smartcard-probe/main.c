/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The smart card probe (ws161-p003): tries a slot /dev/smartcardN.
 *
 *   smartcard-probe [-f DEVICE]      the full test against the test kernel's loopback card
 *   smartcard-probe -i [-f DEVICE]   the information and the state of a slot (any reader);
 *                                    with a card, its ATR and the answer to SELECT of FIDO's applet
 *
 * Without -f it takes the first /dev/smartcardN (with the full test, the
 * loopback card's).  The full test: the state, the claim (a second open
 * cannot power the card, EBUSY, nor send it APDUs, EPERM), SELECT of
 * FIDO's applet ("FIDO_2_0" 90 00) and of another (6A 82), an answer of
 * 500 bytes in parts by GET RESPONSE (61 xx), an answer too long for the
 * caller's room (EMSGSIZE), the card taken out and put back (the second
 * open reads the two events, the next APDU answers ENXIO), and the close
 * of the claim's holder (the card is powered off, the second open may
 * claim it).  Each line is "SMARTCARD ..." on standard output; the last is
 * "SMARTCARD PASS" (status 0) or "SMARTCARD FAIL step=<what> error=<errno>"
 * (status 1).
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
#include <uapi/ccid.h>

/* The nodes looked at, and how long an event may take. */
#define PROBE_NODES		16U
#define PROBE_WAIT_MS		3000

/* The longest answer collected, and the test's long answer. */
#define PROBE_ANSWER_MAX	4096U
#define PROBE_LONG		500U

static int probe_open(const char *named, int loopback, char *path, size_t size);
static int probe_send(int descriptor, const uint8_t *command, size_t size, uint8_t *answer, size_t capacity, size_t *length);
static int probe_event(int descriptor, struct ccid_event *event);
static int probe_fail(const char *step, int error);
static void probe_print_atr(const char *what, const struct ccid_status *status);

/* The commands of the test. */
static const uint8_t probe_select_fido[] = { 0x00, 0xa4, 0x04, 0x00, 0x08, 0xa0, 0x00, 0x00, 0x06, 0x47, 0x2f, 0x00, 0x01 };
static const uint8_t probe_select_other[] = { 0x00, 0xa4, 0x04, 0x00, 0x05, 0xa0, 0x00, 0x00, 0x00, 0x03 };
static const uint8_t probe_long[] = { 0x80, 0xcb, 0x00, 0x00, 0x02, (uint8_t)(PROBE_LONG >> 8), (uint8_t)PROBE_LONG };
static const uint8_t probe_get_response[] = { 0x00, 0xc0, 0x00, 0x00, 0x00 };
static const uint8_t probe_reinsert[] = { 0x80, 0xfe, 0x00, 0x00 };

/*
 * Tries the slot; the exit status says whether every step held.
 */
int
main(
	int argc,
	char **argv)
{
	struct ccid_info info;
	struct ccid_status status;
	struct ccid_event event;
	struct ccid_transmit transmit;
	uint8_t answer[PROBE_ANSWER_MAX];
	uint8_t collected[PROBE_ANSWER_MAX];
	uint8_t small[10];
	char path[64];
	const char *named;
	size_t length;
	size_t total;
	size_t index;
	int only_info;
	int first;
	int second;
	int argument;
	int error;

	/* -i and -f. */
	only_info = 0;
	named = NULL;
	for (argument = 1; argument < argc; argument++) {
		if (strcmp(argv[argument], "-i") == 0)
			only_info = 1;
		else if (strcmp(argv[argument], "-f") == 0 && argument + 1 < argc)
			named = argv[++argument];
	}

	/* The slot. */
	first = probe_open(named, !only_info, path, sizeof(path));
	if (first < 0)
		return probe_fail("open", errno);

	/* What it is, and its state. */
	error = ioctl(first, CCID_GET_INFO, &info);
	if (error != 0)
		return probe_fail("info", errno);
	printf("SMARTCARD info path=%s vendor=%04x product=%04x slot=%u/%u features=%08x protocols=%08x message=%u command=%u response=%u flags=%x name=\"%s\"\n",
	    path, info.vendor, info.product, info.slot, info.slot_count, info.features, info.protocols,
	    info.max_message, info.max_command, info.max_response, info.flags, info.name);
	error = ioctl(first, CCID_GET_STATUS, &status);
	if (error != 0)
		return probe_fail("status", errno);
	printf("SMARTCARD status state=%u changes=%u\n", status.state, status.changes);

	/* -i: with a card, its ATR and the answer to SELECT of FIDO's applet. */
	if (only_info) {
		if (status.state == CCID_CARD_ABSENT) {
			printf("SMARTCARD PASS (no card)\n");
			return 0;
		}
		error = ioctl(first, CCID_POWER_ON, &status);
		if (error != 0)
			return probe_fail("power-on", errno);
		probe_print_atr("atr", &status);
		error = probe_send(first, probe_select_fido, sizeof(probe_select_fido), answer, sizeof(answer), &length);
		if (error != 0)
			return probe_fail("select-fido", error);
		printf("SMARTCARD select-fido bytes=%u sw=%02x%02x\n", (unsigned)length,
		    answer[length - 2U], answer[length - 1U]);
		(void)close(first);
		printf("SMARTCARD PASS\n");
		return 0;
	}

	/* The loopback card is there, not powered. */
	if (status.state != CCID_CARD_PRESENT)
		return probe_fail("state-present", EPROTO);

	/* A second open, which watches the events. */
	second = open(path, O_RDWR);
	if (second < 0)
		return probe_fail("open-second", errno);

	/* The first open claims the slot and powers the card. */
	error = ioctl(first, CCID_POWER_ON, &status);
	if (error != 0)
		return probe_fail("power-on", errno);
	probe_print_atr("atr", &status);
	if (status.state != CCID_CARD_POWERED || status.atr_size == 0U)
		return probe_fail("powered", EPROTO);
	error = ioctl(first, CCID_GET_INFO, &info);
	if (error != 0 || (info.flags & CCID_INFO_CLAIMED) == 0U)
		return probe_fail("claimed", error != 0 ? errno : EPROTO);

	/* The second open can neither power the card nor send it APDUs. */
	error = ioctl(second, CCID_POWER_ON, &status);
	if (error == 0 || errno != EBUSY)
		return probe_fail("second-power-on", error == 0 ? EEXIST : errno);
	error = probe_send(second, probe_select_fido, sizeof(probe_select_fido), answer, sizeof(answer), &length);
	if (error != EPERM)
		return probe_fail("second-transmit", error == 0 ? EEXIST : error);
	printf("SMARTCARD claim ok\n");

	/* SELECT of FIDO's applet, and of another. */
	error = probe_send(first, probe_select_fido, sizeof(probe_select_fido), answer, sizeof(answer), &length);
	if (error != 0)
		return probe_fail("select-fido", error);
	if (length != 10U || memcmp(answer, "FIDO_2_0\x90\x00", 10U) != 0)
		return probe_fail("select-fido-answer", EPROTO);
	error = probe_send(first, probe_select_other, sizeof(probe_select_other), answer, sizeof(answer), &length);
	if (error != 0 || length != 2U || answer[0] != 0x6aU || answer[1] != 0x82U)
		return probe_fail("select-other", error != 0 ? error : EPROTO);
	printf("SMARTCARD select ok\n");

	/* An answer of 500 bytes, in parts by GET RESPONSE. */
	error = probe_send(first, probe_long, sizeof(probe_long), answer, sizeof(answer), &length);
	total = 0U;
	while (error == 0 && length >= 2U) {
		memcpy(collected + total, answer, length - 2U);
		total += length - 2U;
		if (answer[length - 2U] != 0x61U)
			break;
		error = probe_send(first, probe_get_response, sizeof(probe_get_response), answer, sizeof(answer), &length);
	}
	if (error != 0)
		return probe_fail("long", error);
	if (total != PROBE_LONG || answer[length - 2U] != 0x90U)
		return probe_fail("long-length", EPROTO);
	for (index = 0U; index < total; index++) {
		if (collected[index] != (uint8_t)index)
			return probe_fail("long-bytes", EPROTO);
	}
	printf("SMARTCARD get-response ok bytes=%u\n", (unsigned)total);

	/* An answer longer than the room. */
	memset(&transmit, 0, sizeof(transmit));
	transmit.command = (uint64_t)(uintptr_t)probe_select_fido;
	transmit.command_size = sizeof(probe_select_fido);
	transmit.response = (uint64_t)(uintptr_t)small;
	transmit.response_capacity = 5U;
	error = ioctl(first, CCID_TRANSMIT, &transmit);
	if (error == 0 || errno != EMSGSIZE)
		return probe_fail("too-long", error == 0 ? EEXIST : errno);
	printf("SMARTCARD too-long ok\n");

	/* The card taken out and put back: two events, and the next APDU answers ENXIO. */
	error = probe_send(first, probe_reinsert, sizeof(probe_reinsert), answer, sizeof(answer), &length);
	if (error != 0)
		return probe_fail("reinsert", error);
	error = probe_event(second, &event);
	if (error != 0 || event.kind != CCID_EVENT_REMOVED)
		return probe_fail("event-removed", error != 0 ? error : EPROTO);
	error = probe_event(second, &event);
	if (error != 0 || event.kind != CCID_EVENT_INSERTED)
		return probe_fail("event-inserted", error != 0 ? error : EPROTO);
	error = probe_send(first, probe_select_fido, sizeof(probe_select_fido), answer, sizeof(answer), &length);
	if (error != ENXIO)
		return probe_fail("after-removal", error == 0 ? EEXIST : error);
	printf("SMARTCARD events ok changes=%u\n", event.changes);

	/* The holder powers the new card, then closes: the card is off and the second open may claim it. */
	error = ioctl(first, CCID_POWER_ON, &status);
	if (error != 0)
		return probe_fail("power-on-again", errno);
	(void)close(first);
	error = ioctl(second, CCID_GET_STATUS, &status);
	if (error != 0 || status.state != CCID_CARD_PRESENT)
		return probe_fail("off-after-close", error != 0 ? errno : EPROTO);
	error = ioctl(second, CCID_POWER_ON, &status);
	if (error != 0)
		return probe_fail("second-claims", errno);
	error = ioctl(second, CCID_POWER_OFF);
	if (error != 0)
		return probe_fail("power-off", errno);
	printf("SMARTCARD close ok\n");

	/* Done. */
	(void)close(second);
	printf("SMARTCARD PASS\n");
	return 0;
}

/* Opens the named slot, or the first (with loopback, the loopback card's); returns the descriptor and the path. */
static int
probe_open(
	const char *named,
	int loopback,
	char *path,
	size_t size)
{
	struct ccid_info info;
	unsigned number;
	int descriptor;
	int error;

	/* A named slot. */
	if (named != NULL) {
		snprintf(path, size, "%s", named);
		descriptor = open(path, O_RDWR);
		return descriptor;
	}

	/* The first, or the first loopback card. */
	for (number = 0U; number < PROBE_NODES; number++) {
		snprintf(path, size, "/dev/smartcard%u", number);
		descriptor = open(path, O_RDWR);
		if (descriptor < 0)
			continue;
		if (!loopback)
			return descriptor;
		error = ioctl(descriptor, CCID_GET_INFO, &info);
		if (error == 0 && strstr(info.name, "Loopback") != NULL)
			return descriptor;
		(void)close(descriptor);
	}

	/* None. */
	errno = ENOENT;
	return -1;
}

/* Sends one APDU and gives the answer with its status word; returns 0 or the errno value. */
static int
probe_send(
	int descriptor,
	const uint8_t *command,
	size_t size,
	uint8_t *answer,
	size_t capacity,
	size_t *length)
{
	struct ccid_transmit transmit;
	int error;

	/* The exchange. */
	memset(&transmit, 0, sizeof(transmit));
	transmit.command = (uint64_t)(uintptr_t)command;
	transmit.command_size = (uint32_t)size;
	transmit.response = (uint64_t)(uintptr_t)answer;
	transmit.response_capacity = (uint32_t)capacity;
	error = ioctl(descriptor, CCID_TRANSMIT, &transmit);
	if (error != 0)
		return errno;

	/* The answer. */
	*length = transmit.response_size;
	return 0;
}

/* Reads one event, waiting at most PROBE_WAIT_MS. */
static int
probe_event(
	int descriptor,
	struct ccid_event *event)
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

	/* One record. */
	count = read(descriptor, event, sizeof(*event));
	if (count < 0)
		return errno;
	if (count != (ssize_t)sizeof(*event))
		return EIO;

	/* Succeeded: the event. */
	return 0;
}

/* Prints a powered card's ATR. */
static void
probe_print_atr(
	const char *what,
	const struct ccid_status *status)
{
	uint32_t index;

	/* The bytes in hex. */
	printf("SMARTCARD %s state=%u bytes=%u value=", what, status->state, status->atr_size);
	for (index = 0U; index < status->atr_size && index < CCID_ATR_MAX; index++)
		printf("%02x", status->atr[index]);
	printf("\n");
}

/* Says which step failed and why; the exit status is 1. */
static int
probe_fail(
	const char *step,
	int error)
{
	/* The line the test reads. */
	printf("SMARTCARD FAIL step=%s error=%d reason=%s\n", step, error, strerror(error));
	return 1;
}
