/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the Raspberry Pi 4 onboard Bluetooth's pure parts
 * (ws215-p003): the H4 reassembly of bt-hci-proto.c (types mixed, cut
 * anywhere, several in one feed, a refused packet kept, every way a header
 * shows the line out of step, the ACL limit changed, a packet loaded
 * whole), and rpi-bt-link.c: the hunt for the anchor (a false header's
 * prefix before a real answer, overlapping type bytes, an answer to
 * another command, a candidate cut short and replayed, a receive error),
 * the PL011's divisor, and the line's reset history and generation (the
 * program's reset does not empty the history, a reset that came first
 * cancels the transport's own).
 */

#include <drivers/generic/bt-hci.h>
#include "drivers/platform/rpi-common/rpi-bt-link.h"

#include <stdio.h>
#include <string.h>
#include <uapi/bluetooth.h>
#include <uapi/errno.h>

/* The most packets one test gathers. */
#define TEST_PACKETS	16U

/* The checks that failed. */
static unsigned failures;

/* The packets the assembler delivered in one test, and whether the next delivery is refused. */
static uint8_t delivered_types[TEST_PACKETS];
static size_t delivered_lengths[TEST_PACKETS];
static uint8_t delivered_bytes[TEST_PACKETS][8U + BT_ACL_DATA_MAX];
static unsigned delivered_count;
static unsigned refuse_next;

/* The assembler under test and its storage. */
static struct bt_hci_assembler assembler;
static uint8_t assembly[BT_ACL_PACKET_MAX];

static void expect(int condition, const char *what);
static int keep(void *context, uint8_t type, const uint8_t *packet, size_t length);
static void start(size_t acl_data_max);
static int feed(const uint8_t *bytes, size_t size, size_t *consumed);
static void test_h4_packets(void);
static void test_h4_cuts(void);
static void test_h4_refused(void);
static void test_h4_out_of_step(void);
static void test_h4_load(void);
static int hunt_bytes(struct drv_rpi4_bt_hunt *hunt, const uint8_t *bytes, size_t size, size_t *length);
static void test_hunt_plain(void);
static void test_hunt_false_prefix(void);
static void test_hunt_overlap(void);
static void test_hunt_other_command(void);
static void test_hunt_status(void);
static void test_hunt_cut_short(void);
static void test_divisor(void);
static void test_history(void);
static void test_link(void);

/*
 * Runs every test; reports how many checks failed.
 */
int
main(void)
{
	/* The UAPI's structure keeps its size (a compile-time check, here once more at run time). */
	expect(sizeof(struct bt_info) == 160U, "struct bt_info is 160 bytes");

	/* The H4 reassembly. */
	test_h4_packets();
	test_h4_cuts();
	test_h4_refused();
	test_h4_out_of_step();
	test_h4_load();

	/* The hunt. */
	test_hunt_plain();
	test_hunt_false_prefix();
	test_hunt_overlap();
	test_hunt_other_command();
	test_hunt_status();
	test_hunt_cut_short();

	/* The divisor, the history and the line. */
	test_divisor();
	test_history();
	test_link();

	/* A failed check fails the whole. */
	if (failures != 0U) {
		printf("rpi4-bt: %u check(s) failed\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("rpi4-bt: all checks passed\n");
	return 0;
}

/* Counts and reports a check that does not hold. */
static void
expect(
	int condition,
	const char *what)
{
	/* A check that holds says nothing. */
	if (condition)
		return;

	/* A failed one is counted and printed. */
	failures++;
	printf("FAIL: %s\n", what);
}

/* Keeps a delivered packet, or refuses it once when refuse_next is set. */
static int
keep(
	void *context,
	uint8_t type,
	const uint8_t *packet,
	size_t length)
{
	(void)context;

	/* The class with no room. */
	if (refuse_next) {
		refuse_next = 0U;
		return ENOSPC;
	}

	/* A test never gathers more than TEST_PACKETS. */
	if (delivered_count == TEST_PACKETS)
		return EIO;

	/* The packet as the class would queue it. */
	delivered_types[delivered_count] = type;
	delivered_lengths[delivered_count] = length;
	memcpy(delivered_bytes[delivered_count], packet, length);
	delivered_count++;

	/* Succeeded: taken. */
	return 0;
}

/* Starts an H4 assembler and an empty delivery record. */
static void
start(
	size_t acl_data_max)
{
	/* Nothing delivered, nothing refused. */
	delivered_count = 0U;
	refuse_next = 0U;

	/* The assembler, empty. */
	bt_hci_assembler_init(&assembler, assembly, sizeof(assembly), BT_HCI_ASSEMBLE_H4, acl_data_max);
}

/* Feeds bytes to the assembler under test. */
static int
feed(
	const uint8_t *bytes,
	size_t size,
	size_t *consumed)
{
	int error;

	/* The bytes, delivered to keep(). */
	error = bt_hci_assembler_feed(&assembler, bytes, size, keep, NULL, consumed);

	/* Reports what the feed returned. */
	return error;
}

/* Feeds an event, an ACL packet, an SCO packet and a vendor event in one feed. */
static void
test_h4_packets(void)
{
	static const uint8_t stream[] = {
		0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00,
		0x02, 0x40, 0x20, 0x03, 0x00, 0xaa, 0xbb, 0xcc,
		0x03, 0x01, 0x00, 0x02, 0x11, 0x22,
		0x04, 0xff, 0x01, 0x99
	};
	size_t consumed;
	int error;

	/* The four packets in one feed. */
	start(BT_ACL_DATA_DEFAULT);
	error = feed(stream, sizeof(stream), &consumed);
	expect(error == 0, "h4: a mixed stream is taken");
	expect(consumed == sizeof(stream), "h4: every byte of the mixed stream is consumed");
	expect(delivered_count == 4U, "h4: four packets");
	expect(delivered_types[0] == BT_PACKET_EVENT && delivered_lengths[0] == 6U, "h4: the event without its type byte");
	expect(delivered_bytes[0][0] == 0x0eU && delivered_bytes[0][5] == 0x00U, "h4: the event's bytes");
	expect(delivered_types[1] == BT_PACKET_ACL && delivered_lengths[1] == 7U, "h4: the ACL packet");
	expect(delivered_bytes[1][6] == 0xccU, "h4: the ACL packet's last byte");
	expect(delivered_types[2] == BT_PACKET_SCO && delivered_lengths[2] == 5U, "h4: the SCO packet is delivered as SCO");
	expect(delivered_types[3] == BT_PACKET_EVENT && delivered_bytes[3][0] == 0xffU, "h4: the vendor event");
}

/* Feeds the same stream a byte at a time and cut at every place. */
static void
test_h4_cuts(void)
{
	static const uint8_t stream[] = {
		0x02, 0x01, 0x00, 0x05, 0x00, 1, 2, 3, 4, 5,
		0x04, 0x13, 0x05, 0x01, 0x40, 0x00, 0x01, 0x00
	};
	size_t consumed;
	size_t cut;
	size_t index;
	int error;
	int good;

	/* A byte at a time. */
	start(BT_ACL_DATA_DEFAULT);
	good = 1;
	for (index = 0U; index < sizeof(stream); index++) {
		error = feed(stream + index, 1U, &consumed);
		if (error != 0 || consumed != 1U)
			good = 0;
	}

	/* Every byte was taken and both packets came. */
	expect(good, "h4: every single byte is taken");
	expect(delivered_count == 2U, "h4: a byte at a time gives the two packets");
	expect(delivered_types[1] == BT_PACKET_EVENT && delivered_lengths[1] == 7U, "h4: the second packet is the event");

	/* Cut in two at every place. */
	good = 1;
	for (cut = 0U; cut <= sizeof(stream); cut++) {
		start(BT_ACL_DATA_DEFAULT);
		error = feed(stream, cut, &consumed);
		if (error != 0)
			good = 0;
		error = feed(stream + cut, sizeof(stream) - cut, &consumed);
		if (error != 0)
			good = 0;
		if (delivered_count != 2U)
			good = 0;
	}

	/* The same two packets wherever the stream was cut. */
	expect(good, "h4: a stream cut anywhere gives the same two packets");
}

/* Refuses a packet: it stays whole, the feed stops after it, and the next feed delivers it first. */
static void
test_h4_refused(void)
{
	static const uint8_t stream[] = {
		0x04, 0x0e, 0x04, 0x01, 0x01, 0x10, 0x00,
		0x04, 0x0f, 0x04, 0x00, 0x01, 0x05, 0x04
	};
	size_t consumed;
	int error;

	/* The first packet refused. */
	start(BT_ACL_DATA_DEFAULT);
	refuse_next = 1U;
	error = feed(stream, sizeof(stream), &consumed);
	expect(error == ENOSPC, "h4: a refused packet stops the feed");
	expect(consumed == 7U, "h4: the feed stops right after the refused packet");
	expect(delivered_count == 0U, "h4: nothing was delivered yet");

	/* The held packet alone, then the rest. */
	error = feed(NULL, 0U, &consumed);
	expect(error == 0 && delivered_count == 1U, "h4: the held packet is delivered by an empty feed");
	error = feed(stream + 7, sizeof(stream) - 7U, &consumed);
	expect(error == 0 && delivered_count == 2U, "h4: the rest follows");
	expect(delivered_bytes[1][0] == 0x0fU, "h4: the second is the Command Status");
}

/* Every header no controller sends is EBADMSG, and the whole feed is consumed. */
static void
test_h4_out_of_step(void)
{
	static const uint8_t command[] = { 0x01, 0x03, 0x0c, 0x00 };
	static const uint8_t junk[] = { 0x00, 0x04 };
	static const uint8_t unknown_event[] = { 0x04, 0x5a, 0x00 };
	static const uint8_t no_event[] = { 0x04, 0x00, 0x00 };
	static const uint8_t acl_handle[] = { 0x02, 0x00, 0x0f, 0x01, 0x00, 0x00 };
	static const uint8_t acl_long[] = { 0x02, 0x01, 0x00, 0x00, 0x04 };
	static const uint8_t sco_handle[] = { 0x03, 0xff, 0x0f, 0x01, 0x00 };
	static const uint8_t acl_small[] = { 0x02, 0x01, 0x00, 0x1c, 0x00 };
	static const uint8_t last_event[] = { 0x04, 0x59, 0x00 };
	size_t consumed;
	int error;

	/* A command from the controller's side. */
	start(BT_ACL_DATA_DEFAULT);
	error = feed(command, sizeof(command), &consumed);
	expect(error == EBADMSG && consumed == sizeof(command), "h4: a command type is out of step");

	/* A byte of no type. */
	start(BT_ACL_DATA_DEFAULT);
	error = feed(junk, sizeof(junk), &consumed);
	expect(error == EBADMSG, "h4: a zero type byte is out of step");

	/* Event codes past the last defined one, and zero. */
	start(BT_ACL_DATA_DEFAULT);
	error = feed(unknown_event, sizeof(unknown_event), &consumed);
	expect(error == EBADMSG, "h4: event code 0x5a is out of step");
	start(BT_ACL_DATA_DEFAULT);
	error = feed(no_event, sizeof(no_event), &consumed);
	expect(error == EBADMSG, "h4: event code 0 is out of step");
	start(BT_ACL_DATA_DEFAULT);
	error = feed(last_event, sizeof(last_event), &consumed);
	expect(error == 0 && delivered_count == 1U, "h4: event code 0x59 (Encryption Change v2) is taken");

	/* An ACL handle past 0x0eff, an ACL packet past the limit. */
	start(BT_ACL_DATA_DEFAULT);
	error = feed(acl_handle, sizeof(acl_handle), &consumed);
	expect(error == EBADMSG, "h4: ACL handle 0x0f00 is out of step");
	start(BT_ACL_DATA_DEFAULT);
	error = feed(acl_long, sizeof(acl_long), &consumed);
	expect(error == EBADMSG, "h4: an ACL packet of 1024 bytes past the 1021 limit is out of step");

	/* The ACL limit the program set: 28 bytes past 27 is out of step, and is taken again at 1021. */
	start(BT_ACL_DATA_MIN);
	error = feed(acl_small, sizeof(acl_small), &consumed);
	expect(error == EBADMSG, "h4: 28 bytes past a limit of 27 is out of step");
	start(BT_ACL_DATA_MIN);
	assembler.acl_data_max = BT_ACL_DATA_DEFAULT;
	error = feed(acl_small, sizeof(acl_small), &consumed);
	expect(error == 0 && consumed == sizeof(acl_small), "h4: the raised limit takes the header");

	/* An SCO handle past 0x0eff. */
	start(BT_ACL_DATA_DEFAULT);
	error = feed(sco_handle, sizeof(sco_handle), &consumed);
	expect(error == EBADMSG, "h4: SCO handle 0x0fff is out of step");
}

/* Loads a packet whole and delivers it with an empty feed; refuses a wrong length and a busy assembler. */
static void
test_h4_load(void)
{
	static const uint8_t answer[] = { 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };
	static const uint8_t partial[] = { 0x04, 0x0e };
	size_t consumed;
	int error;

	/* A whole answer. */
	start(BT_ACL_DATA_DEFAULT);
	error = bt_hci_assembler_load(&assembler, answer, sizeof(answer));
	expect(error == 0, "h4: a whole answer is loaded");
	error = feed(NULL, 0U, &consumed);
	expect(error == 0 && delivered_count == 1U && delivered_lengths[0] == 6U, "h4: the loaded answer is delivered");

	/* Refused once, then delivered. */
	start(BT_ACL_DATA_DEFAULT);
	error = bt_hci_assembler_load(&assembler, answer, sizeof(answer));
	refuse_next = 1U;
	error = feed(NULL, 0U, &consumed);
	expect(error == ENOSPC, "h4: a loaded answer may be refused");
	error = feed(NULL, 0U, &consumed);
	expect(error == 0 && delivered_count == 1U, "h4: and is delivered later");

	/* A length the header does not say, and an assembler gathering already. */
	start(BT_ACL_DATA_DEFAULT);
	error = bt_hci_assembler_load(&assembler, answer, sizeof(answer) - 1U);
	expect(error == EINVAL && assembler.have == 0U, "h4: a short answer is not loaded");
	error = feed(partial, sizeof(partial), &consumed);
	error = bt_hci_assembler_load(&assembler, answer, sizeof(answer));
	expect(error == EBUSY, "h4: a gathering assembler takes no packet");
}

/* Hunts through bytes; reports FOUND as soon as a byte completes the anchor. */
static int
hunt_bytes(
	struct drv_rpi4_bt_hunt *hunt,
	const uint8_t *bytes,
	size_t size,
	size_t *length)
{
	size_t index;
	int found;

	/* Each byte until the anchor. */
	found = DRV_RPI4_BT_HUNT_MORE;
	for (index = 0U; index < size; index++) {
		found = drv_rpi4_bt_hunt_byte(hunt, bytes[index], length);
		if (found == DRV_RPI4_BT_HUNT_FOUND)
			break;
	}

	/* The last verdict. */
	return found;
}

/* Garbage before the answer to HCI_Reset is passed over byte by byte. */
static void
test_hunt_plain(void)
{
	static const uint8_t stream[] = { 0x00, 0xff, 0x55, 0x04, 0x05, 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };
	struct drv_rpi4_bt_hunt hunt;
	size_t length;
	int found;

	/* Garbage, then the answer to HCI_Reset. */
	drv_rpi4_bt_hunt_restart(&hunt);
	drv_rpi4_bt_hunt_expect(&hunt, 0x0c03U);
	found = hunt_bytes(&hunt, stream, sizeof(stream), &length);
	expect(found == DRV_RPI4_BT_HUNT_FOUND, "hunt: the answer after garbage is found");
	expect(length == 7U && hunt.bytes[0] == 0x04U && hunt.bytes[1] == 0x0eU, "hunt: the anchor is the whole Command Complete");
	expect(hunt.dropped == 5U, "hunt: the five bytes before it were passed over");

	/* Without a command sent, any answer will do. */
	drv_rpi4_bt_hunt_restart(&hunt);
	found = hunt_bytes(&hunt, stream + 5, sizeof(stream) - 5U, &length);
	expect(found == DRV_RPI4_BT_HUNT_FOUND && hunt.dropped == 0U, "hunt: with no command known the answer is taken");
}

/* A false header's prefix (04 0E FF waits for 255 bytes) cannot swallow the real answer behind it. */
static void
test_hunt_false_prefix(void)
{
	static const uint8_t stream[] = { 0x04, 0x0e, 0xff, 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };
	struct drv_rpi4_bt_hunt hunt;
	size_t length;
	int found;

	/* With the command known: 0xff's opcode bytes do not match, so the candidate falls at once. */
	drv_rpi4_bt_hunt_restart(&hunt);
	drv_rpi4_bt_hunt_expect(&hunt, 0x0c03U);
	found = hunt_bytes(&hunt, stream, sizeof(stream), &length);
	expect(found == DRV_RPI4_BT_HUNT_FOUND && length == 7U, "hunt: the real answer after 04 0E FF is found");
	expect(hunt.dropped == 3U, "hunt: only the false prefix was passed over");

	/* With no command known the false candidate swallows the bytes until the gap gives it up. */
	drv_rpi4_bt_hunt_restart(&hunt);
	found = hunt_bytes(&hunt, stream, sizeof(stream), &length);
	expect(found == DRV_RPI4_BT_HUNT_MORE, "hunt: an unknown command's false candidate waits");
	found = drv_rpi4_bt_hunt_give_up(&hunt, &length);
	expect(found == DRV_RPI4_BT_HUNT_FOUND && length == 7U, "hunt: the gap replays the rest and finds the answer");
	expect(hunt.bytes[0] == 0x04U && hunt.bytes[6] == 0x00U, "hunt: the replayed anchor's bytes");
}

/* Overlapping type bytes (04 04 0E ...) still find the answer. */
static void
test_hunt_overlap(void)
{
	static const uint8_t stream[] = { 0x04, 0x04, 0x0e, 0x04, 0x01, 0x01, 0x10, 0x00 };
	struct drv_rpi4_bt_hunt hunt;
	size_t length;
	int found;

	/* The overlapping type bytes, with Read Local Version sent. */
	drv_rpi4_bt_hunt_restart(&hunt);
	drv_rpi4_bt_hunt_expect(&hunt, 0x1001U);
	found = hunt_bytes(&hunt, stream, sizeof(stream), &length);
	expect(found == DRV_RPI4_BT_HUNT_FOUND && length == 7U, "hunt: 04 04 0E finds the answer at the second 04");
	expect(hunt.dropped == 1U, "hunt: one type byte was passed over");
}

/* An answer to another command is passed over; the right one after it is the anchor. */
static void
test_hunt_other_command(void)
{
	static const uint8_t stream[] = {
		0x04, 0x0e, 0x04, 0x01, 0x09, 0x10, 0x00,
		0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00
	};
	struct drv_rpi4_bt_hunt hunt;
	size_t length;
	int found;

	/* The answer to another command, then HCI_Reset's. */
	drv_rpi4_bt_hunt_restart(&hunt);
	drv_rpi4_bt_hunt_expect(&hunt, 0x0c03U);
	found = hunt_bytes(&hunt, stream, sizeof(stream), &length);
	expect(found == DRV_RPI4_BT_HUNT_FOUND && length == 7U, "hunt: the answer to the command sent is found");
	expect(hunt.bytes[4] == 0x03U && hunt.bytes[5] == 0x0cU, "hunt: it names HCI_Reset");
	expect(hunt.dropped == 7U, "hunt: the other command's answer was passed over");
}

/* A Command Status (four parameters, the opcode after the status and the credits) is an anchor too; a wrong length is not. */
static void
test_hunt_status(void)
{
	static const uint8_t wrong[] = { 0x04, 0x0f, 0x05 };
	static const uint8_t status[] = { 0x04, 0x0f, 0x04, 0x00, 0x01, 0x05, 0x04 };
	static const uint8_t short_complete[] = { 0x04, 0x0e, 0x03 };
	struct drv_rpi4_bt_hunt hunt;
	size_t length;
	int found;

	/* The two candidates of wrong lengths, then the Command Status. */
	drv_rpi4_bt_hunt_restart(&hunt);
	drv_rpi4_bt_hunt_expect(&hunt, 0x0405U);
	found = hunt_bytes(&hunt, wrong, sizeof(wrong), &length);
	expect(found == DRV_RPI4_BT_HUNT_MORE && hunt.have == 0U, "hunt: a Command Status of five parameters falls");
	found = hunt_bytes(&hunt, short_complete, sizeof(short_complete), &length);
	expect(found == DRV_RPI4_BT_HUNT_MORE && hunt.have == 0U, "hunt: a Command Complete of three parameters falls");
	found = hunt_bytes(&hunt, status, sizeof(status), &length);
	expect(found == DRV_RPI4_BT_HUNT_FOUND && length == 7U, "hunt: the Command Status naming the command is found");
}

/*
 * A candidate cut short (no byte for the gap) or broken by a receive
 * error is given up, and the rest is replayed; bytes after a replayed
 * anchor stay after it.
 */
static void
test_hunt_cut_short(void)
{
	static const uint8_t stream[] = {
		0x04, 0x0e, 0x20, 0x01, 0x03, 0x0c,
		0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00,
		0x04, 0x13
	};
	struct drv_rpi4_bt_hunt hunt;
	size_t length;
	int found;

	/* A Command Complete for HCI_Reset that claims 32 parameters swallows the real one. */
	drv_rpi4_bt_hunt_restart(&hunt);
	drv_rpi4_bt_hunt_expect(&hunt, 0x0c03U);
	found = hunt_bytes(&hunt, stream, sizeof(stream), &length);
	expect(found == DRV_RPI4_BT_HUNT_MORE && hunt.have == sizeof(stream), "hunt: the long candidate waits");

	/* The gap: the replay finds the real answer, and the two bytes after it are kept. */
	found = drv_rpi4_bt_hunt_give_up(&hunt, &length);
	expect(found == DRV_RPI4_BT_HUNT_FOUND && length == 7U, "hunt: the replay finds the real answer");
	expect(hunt.have == 9U && hunt.bytes[7] == 0x04U && hunt.bytes[8] == 0x13U, "hunt: the bytes after the anchor stay after it");

	/* A receive error gives up as the gap does; nothing gathered gives up nothing. */
	drv_rpi4_bt_hunt_restart(&hunt);
	found = hunt_bytes(&hunt, stream, 3U, &length);
	found = drv_rpi4_bt_hunt_give_up(&hunt, &length);
	expect(found == DRV_RPI4_BT_HUNT_MORE && hunt.have == 0U, "hunt: an error drops the broken candidate");
	found = drv_rpi4_bt_hunt_give_up(&hunt, &length);
	expect(found == DRV_RPI4_BT_HUNT_MORE && hunt.have == 0U, "hunt: giving up nothing is harmless");
}

/* The divisor for the 48 MHz clock, the limits, and the refusals. */
static void
test_divisor(void)
{
	uint32_t integer;
	uint32_t fraction;
	int error;

	/* 115200: 26 and 3/64, the serial console's (uart.c). */
	error = drv_rpi4_bt_divisor(48000000U, 115200U, &integer, &fraction);
	expect(error == 0 && integer == 26U && fraction == 3U, "divisor: 115200 is 26 + 3/64");

	/* 921600, 2 Mbaud, 3 Mbaud (the PL011's limit, exactly 1). */
	error = drv_rpi4_bt_divisor(48000000U, 921600U, &integer, &fraction);
	expect(error == 0 && integer == 3U && fraction == 16U, "divisor: 921600 is 3 + 16/64");
	error = drv_rpi4_bt_divisor(48000000U, 2000000U, &integer, &fraction);
	expect(error == 0 && integer == 1U && fraction == 32U, "divisor: 2 Mbaud is 1 + 32/64");
	error = drv_rpi4_bt_divisor(48000000U, 3000000U, &integer, &fraction);
	expect(error == 0 && integer == 1U && fraction == 0U, "divisor: 3 Mbaud is exactly 1");

	/* Past a sixteenth of the clock, nothing, no clock, and a divisor past 16 bits. */
	error = drv_rpi4_bt_divisor(48000000U, 3000001U, &integer, &fraction);
	expect(error == EINVAL, "divisor: past 3 Mbaud is refused");
	error = drv_rpi4_bt_divisor(48000000U, 0U, &integer, &fraction);
	expect(error == EINVAL, "divisor: 0 baud is refused");
	error = drv_rpi4_bt_divisor(0U, 115200U, &integer, &fraction);
	expect(error == EINVAL, "divisor: no clock is refused");
	error = drv_rpi4_bt_divisor(48000000U, 10U, &integer, &fraction);
	expect(error == EINVAL, "divisor: 10 baud needs more than 16 bits and is refused");

	/* The speed limit: the tree's max-speed, but no more than the PL011's. */
	expect(drv_rpi4_bt_speed_limit(48000000U, 3000000U) == 3000000U, "limit: the tree's 3 Mbaud");
	expect(drv_rpi4_bt_speed_limit(48000000U, 0U) == 3000000U, "limit: no max-speed is the PL011's");
	expect(drv_rpi4_bt_speed_limit(48000000U, 4000000U) == 3000000U, "limit: a max-speed past the PL011's is cut");
	expect(drv_rpi4_bt_speed_limit(24000000U, 3000000U) == 1500000U, "limit: a slower clock lowers it");
	expect(drv_rpi4_bt_speed_limit(48000000U, 921600U) == 921600U, "limit: a lower max-speed is kept");
}

/* Three resets a minute are admitted, the fourth is not; old ones fall out by time. */
static void
test_history(void)
{
	struct drv_rpi4_bt_history history;
	int admitted;

	/* Three resets within the first seconds. */
	drv_rpi4_bt_history_init(&history);
	admitted = drv_rpi4_bt_history_admit(&history, 1000U);
	expect(admitted == 1, "history: the first reset");
	admitted = drv_rpi4_bt_history_admit(&history, 2000U);
	expect(admitted == 1, "history: the second reset");
	admitted = drv_rpi4_bt_history_admit(&history, 3000U);
	expect(admitted == 1, "history: the third reset");
	admitted = drv_rpi4_bt_history_admit(&history, 4000U);
	expect(admitted == 0, "history: the fourth within the minute is refused");
	admitted = drv_rpi4_bt_history_admit(&history, 60999U);
	expect(admitted == 0, "history: still refused while three are within the minute");

	/* The first falls out at 61000, so one more fits; then the next ones fall out in turn. */
	admitted = drv_rpi4_bt_history_admit(&history, 61000U);
	expect(admitted == 1, "history: one fits once the first is a minute old");
	admitted = drv_rpi4_bt_history_admit(&history, 61500U);
	expect(admitted == 0, "history: but not two");
	admitted = drv_rpi4_bt_history_admit(&history, 200000U);
	expect(admitted == 1, "history: long after, the count starts again");
}

/*
 * The line: the transitions, the anchor, the transport's own resets up to
 * STOPPED, the program's reset that leaves STOPPED but keeps the history,
 * and a reset that came first cancelling the transport's own.
 */
static void
test_link(void)
{
	struct drv_rpi_bt_link link;
	uint32_t observed;
	int decision;

	/* Never powered: nothing to reset. */
	drv_rpi_bt_link_init(&link);
	decision = drv_rpi_bt_link_self_reset(&link, link.generation, 0U);
	expect(decision == DRV_RPI4_BT_SELF_RESET_CANCEL, "link: an unpowered line is not reset");

	/* The power-up: quiet, then hunting, then in step on the anchor. */
	drv_rpi_bt_link_transition_begin(&link);
	expect(link.mode == DRV_RPI4_BT_MODE_QUIET && link.generation == 1U, "link: a transition is quiet at a new generation");
	drv_rpi_bt_link_anchor(&link);
	expect(link.mode == DRV_RPI4_BT_MODE_QUIET, "link: a quiet line takes no anchor");
	drv_rpi_bt_link_transition_end(&link);
	expect(link.mode == DRV_RPI4_BT_MODE_HUNT, "link: after the quiet time it hunts");
	drv_rpi_bt_link_anchor(&link);
	expect(link.mode == DRV_RPI4_BT_MODE_SYNC, "link: the anchor puts it in step");

	/* Three resets of its own are made, each a transition. */
	decision = drv_rpi_bt_link_self_reset(&link, link.generation, 1000U);
	expect(decision == DRV_RPI4_BT_SELF_RESET_GO, "link: the first own reset");
	drv_rpi_bt_link_transition_begin(&link);
	drv_rpi_bt_link_transition_end(&link);
	decision = drv_rpi_bt_link_self_reset(&link, link.generation, 2000U);
	expect(decision == DRV_RPI4_BT_SELF_RESET_GO, "link: the second own reset");
	drv_rpi_bt_link_transition_begin(&link);
	drv_rpi_bt_link_transition_end(&link);
	decision = drv_rpi_bt_link_self_reset(&link, link.generation, 3000U);
	expect(decision == DRV_RPI4_BT_SELF_RESET_GO, "link: the third own reset");
	drv_rpi_bt_link_transition_begin(&link);
	drv_rpi_bt_link_transition_end(&link);

	/* The fourth within the minute stops the line. */
	decision = drv_rpi_bt_link_self_reset(&link, link.generation, 4000U);
	expect(decision == DRV_RPI4_BT_SELF_RESET_STOP, "link: the fourth own reset stops");
	observed = link.generation;
	drv_rpi_bt_link_stop(&link);
	expect(link.mode == DRV_RPI4_BT_MODE_STOPPED && link.generation != observed, "link: stopped at a new generation");
	decision = drv_rpi_bt_link_self_reset(&link, link.generation, 4100U);
	expect(decision == DRV_RPI4_BT_SELF_RESET_CANCEL, "link: a stopped line is not reset by itself");
	drv_rpi_bt_link_transition_end(&link);
	expect(link.mode == DRV_RPI4_BT_MODE_STOPPED, "link: a stop is not undone by the end of a transition");

	/* The program's reset starts the line again, but one more own reset within the window stops it again. */
	drv_rpi_bt_link_transition_begin(&link);
	drv_rpi_bt_link_transition_end(&link);
	expect(link.mode == DRV_RPI4_BT_MODE_HUNT, "link: the program's reset leaves STOPPED");
	decision = drv_rpi_bt_link_self_reset(&link, link.generation, 5000U);
	expect(decision == DRV_RPI4_BT_SELF_RESET_STOP, "link: one own reset in the window after the program's reset stops again");

	/* Out of the window, the count starts again. */
	drv_rpi_bt_link_stop(&link);
	drv_rpi_bt_link_transition_begin(&link);
	drv_rpi_bt_link_transition_end(&link);
	decision = drv_rpi_bt_link_self_reset(&link, link.generation, 64000U);
	expect(decision == DRV_RPI4_BT_SELF_RESET_GO, "link: a minute after the first resets one is made again");

	/*
	 * The race of design 中-D: the worker sees generation g and wants a
	 * reset; the program's reset makes its transition first; the worker's
	 * then finds another generation and is cancelled, without counting.
	 */
	drv_rpi_bt_link_transition_begin(&link);
	drv_rpi_bt_link_transition_end(&link);
	observed = link.generation;
	drv_rpi_bt_link_transition_begin(&link);
	drv_rpi_bt_link_transition_end(&link);
	decision = drv_rpi_bt_link_self_reset(&link, observed, 64100U);
	expect(decision == DRV_RPI4_BT_SELF_RESET_CANCEL, "link: a reset that came first cancels the worker's");
	decision = drv_rpi_bt_link_self_reset(&link, link.generation, 64200U);
	expect(decision == DRV_RPI4_BT_SELF_RESET_GO, "link: the cancelled one was not counted (the second of the window)");
	decision = drv_rpi_bt_link_self_reset(&link, link.generation, 64300U);
	expect(decision == DRV_RPI4_BT_SELF_RESET_GO, "link: the third of the window");
	decision = drv_rpi_bt_link_self_reset(&link, link.generation, 64400U);
	expect(decision == DRV_RPI4_BT_SELF_RESET_STOP, "link: the fourth of the window stops");
}
