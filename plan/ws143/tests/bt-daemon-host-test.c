/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of bluetoothd (ws143-p003, plan/ws143/phase003/phase.md
 * section 7): userland/base/bluetoothd/hci.c, intel.c and session.c built
 * with the host's compiler.
 *
 *   hci      commands and events, the supported commands' bits (checked
 *            against a field built by hand from the Core's table, not the
 *            daemon's constants), every scan event and its broken lengths,
 *            the names' rules, the escape of names from the air
 *   intel    Read Version's TLVs (broken, twice, missing), the file name,
 *            the loadable rules, the fragments of synthetic .sfi files, the
 *            .ddc records
 *   session  a scripted controller on a socket pair: a plain controller's
 *            start and scan, Intel's operational, bootloader (load, boot,
 *            DDC, the bootloader path's ioctls), a missing file, a load not
 *            allowed, a Secure Send refused, a timeout, a stray answer, a
 *            node that goes, a hardware error, a bootloader path left on
 *   fuzz     random and mutated bytes into every parser (fixed seed)
 *   real     with the intelbt-firmware package's verified cache (the
 *            second argument; ws143-p003 i02): each real .sfi file's plan
 *            under both engines, and a scripted bootloader loaded with it
 *            and its .ddc, every fragment checked against the plan
 *
 *   plan/ws143/tests/bt-daemon-host-test.sh
 */

#include "userland/base/bluetoothd/hci.h"
#include "userland/base/bluetoothd/intel.h"
#include "userland/base/bluetoothd/session.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* The fuzz's rounds. */
#define TEST_FUZZ_ROUNDS	20000U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/*
 * The scripted controller of one session test: what it plays, what it
 * saw, and its end of the socket pair.  The test's thread and the
 * controller's thread share it; the controller writes only its counters,
 * which the test reads after joining it.
 */
struct fake {
	int descriptor;
	int intel;
	int bootloader;
	int answer_firmware;
	int silent_opcode;
	int stray_before_reset;
	int close_at_opcode;
	int secure_send_status;
	int hardware_error;
	const uint8_t *file;
	size_t file_length;
	size_t file_offset;
	int custom_version;
	uint32_t cnvi_top;
	uint32_t cnvr_top;
	uint32_t cnvi_bt;
	uint8_t sbe_type;
	const struct btd_intel_fragment *plan;
	size_t plan_count;
	unsigned fragments;
	unsigned fragment_mismatch;
	unsigned ddc_records;
	unsigned resets_sent;
	uint16_t opcodes[256];
	unsigned opcode_count;
	struct bt_info info;
	unsigned long ioctls[32];
	uint32_t ioctl_values[32];
	unsigned ioctl_count;
};

static void expect(int condition, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void test_hci(void);
static void test_hci_scan(void);
static void test_escape(void);
static void test_intel(void);
static void test_intel_plan(void);
static void test_session_plain(void);
static void test_session_intel(void);
static void test_session_failures(void);
static void test_fuzz(void);
static void test_real_firmware(const char *folder);
static void test_real_plan(const char *name, const uint8_t *file, size_t length, uint8_t sbe_type, struct btd_intel_plan *plan);
static void test_real_load(const char *folder, const char *name, uint32_t cnvi_top, uint32_t cnvr_top);
static uint8_t *read_whole(const char *path, size_t *length);
static void put_le32(uint8_t *at, uint32_t value);
static size_t sfi_command(uint8_t *at, uint16_t opcode, size_t parameters, uint8_t fill);
static size_t make_sfi(uint8_t *file, size_t size, int ecdsa);
static void test_ddc(void);
static void fake_start(struct fake *fake, struct btd_session *session, pthread_t *thread);
static void fake_stop(struct fake *fake, struct btd_session *session, pthread_t thread);
static void *fake_run(void *argument);
static void fake_send(struct fake *fake, const uint8_t *event, size_t length);
static void fake_complete(struct fake *fake, uint16_t opcode, const uint8_t *returned, size_t count);
static void fake_vendor(struct fake *fake, uint8_t code);
static int fake_control(void *context, unsigned long request, void *argument);
static int fake_saw(const struct fake *fake, uint16_t opcode);
static int fake_planned(const struct fake *fake, uint8_t type, const uint8_t *data, size_t length);

/* The synthetic firmware's folder (made under the build folder by the script). */
static const char *firmware_folder;

/*
 * The intelbt-firmware package's verified cache, read only; NULL when the
 * script found none, and the real files' tests are passed over.
 */
static const char *real_folder;

/*
 * Runs every part; the exit status says whether every check held.
 */
int
main(
	int argc,
	char **argv)
{
	/* The firmware folder the script made, and the package's cache when given. */
	firmware_folder = "build/tmp";
	real_folder = NULL;
	if (argc > 1)
		firmware_folder = argv[1];
	if (argc > 2)
		real_folder = argv[2];

	/* The parts. */
	test_hci();
	test_hci_scan();
	test_escape();
	test_intel();
	test_intel_plan();
	test_ddc();
	test_session_plain();
	test_session_intel();
	test_session_failures();
	test_fuzz();

	/* The real files, when the package's cache is at hand. */
	if (real_folder != NULL) {
		test_real_firmware(real_folder);
	} else {
		printf("bt-daemon-host-test: the real firmware files skipped (no cache given)\n");
	}

	/* The verdict. */
	if (failures != 0U) {
		printf("bt-daemon-host-test: FAIL (%u of %u checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("bt-daemon-host-test: PASS (%u checks)\n", checks);
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

/* Commands, events, answers and the supported commands' bits. */
static void
test_hci(void)
{
	static const uint8_t parameters[3] = { 0x33U, 0x8bU, 0x9eU };
	static const uint8_t complete[] = { 0x04U, 0x0eU, 0x06U, 0x01U, 0x09U, 0x10U, 0x00U, 0xaaU, 0xbbU };
	static const uint8_t status[] = { 0x04U, 0x0fU, 0x04U, 0x0cU, 0x01U, 0x01U, 0x04U };
	static const uint8_t short_status[] = { 0x04U, 0x0fU, 0x03U, 0x00U, 0x01U, 0x01U };
	static const uint8_t wrong_length[] = { 0x04U, 0x0eU, 0x05U, 0x01U };
	struct btd_event event;
	struct btd_answer answer;
	uint8_t commands[64];
	uint8_t packet[300];
	size_t length;
	int error;
	int taken;

	/* A command: type, opcode least significant first, length, parameters. */
	length = btd_hci_command(packet, sizeof(packet), 0x0401U, parameters, sizeof(parameters));
	expect(length == 7U && packet[0] == 0x01U && packet[1] == 0x01U && packet[2] == 0x04U && packet[3] == 3U && packet[6] == 0x9eU,
	       "hci: Inquiry's packet");
	length = btd_hci_command(packet, 5U, 0x0401U, parameters, sizeof(parameters));
	expect(length == 0U, "hci: a packet too small is refused");

	/* A Command Complete: the opcode, the window, the status and the return parameters after it. */
	error = btd_hci_event(complete, sizeof(complete), &event);
	taken = btd_hci_answer(&event, &answer);
	expect(error == 0 && taken == 1 && answer.opcode == 0x1009U && answer.window == 1U && answer.status == 0U &&
	       answer.returned_length == 3U && answer.returned[1] == 0xaaU,
	       "hci: Command Complete");

	/* A Command Status. */
	error = btd_hci_event(status, sizeof(status), &event);
	taken = btd_hci_answer(&event, &answer);
	expect(error == 0 && taken == 1 && answer.is_status && answer.status == 0x0cU && answer.opcode == 0x0401U,
	       "hci: Command Status");

	/* A Command Status too short, and an event whose length lies. */
	error = btd_hci_event(short_status, sizeof(short_status), &event);
	taken = btd_hci_answer(&event, &answer);
	expect(error == 0 && taken == -1, "hci: a short Command Status is malformed");
	error = btd_hci_event(wrong_length, sizeof(wrong_length), &event);
	expect(error == EBADMSG, "hci: an event whose length lies is refused");
	error = btd_hci_event(packet, 2U, &event);
	expect(error == EBADMSG, "hci: two bytes are no event");

	/*
	 * The supported commands' bits, built by hand from the Core's table
	 * (Vol 4 Part E §6.27): Inquiry is octet 0 bit 0, LE Set Event Mask
	 * octet 25 bit 0, LE Set Scan Parameters octet 26 bit 2, LE Set Scan
	 * Enable octet 26 bit 3, LE Read Local P-256 Public Key octet 34 bit
	 * 1, LE Generate DHKey octet 34 bit 2.  Octet 25 bits 5 and 6 (LE Set
	 * Advertising Parameters, LE Read Advertising Physical Channel Tx
	 * Power) are set to show they are not taken for the scan.
	 */
	memset(commands, 0, sizeof(commands));
	commands[0] = 0x01U;
	commands[25] = 0x61U;
	commands[26] = 0x0cU;
	commands[34] = 0x06U;
	expect(btd_hci_supported(commands, sizeof(commands), 0U, 0U) == 1, "hci: Inquiry's bit");
	expect(btd_hci_supported(commands, sizeof(commands), 26U, 2U) == 1, "hci: LE Set Scan Parameters' bit");
	expect(btd_hci_supported(commands, sizeof(commands), 26U, 3U) == 1, "hci: LE Set Scan Enable's bit");
	expect(btd_hci_supported(commands, sizeof(commands), 34U, 1U) == 1, "hci: P-256's bit");
	expect(btd_hci_supported(commands, sizeof(commands), 34U, 2U) == 1, "hci: DHKey's bit");
	expect(btd_hci_supported(commands, sizeof(commands), 64U, 0U) == 0, "hci: an octet past the field is no");
	commands[26] = 0x00U;
	expect(btd_hci_supported(commands, sizeof(commands), 26U, 2U) == 0, "hci: the scan's bits are not octet 25's");
}

/* Every scan event, its fields, the names' rules and its broken lengths. */
static void
test_hci_scan(void)
{
	struct btd_devices devices;
	struct btd_event event;
	uint8_t parameters[256];
	uint8_t data[40];
	int taken;

	/* Inquiry Result: two responses of 14 bytes, classes at their place. */
	btd_devices_clear(&devices);
	memset(parameters, 0, sizeof(parameters));
	parameters[0] = 2U;
	parameters[1] = 0x01U;
	parameters[10] = 0x40U;
	parameters[11] = 0x25U;
	parameters[15] = 0x02U;
	parameters[24] = 0x80U;
	parameters[25] = 0x25U;
	event.code = BTD_EVENT_INQUIRY_RESULT;
	event.parameters = parameters;
	event.length = 1U + 2U * 14U;
	taken = btd_devices_take(&devices, &event);
	expect(taken == 1 && devices.count == 2U && devices.entries[0].class_of_device == 0x2540U &&
	       devices.entries[1].class_of_device == 0x2580U && !devices.entries[0].has_rssi,
	       "scan: Inquiry Result's two responses");

	/* The same responses again are the same devices; one byte short is malformed. */
	taken = btd_devices_take(&devices, &event);
	expect(taken == 1 && devices.count == 2U, "scan: a device seen twice is one");
	event.length = 1U + 2U * 14U - 1U;
	taken = btd_devices_take(&devices, &event);
	expect(taken == -1, "scan: Num_Responses past the parameters is malformed");

	/* Inquiry Result with RSSI: the class one byte earlier, the RSSI last. */
	btd_devices_clear(&devices);
	memset(parameters, 0, sizeof(parameters));
	parameters[0] = 1U;
	parameters[1] = 0x07U;
	parameters[9] = 0x0cU;
	parameters[10] = 0x01U;
	parameters[14] = (uint8_t)(int8_t)-61;
	event.code = BTD_EVENT_INQUIRY_RSSI;
	event.parameters = parameters;
	event.length = 15U;
	taken = btd_devices_take(&devices, &event);
	expect(taken == 1 && devices.count == 1U && devices.entries[0].class_of_device == 0x010cU &&
	       devices.entries[0].has_rssi && devices.entries[0].rssi == -61,
	       "scan: Inquiry Result with RSSI");

	/* Extended Inquiry Result: a short name, then a complete one, then a short one that does not replace it. */
	btd_devices_clear(&devices);
	memset(parameters, 0, sizeof(parameters));
	parameters[0] = 1U;
	parameters[1] = 0x09U;
	parameters[9] = 0x40U;
	parameters[10] = 0x25U;
	parameters[14] = (uint8_t)(int8_t)-40;
	memcpy(parameters + 15, "\x04\x08" "Abc" "\x06\x09" "Abcde" "\x04\x08" "Xyz", 17U);
	event.code = BTD_EVENT_EXTENDED_INQUIRY;
	event.parameters = parameters;
	event.length = 255U;
	taken = btd_devices_take(&devices, &event);
	expect(taken == 1 && devices.count == 1U && strcmp(devices.entries[0].name, "Abcde") == 0 &&
	       devices.entries[0].name_complete && devices.entries[0].rssi == -40 && devices.entries[0].class_of_device == 0x2540U,
	       "scan: Extended Inquiry Result's complete name wins (%s)", devices.entries[0].name);
	event.length = 14U;
	taken = btd_devices_take(&devices, &event);
	expect(taken == -1, "scan: an Extended Inquiry Result without its head is malformed");

	/* The data: a name with a NUL in it, an empty name, a structure past the end, a length of 0. */
	btd_devices_clear(&devices);
	memcpy(data, "\x05\x09" "Ab\0d", 6U);
	btd_device_data(&devices.entries[0], data, 6U);
	expect(strcmp(devices.entries[0].name, "Ab") == 0, "data: a name ends at a NUL");
	memcpy(data, "\x01\x09", 2U);
	btd_device_data(&devices.entries[0], data, 2U);
	expect(devices.entries[0].name[0] == '\0', "data: a type alone is an empty name");
	memcpy(data, "\x09\x08" "Ab", 4U);
	devices.entries[1].name[0] = '\0';
	btd_device_data(&devices.entries[1], data, 4U);
	expect(devices.entries[1].name[0] == '\0', "data: a structure past the end is not read");
	memcpy(data, "\x00\x09" "Ab", 4U);
	btd_device_data(&devices.entries[1], data, 4U);
	expect(devices.entries[1].name[0] == '\0', "data: a length of 0 ends the data");

	/* LE Advertising Report: two reports, public and random, a name and an appearance; RSSI 127 is none. */
	btd_devices_clear(&devices);
	memset(parameters, 0, sizeof(parameters));
	parameters[0] = BTD_LE_ADVERTISING_REPORT;
	parameters[1] = 2U;
	parameters[2] = 0x00U;
	parameters[3] = 0x00U;
	parameters[4] = 0x03U;
	parameters[10] = 9U;
	memcpy(parameters + 11, "\x04\x09" "Mou" "\x03\x19\xc2\x03", 9U);
	parameters[20] = (uint8_t)(int8_t)-50;
	parameters[21] = 0x00U;
	parameters[22] = 0x01U;
	parameters[23] = 0x04U;
	parameters[29] = 0U;
	parameters[30] = 127U;
	event.code = BTD_EVENT_LE_META;
	event.parameters = parameters;
	event.length = 31U;
	taken = btd_devices_take(&devices, &event);
	expect(taken == 1 && devices.count == 2U && devices.entries[0].type == BTD_ADDRESS_LE_PUBLIC &&
	       strcmp(devices.entries[0].name, "Mou") == 0 && devices.entries[0].appearance == 0x03c2U &&
	       devices.entries[0].rssi == -50 && devices.entries[1].type == BTD_ADDRESS_LE_RANDOM && !devices.entries[1].has_rssi,
	       "scan: LE Advertising Report's two reports");

	/* A report's RSSI missing, its data past the end, Num_Reports 0, another subevent. */
	event.length = 30U;
	taken = btd_devices_take(&devices, &event);
	expect(taken == -1, "scan: a report without its RSSI is malformed");
	parameters[10] = 200U;
	event.length = 31U;
	taken = btd_devices_take(&devices, &event);
	expect(taken == -1, "scan: a report's data past the end is malformed");
	parameters[1] = 0U;
	taken = btd_devices_take(&devices, &event);
	expect(taken == 1, "scan: no reports is an empty report");
	parameters[0] = 0x01U;
	taken = btd_devices_take(&devices, &event);
	expect(taken == 0, "scan: another LE subevent is not a report");

	/* A full table counts what it drops. */
	btd_devices_clear(&devices);
	memset(parameters, 0, sizeof(parameters));
	parameters[0] = 1U;
	event.code = BTD_EVENT_INQUIRY_RSSI;
	event.parameters = parameters;
	event.length = 15U;
	for (taken = 0; taken < (int)BTD_DEVICES_MAX + 3; taken++) {
		parameters[1] = (uint8_t)taken;
		parameters[2] = (uint8_t)(taken >> 8);
		(void)btd_devices_take(&devices, &event);
	}

	/* The table's room and the drops. */
	expect(devices.count == BTD_DEVICES_MAX && devices.dropped == 3U, "scan: the table holds %u and drops the rest", BTD_DEVICES_MAX);
}

/* The escape of names for the socket's lines. */
static void
test_escape(void)
{
	char output[64];
	char address[24];
	static const uint8_t address_bytes[6] = { 0x55U, 0x44U, 0x33U, 0x22U, 0x11U, 0x00U };
	int error;

	/* Printable ASCII and UTF-8 stay; the quote and the backslash are escaped. */
	error = btd_escape("Kei \xe3\x82\xad\xe3\x83\xbc \"q\" \\", output, sizeof(output));
	expect(error == 0 && strcmp(output, "Kei \xe3\x82\xad\xe3\x83\xbc \\x22q\\x22 \\x5c") == 0, "escape: UTF-8 stays (%s)", output);

	/* Control bytes, DEL, a C1 control, a lone continuation, an overlong form, a surrogate, a cut sequence. */
	error = btd_escape("\x1b[\x7f\xc2\x9b\x80\xc0\xaf\xed\xa0\x80\xe3\x82", output, sizeof(output));
	expect(error == 0 && strcmp(output, "\\x1b[\\x7f\\xc2\\x9b\\x80\\xc0\\xaf\\xed\\xa0\\x80\\xe3\\x82") == 0,
	       "escape: what could act on a terminal is escaped (%s)", output);

	/* Too long. */
	error = btd_escape("\x01\x02\x03\x04", output, 8U);
	expect(error == ENAMETOOLONG && output[0] == '\0', "escape: a text that does not fit is refused");

	/* An address as people read it. */
	btd_format_address(address_bytes, address, sizeof(address));
	expect(strcmp(address, "00:11:22:33:44:55") == 0, "address: most significant byte first (%s)", address);
}

/* Read Version's TLVs, the file name and the loadable rules. */
static void
test_intel(void)
{
	struct btd_intel_version version;
	const char *reason;
	char path[128];
	uint8_t answer[64];
	size_t length;
	int error;

	/* A bootloader's answer: CNVi and CNVR top, CNVi BT (variant 0x17), image 1, limited CCE 0, SBE 1, the address. */
	length = 0U;
	answer[length++] = 0x00U;
	memcpy(answer + length, "\x10\x04\x67\x45\x23\x01", 6U);
	length += 6U;
	memcpy(answer + length, "\x11\x04\x00\x0a\x40\x00", 6U);
	length += 6U;
	memcpy(answer + length, "\x12\x04\x00\x00\x17\x00", 6U);
	length += 6U;
	memcpy(answer + length, "\x1c\x01\x01", 3U);
	length += 3U;
	memcpy(answer + length, "\x2e\x01\x00", 3U);
	length += 3U;
	memcpy(answer + length, "\x2f\x01\x01", 3U);
	length += 3U;
	memcpy(answer + length, "\x99\x02\xaa\xbb", 4U);
	length += 4U;
	error = btd_intel_version_parse(answer, length, &version);
	expect(error == 0 && version.image_type == 1U && version.sbe_type == 1U && btd_intel_variant(&version) == 0x17U,
	       "intel: a bootloader's TLV answer (unknown types passed over)");

	/*
	 * The file name: 0x01234567 packs to 0x7156 (0x01 << 8, 0x7 << 12,
	 * 0x56), 0x00400a00 to 0x00a0 (0, 0, 0xa0).
	 */
	error = btd_intel_file_name(&version, "/lib/firmware/intel", "sfi", path, sizeof(path));
	expect(error == 0 && strcmp(path, "/lib/firmware/intel/ibt-7156-00a0.sfi") == 0, "intel: the file's name (%s)", path);
	expect(btd_intel_pack_id(0x0f00000fU) == 0xff00U && btd_intel_pack_id(0x00000ff0U) == 0x00ffU, "intel: the packing's bits");

	/* Loadable as it is; not with limited CCE, another engine, variant 0x15, or RSA-only variant with ECDSA. */
	error = btd_intel_loadable(&version, &reason);
	expect(error == 0, "intel: variant 0x17 with ECDSA is loadable");
	version.limited_cce = 1U;
	error = btd_intel_loadable(&version, &reason);
	expect(error == ENOTSUP && strcmp(reason, "limited-cce") == 0, "intel: limited CCE is not loadable");
	version.limited_cce = 0U;
	version.sbe_type = 2U;
	error = btd_intel_loadable(&version, &reason);
	expect(error == ENOTSUP, "intel: an engine past ECDSA is not loadable");
	version.sbe_type = 1U;
	version.cnvi_bt = 0x00150000U;
	error = btd_intel_loadable(&version, &reason);
	expect(error == ENOTSUP && strcmp(reason, "hardware-variant") == 0, "intel: variant 0x15 is not loadable");
	version.cnvi_bt = 0x00140000U;
	error = btd_intel_loadable(&version, &reason);
	expect(error == ENOTSUP, "intel: variant 0x14 with ECDSA is not loadable");
	version.have_sbe_type = 0;
	error = btd_intel_loadable(&version, &reason);
	expect(error == EBADMSG, "intel: a bootloader without its SBE type is not loadable");

	/* Broken answers: a status, a field twice, no image type, a length that lies, a known field of the wrong length. */
	answer[0] = 0x01U;
	error = btd_intel_version_parse(answer, length, &version);
	expect(error == EIO, "intel: a failed Read Version");
	answer[0] = 0x00U;
	memcpy(answer + length, "\x1c\x01\x03", 3U);
	error = btd_intel_version_parse(answer, length + 3U, &version);
	expect(error == EBADMSG, "intel: the image type twice");
	error = btd_intel_version_parse(answer, 7U, &version);
	expect(error == EBADMSG, "intel: no image type");
	memcpy(answer, "\x00\x1c\x05\x01", 4U);
	error = btd_intel_version_parse(answer, 4U, &version);
	expect(error == EBADMSG, "intel: a length past the answer");
	memcpy(answer, "\x00\x10\x02\x01\x02\x1c\x01\x01", 8U);
	error = btd_intel_version_parse(answer, 8U, &version);
	expect(error == EBADMSG, "intel: CNVi top of two bytes");
}

/* Writes a command frame of a synthetic .sfi file; returns the bytes written. */
static size_t
sfi_command(
	uint8_t *at,
	uint16_t opcode,
	size_t parameters,
	uint8_t fill)
{
	/* The frame and its parameters. */
	at[0] = (uint8_t)(opcode & 0xffU);
	at[1] = (uint8_t)(opcode >> 8);
	at[2] = (uint8_t)parameters;
	memset(at + 3, fill, parameters);
	return 3U + parameters;
}

/*
 * Makes a synthetic .sfi file: the RSA header (CSS version 0x00010000 at
 * 8), with ecdsa the ECDSA header (0x06 at 644, version 0x00020000 at 652),
 * then commands: 1 parameter, 250, none, the boot parameter 0x11223344, 1,
 * none, 3.  Each header byte is its offset's low byte, so a fragment's data
 * shows where it came from.  Returns the file's length.
 */
static size_t
make_sfi(
	uint8_t *file,
	size_t size,
	int ecdsa)
{
	size_t length;
	size_t index;

	/* The headers. */
	length = 644U;
	if (ecdsa)
		length = 964U;
	if (size < length + 600U)
		return 0U;
	for (index = 0U; index < length; index++)
		file[index] = (uint8_t)index;
	memcpy(file + 8, "\x00\x00\x01\x00", 4U);
	if (ecdsa) {
		file[644] = 0x06U;
		memcpy(file + 652, "\x00\x00\x02\x00", 4U);
	}

	/* The commands. */
	length += sfi_command(file + length, 0xfc8eU, 1U, 0xa1U);
	length += sfi_command(file + length, 0xfc8eU, 250U, 0xa2U);
	length += sfi_command(file + length, 0xfc8eU, 0U, 0xa3U);
	length += sfi_command(file + length, 0xfc0eU, 4U, 0x00U);
	memcpy(file + length - 4U, "\x44\x33\x22\x11", 4U);
	length += sfi_command(file + length, 0xfc8eU, 1U, 0xa4U);
	length += sfi_command(file + length, 0xfc8eU, 0U, 0xa5U);
	length += sfi_command(file + length, 0xfc8eU, 3U, 0xa6U);

	/* Succeeded: the file's length. */
	return length;
}

/*
 * The fragments of synthetic files.  For the commands above (frames of 4,
 * 253, 3, 7, 4, 3 and 6 bytes) the rule gives: 4 at once (a multiple of
 * 4); 253 waits, 252 go, 1 waits; +3 = 4 go; +7 = 7 waits; +4 = 11 waits;
 * +3 = 14 waits; +6 = 20 go.  So four fragments of the commands: 4, 252,
 * 4, 20.
 */
static void
test_intel_plan(void)
{
	static uint8_t file[2048];
	struct btd_intel_fragment fragments[64];
	struct btd_intel_version version;
	struct btd_intel_plan plan;
	const char *reason;
	size_t length;
	int error;

	/* ECDSA on variant 0x17: three header parts from 644, then the commands from 964. */
	length = make_sfi(file, sizeof(file), 1);
	memset(&version, 0, sizeof(version));
	version.cnvi_bt = 0x00170000U;
	version.sbe_type = 1U;
	memset(&plan, 0, sizeof(plan));
	plan.fragments = fragments;
	plan.capacity = 64U;
	error = btd_intel_plan_make(file, length, &version, &plan, &reason);
	expect(error == 0 && plan.count == 7U, "plan: ECDSA's 3 header parts and 4 command fragments (%d, %u)", error, (unsigned)plan.count);
	expect(fragments[0].type == 0x00U && fragments[0].offset == 644U && fragments[0].length == 128U &&
	       fragments[1].type == 0x03U && fragments[1].offset == 772U && fragments[1].length == 96U &&
	       fragments[2].type == 0x02U && fragments[2].offset == 868U && fragments[2].length == 96U,
	       "plan: ECDSA's CSS, key and signature");
	expect(fragments[3].type == 0x01U && fragments[3].offset == 964U && fragments[3].length == 4U &&
	       fragments[4].length == 252U && fragments[5].length == 4U && fragments[6].length == 20U &&
	       fragments[6].offset == 964U + 4U + 252U + 4U,
	       "plan: the commands in 4, 252, 4 and 20");
	expect(plan.have_boot_parameter && plan.boot_parameter == 0x11223344U, "plan: the boot parameter");

	/* RSA on variant 0x17: the RSA parts, the commands still from 964. */
	version.sbe_type = 0U;
	error = btd_intel_plan_make(file, length, &version, &plan, &reason);
	expect(error == 0 && plan.count == 9U && fragments[0].offset == 0U && fragments[3].offset == 388U &&
	       fragments[4].offset == 516U && fragments[5].offset == 964U,
	       "plan: RSA's five parts on variant 0x17 (4 bytes skipped before the signature)");

	/* RSA alone on variant 0x12: the commands from 644. */
	length = make_sfi(file, sizeof(file), 0);
	version.cnvi_bt = 0x00120000U;
	error = btd_intel_plan_make(file, length, &version, &plan, &reason);
	expect(error == 0 && plan.count == 9U && fragments[5].offset == 644U, "plan: RSA alone, the commands from 644");

	/* A file for the other header, a CSS version wrong, a command past the end, no room. */
	version.cnvi_bt = 0x00170000U;
	error = btd_intel_plan_make(file, length, &version, &plan, &reason);
	expect(error == EINVAL, "plan: an RSA-only file for variant 0x17 is refused");
	version.cnvi_bt = 0x00120000U;
	file[10] = 0x05U;
	error = btd_intel_plan_make(file, length, &version, &plan, &reason);
	expect(error == EINVAL, "plan: a wrong CSS version is refused");
	file[10] = 0x01U;
	file[length - 1U - 3U] = 9U;
	error = btd_intel_plan_make(file, length, &version, &plan, &reason);
	expect(error == EINVAL, "plan: a command past the end is refused");
	length = make_sfi(file, sizeof(file), 0);
	plan.capacity = 6U;
	error = btd_intel_plan_make(file, length, &version, &plan, &reason);
	expect(error == ENOSPC, "plan: too small an array");
}

/* DDC records: [L][L bytes] sent whole; a record of 0 or past the end stops. */
static void
test_ddc(void)
{
	static const uint8_t ddc[] = { 0x03U, 0x01U, 0x02U, 0x03U, 0x02U, 0x05U, 0x06U, 0x00U };
	const uint8_t *record;
	size_t record_length;
	size_t offset;
	int more;

	/* The two records, then the end. */
	offset = 0U;
	more = btd_intel_ddc_next(ddc, 7U, &offset, &record, &record_length);
	expect(more == 1 && record == ddc && record_length == 4U, "ddc: the first record with its length byte");
	more = btd_intel_ddc_next(ddc, 7U, &offset, &record, &record_length);
	expect(more == 1 && record_length == 3U, "ddc: the second record");
	more = btd_intel_ddc_next(ddc, 7U, &offset, &record, &record_length);
	expect(more == 0, "ddc: the end");

	/* A record of length 0, and one past the end. */
	more = btd_intel_ddc_next(ddc, 8U, &offset, &record, &record_length);
	expect(more == -1, "ddc: a record of length 0");
	offset = 4U;
	more = btd_intel_ddc_next(ddc, 6U, &offset, &record, &record_length);
	expect(more == -1, "ddc: a record past the end");
}

/* A plain controller's start and scan. */
static void
test_session_plain(void)
{
	struct btd_session session;
	struct fake fake;
	pthread_t thread;
	unsigned index;
	unsigned found;
	int same;
	int error;

	/* A controller of no vendor in particular. */
	memset(&fake, 0, sizeof(fake));
	fake.info.vendor = 0x1209U;
	fake_start(&fake, &session, &thread);

	/* The start: ready, with what the controller said. */
	error = btd_session_start(&session);
	expect(error == 0 && session.state == BTD_STATE_READY, "session: a plain start is ready (%d %s)", error, session.reason);
	expect(session.have_address && session.address[0] == 0x55U && session.hci_version == 0x0cU && session.manufacturer == 0xffffU,
	       "session: the version and the address");
	expect(session.le && session.p256 && session.dhkey, "session: LE, P-256 and DHKey from the commands and features");
	expect(session.acl_length == 1021U && fake.ioctl_count >= 2U && fake.ioctls[fake.ioctl_count - 1U] == BT_IOC_SET_ACL_MAX &&
	       fake.ioctl_values[fake.ioctl_count - 1U] == 1021U,
	       "session: the node is told the ACL length");

	/* The scan: the inquiry's two devices and the LE scan's two, each once with its fields. */
	error = btd_session_scan_start(&session, 2U);
	expect(error == 0 && session.state == BTD_STATE_SCANNING, "session: the scan starts (%d)", error);
	(void)usleep(100000U);
	for (;;) {
		/* Each packet the controller sent, until none is left. */
		error = btd_session_input(&session);
		if (error != 0)
			break;
	}

	/* The scan's end. */
	error = btd_session_scan_stop(&session);
	expect(error == 0 && session.state == BTD_STATE_READY, "session: the scan stops (%d)", error);
	expect(session.devices.count == 4U, "session: four devices (%u)", session.devices.count);
	found = 0U;
	for (index = 0U; index < session.devices.count; index++) {
		/* The keyboard of the extended result. */
		same = strcmp(session.devices.entries[index].name, "Loopback Keyboard");
		if (same == 0 && session.devices.entries[index].class_of_device == 0x2540U)
			found |= 1U;

		/* The result with RSSI. */
		if (session.devices.entries[index].type == BTD_ADDRESS_BREDR &&
		    session.devices.entries[index].rssi == -60 &&
		    session.devices.entries[index].class_of_device == 0x2580U)
			found |= 2U;

		/* The mouse of the first report. */
		same = strcmp(session.devices.entries[index].name, "Loopback Mouse");
		if (same == 0 && session.devices.entries[index].appearance == 0x03c2U)
			found |= 4U;

		/* The random address of the second report. */
		if (session.devices.entries[index].type == BTD_ADDRESS_LE_RANDOM)
			found |= 8U;
	}

	/* All four. */
	expect(found == 15U, "session: the four devices' fields (%x)", found);
	expect(!session.inquiring && !session.le_scanning && fake.opcodes[fake.opcode_count - 1U] == 0x200cU,
	       "session: the inquiry ended by itself, the LE scan was turned off last");

	/* A hardware error makes the controller start again. */
	fake_send(&fake, (const uint8_t *)"\x04\x10\x01\x01", 4U);
	error = btd_session_input(&session);
	expect(error == 0 && session.state == BTD_STATE_ERROR && session.hardware_errors == 1U, "session: a hardware error");
	fake_stop(&fake, &session, thread);
}

/* Intel: operational; bootloader with a file (load, boot, DDC); a missing file; a load not allowed; a refused fragment. */
static void
test_session_intel(void)
{
	static uint8_t file[2048];
	struct btd_session session;
	struct fake fake;
	pthread_t thread;
	char path[256];
	size_t length;
	int descriptor;
	int error;

	/* Operational: nothing loaded. */
	memset(&fake, 0, sizeof(fake));
	fake.intel = 1;
	fake.info.vendor = 0x8087U;
	fake_start(&fake, &session, &thread);
	error = btd_session_start(&session);
	expect(error == 0 && session.state == BTD_STATE_READY && !session.firmware_loaded && fake.fragments == 0U,
	       "intel: an operational controller is not loaded (%d %s)", error, session.reason);
	fake_stop(&fake, &session, thread);

	/* The firmware files of the bootloader's ids (ibt-7156-00a0). */
	length = make_sfi(file, sizeof(file), 1);
	(void)snprintf(path, sizeof(path), "%s/ibt-7156-00a0.sfi", firmware_folder);
	descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	expect(descriptor >= 0, "intel: the synthetic firmware is written");
	if (descriptor >= 0) {
		expect(write(descriptor, file, length) == (ssize_t)length, "intel: all of it");
		(void)close(descriptor);
	}

	/* Its DDC file of two records. */
	(void)snprintf(path, sizeof(path), "%s/ibt-7156-00a0.ddc", firmware_folder);
	descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (descriptor >= 0) {
		expect(write(descriptor, "\x03\x01\x02\x03\x02\x05\x06", 7U) == 7, "intel: the DDC file");
		(void)close(descriptor);
	}

	/* Bootloader: the fragments as planned, the download's end, the path back to normal before the reset, the boot, DDC. */
	memset(&fake, 0, sizeof(fake));
	fake.intel = 1;
	fake.bootloader = 1;
	fake.info.vendor = 0x8087U;
	fake.file = file;
	fake.file_length = length;
	fake_start(&fake, &session, &thread);
	error = btd_session_start(&session);
	expect(error == 0 && session.state == BTD_STATE_READY && session.firmware_loaded,
	       "intel: the bootloader is loaded (%d %s %s)", error, btd_state_name(session.state), session.reason);
	expect(fake.fragments == 7U && fake.fragment_mismatch == 0U, "intel: 7 fragments with the file's bytes (%u, %u wrong)",
	       fake.fragments, fake.fragment_mismatch);
	expect(fake.resets_sent == 1U && fake.ddc_records == 2U && fake_saw(&fake, 0xfc52U), "intel: one boot, two DDC records, the event mask");
	expect(fake.ioctl_count >= 3U && fake.ioctls[1] == BT_IOC_SET_BOOTLOADER && fake.ioctl_values[1] == 1U &&
	       fake.ioctls[2] == BT_IOC_SET_BOOTLOADER && fake.ioctl_values[2] == 0U,
	       "intel: the bootloader path on for the download, off before the boot");
	expect(session.trace[0] != '\0', "intel: the first answers are kept for the log");
	fake_stop(&fake, &session, thread);

	/* A load not allowed (it came back in its bootloader after one). */
	memset(&fake, 0, sizeof(fake));
	fake.intel = 1;
	fake.bootloader = 1;
	fake.info.vendor = 0x8087U;
	fake.file = file;
	fake.file_length = length;
	fake_start(&fake, &session, &thread);
	session.load_allowed = 0;
	error = btd_session_start(&session);
	expect(error != 0 && session.state == BTD_STATE_FIRMWARE_FAILED && fake.fragments == 0U, "intel: a second load is not tried");
	fake_stop(&fake, &session, thread);

	/* A refused fragment: firmware-failed, the path back to normal. */
	memset(&fake, 0, sizeof(fake));
	fake.intel = 1;
	fake.bootloader = 1;
	fake.secure_send_status = 0x12;
	fake.info.vendor = 0x8087U;
	fake.file = file;
	fake.file_length = length;
	fake_start(&fake, &session, &thread);
	error = btd_session_start(&session);
	expect(error == EIO && session.state == BTD_STATE_FIRMWARE_FAILED && fake.ioctl_values[fake.ioctl_count - 1U] == 0U,
	       "intel: a refused fragment fails the load, the path back to normal (%s)", session.reason);
	fake_stop(&fake, &session, thread);

	/* No file: firmware-needed with the file's path. */
	memset(&fake, 0, sizeof(fake));
	fake.intel = 1;
	fake.bootloader = 1;
	fake.info.vendor = 0x8087U;
	fake_start(&fake, &session, &thread);
	session.firmware_folder = "/nonexistent";
	error = btd_session_start(&session);
	expect(error == ENOENT && session.state == BTD_STATE_FIRMWARE_NEEDED && strstr(session.reason, "ibt-7156-00a0.sfi") != NULL,
	       "intel: no file is firmware-needed (%s)", session.reason);
	fake_stop(&fake, &session, thread);
}

/* A timeout, a stray answer, a node that goes, a bootloader path left on. */
static void
test_session_failures(void)
{
	struct btd_session session;
	struct fake fake;
	pthread_t thread;
	int error;

	/* A controller that never answers Read Local Version. */
	memset(&fake, 0, sizeof(fake));
	fake.info.vendor = 0x1209U;
	fake.silent_opcode = 0x1001;
	fake_start(&fake, &session, &thread);
	error = btd_session_start(&session);
	expect(error == ETIMEDOUT && session.state == BTD_STATE_ERROR && strncmp(session.reason, "local-version", 13U) == 0,
	       "failures: a timeout names its step (%s)", session.reason);
	fake_stop(&fake, &session, thread);

	/* An answer to another command before Reset's is passed over. */
	memset(&fake, 0, sizeof(fake));
	fake.info.vendor = 0x1209U;
	fake.stray_before_reset = 1;
	fake_start(&fake, &session, &thread);
	error = btd_session_start(&session);
	expect(error == 0 && session.stray_answers == 1U, "failures: a stray answer is passed over (%u)", session.stray_answers);
	fake_stop(&fake, &session, thread);

	/* The node goes during the start. */
	memset(&fake, 0, sizeof(fake));
	fake.info.vendor = 0x1209U;
	fake.close_at_opcode = 0x1009;
	fake_start(&fake, &session, &thread);
	error = btd_session_start(&session);
	expect(error == ENODEV && session.state == BTD_STATE_LOST, "failures: a node that goes is lost (%d)", error);
	fake_stop(&fake, &session, thread);

	/* A bootloader path left on goes off first. */
	memset(&fake, 0, sizeof(fake));
	fake.info.vendor = 0x1209U;
	fake.info.flags = BT_INFO_BOOTLOADER;
	fake_start(&fake, &session, &thread);
	error = btd_session_start(&session);
	expect(error == 0 && fake.ioctl_count >= 2U && fake.ioctls[1] == BT_IOC_SET_BOOTLOADER && fake.ioctl_values[1] == 0U,
	       "failures: a bootloader path left on goes off");
	fake_stop(&fake, &session, thread);
}

/* Random and mutated bytes into every parser; ASan and UBSan watch. */
static void
test_fuzz(void)
{
	static uint8_t file[2048];
	struct btd_intel_fragment fragments[600];
	struct btd_intel_version version;
	struct btd_intel_plan plan;
	struct btd_devices devices;
	struct btd_event event;
	struct btd_answer answer;
	const char *reason;
	uint8_t packet[260];
	char text[1100];
	size_t length;
	size_t file_length;
	static const uint8_t codes[4] = { BTD_EVENT_INQUIRY_RESULT, BTD_EVENT_INQUIRY_RSSI, BTD_EVENT_EXTENDED_INQUIRY, BTD_EVENT_LE_META };
	unsigned round;
	unsigned index;
	int error;

	/* A fixed seed: the same run every time. */
	srand(143U);
	file_length = make_sfi(file, sizeof(file), 1);
	btd_devices_clear(&devices);
	for (round = 0U; round < TEST_FUZZ_ROUNDS; round++) {
		/* A random event packet, its length right or not. */
		length = (size_t)(rand() % (int)sizeof(packet));
		for (index = 0U; index < length; index++)
			packet[index] = (uint8_t)rand();
		if (length >= 3U && (round & 1U) != 0U) {
			packet[0] = 0x04U;
			packet[2] = (uint8_t)(length - 3U);
		}

		/* An event taken apart is read as each scan event in turn. */
		error = btd_hci_event(packet, length, &event);
		if (error == 0) {
			(void)btd_hci_answer(&event, &answer);
			btd_devices_clear(&devices);
			event.code = codes[round % 4U];
			(void)btd_devices_take(&devices, &event);
		}

		/* Random data and names. */
		btd_device_data(&devices.entries[0], packet, length);
		memcpy(text, packet, length);
		text[length] = '\0';
		(void)btd_escape(text, (char *)file + 1500, 500U);

		/* Random TLVs. */
		(void)btd_intel_version_parse(packet, length, &version);

		/* A mutated firmware file. */
		file[964U + (unsigned)rand() % (unsigned)(file_length - 964U)] = (uint8_t)rand();
		memset(&version, 0, sizeof(version));
		version.cnvi_bt = 0x00170000U;
		version.sbe_type = 1U;
		memset(&plan, 0, sizeof(plan));
		plan.fragments = fragments;
		plan.capacity = btd_intel_plan_capacity(file_length);
		if (plan.capacity > 600U)
			plan.capacity = 600U;
		(void)btd_intel_plan_make(file, file_length, &version, &plan, &reason);
	}

	/* The fuzz ran to its end. */
	expect(1, "fuzz");
	printf("bt-daemon-host-test: fuzz %u rounds\n", TEST_FUZZ_ROUNDS);
}

/* Starts the scripted controller on a socket pair and the session on the other end, with short timing. */
static void
fake_start(
	struct fake *fake,
	struct btd_session *session,
	pthread_t *thread)
{
	int pair[2];
	int status;

	/* A socket pair that keeps packets apart, as the node does. */
	status = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair);
	if (status != 0) {
		perror("socketpair");
		exit(2);
	}

	/* The session on one end, the controller on the other. */
	fake->descriptor = pair[1];
	btd_session_init(session, pair[0], fake_control, fake, "fake", firmware_folder);
	session->timing.command_ms = 300U;
	session->timing.download_ms = 500U;
	session->timing.boot_ms = 500U;
	status = pthread_create(thread, NULL, fake_run, fake);
	if (status != 0) {
		perror("pthread_create");
		exit(2);
	}
}

/* Ends the scripted controller: the session's end closes, the controller sees it and returns. */
static void
fake_stop(
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

/* The scripted controller: answers each command as the test's fields say, until its socket closes. */
static void *
fake_run(
	void *argument)
{
	static const uint8_t ok[1] = { 0x00U };
	static const uint8_t version[9] = { 0x00U, 0x0cU, 0x00U, 0x01U, 0x0cU, 0xffU, 0xffU, 0x01U, 0x00U };
	static const uint8_t address[7] = { 0x00U, 0x55U, 0x44U, 0x33U, 0x22U, 0x11U, 0x00U };
	static const uint8_t features[9] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x40U, 0x00U, 0x00U, 0x00U };
	static const uint8_t buffer_size[8] = { 0x00U, 0xfdU, 0x03U, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U };
	struct fake *fake;
	uint8_t packet[1100];
	uint8_t returned[80];
	uint8_t event[260];
	uint16_t opcode;
	ssize_t got;
	size_t length;
	size_t data;
	uint8_t status;
	unsigned last;
	int recorded;
	int differs;

	/* Plays until the session's end closes. */
	fake = argument;
	for (;;) {
		/* The next command (the session closing ends the play). */
		got = read(fake->descriptor, packet, sizeof(packet));
		if (got <= 0)
			return NULL;
		if (packet[0] != 0x01U || got < 4)
			continue;
		opcode = (uint16_t)(packet[1] | (packet[2] << 8));

		/* The commands seen, but not a real file's thousands of fragments (the plan counts those). */
		recorded = 1;
		if (fake->plan != NULL && opcode == BTD_INTEL_SECURE_SEND)
			recorded = 0;
		if (recorded && fake->opcode_count < 256U)
			fake->opcodes[fake->opcode_count++] = opcode;

		/* The node goes. */
		if (fake->close_at_opcode != 0 && opcode == (uint16_t)fake->close_at_opcode) {
			(void)shutdown(fake->descriptor, SHUT_RDWR);
			return NULL;
		}

		/* A command left without an answer. */
		if (fake->silent_opcode != 0 && opcode == (uint16_t)fake->silent_opcode)
			continue;

		/* Each command's answer. */
		switch (opcode) {
		case BTD_INTEL_READ_VERSION:
			/* The TLVs: ids, variant 0x17 (or the test's ids, variant and engine), the image, limited CCE 0, ECDSA. */
			length = 0U;
			returned[length++] = 0x00U;
			memcpy(returned + length, "\x10\x04\x67\x45\x23\x01\x11\x04\x00\x0a\x40\x00\x12\x04\x00\x00\x17\x00", 18U);
			if (fake->custom_version) {
				put_le32(returned + length + 2U, fake->cnvi_top);
				put_le32(returned + length + 8U, fake->cnvr_top);
				put_le32(returned + length + 14U, fake->cnvi_bt);
			}

			/* The image type: a bootloader until the boot. */
			length += 18U;
			returned[length++] = 0x1cU;
			returned[length++] = 1U;
			returned[length] = BTD_INTEL_IMAGE_OPERATIONAL;
			if (fake->bootloader)
				returned[length] = BTD_INTEL_IMAGE_BOOTLOADER;
			length++;
			memcpy(returned + length, "\x2e\x01\x00\x2f\x01\x01", 6U);
			if (fake->custom_version)
				returned[length + 5U] = fake->sbe_type;
			length += 6U;
			fake_complete(fake, opcode, returned, length);
			break;
		case BTD_INTEL_SECURE_SEND:
			/* A fragment: its type and bytes must be the planned ones (the test's plan, or the synthetic file's order). */
			data = (size_t)packet[3] - 1U;
			differs = 1;
			if (fake->plan != NULL) {
				differs = fake_planned(fake, packet[4], packet + 5, data);
			} else {
				if (fake->file_offset == 0U && packet[4] == 0x00U)
					fake->file_offset = 644U;
				if (fake->file != NULL && fake->file_offset + data <= fake->file_length)
					differs = memcmp(packet + 5, fake->file + fake->file_offset, data);
				fake->file_offset += data;
			}

			/* Counted, and answered with the status asked for. */
			if (differs != 0)
				fake->fragment_mismatch++;
			fake->fragments++;
			status = (uint8_t)fake->secure_send_status;
			fake_complete(fake, opcode, &status, 1U);

			/* The download's end after the last fragment. */
			last = 7U;
			if (fake->plan != NULL)
				last = (unsigned)fake->plan_count;
			if (fake->fragments == last)
				fake_vendor(fake, BTD_INTEL_EVENT_DOWNLOADED);
			break;
		case BTD_INTEL_RESET:
			/* The boot: no Command Complete, the vendor event, then the operational image. */
			fake->resets_sent++;
			fake->bootloader = 0;
			fake_vendor(fake, BTD_INTEL_EVENT_BOOTED);
			break;
		case BTD_INTEL_WRITE_DDC:
			fake->ddc_records++;
			fake_complete(fake, opcode, ok, sizeof(ok));
			break;
		case 0x0c03U:
			/* Reset, after a stray answer when asked. */
			if (fake->stray_before_reset)
				fake_complete(fake, 0x1234U, ok, sizeof(ok));
			fake_complete(fake, opcode, ok, sizeof(ok));
			break;
		case 0x1001U:
			fake_complete(fake, opcode, version, sizeof(version));
			break;
		case 0x1009U:
			fake_complete(fake, opcode, address, sizeof(address));
			break;
		case 0x1002U:
			/* The commands: Inquiry, LE's event mask and scan, P-256 and DHKey. */
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
			fake_complete(fake, opcode, buffer_size, sizeof(buffer_size));
			break;
		case 0x0401U:
			/* Inquiry: the status, the two results, the end (as the loopback controller). */
			fake_send(fake, (const uint8_t *)"\x04\x0f\x04\x00\x01\x01\x04", 7U);
			memset(event, 0, sizeof(event));
			memcpy(event, "\x04\x2f\xff\x01\x01\x0e\x0d\x0c\x0b\x0a", 10U);
			event[12] = 0x40U;
			event[13] = 0x25U;
			event[17] = (uint8_t)(int8_t)-40;
			memcpy(event + 18, "\x12\x09" "Loopback Keyboard", 19U);
			fake_send(fake, event, 258U);
			memset(event, 0, sizeof(event));
			memcpy(event, "\x04\x22\x0f\x01\x02\x0e\x0d\x0c\x0b\x0a", 10U);
			event[12] = 0x80U;
			event[13] = 0x25U;
			event[17] = (uint8_t)(int8_t)-60;
			fake_send(fake, event, 18U);
			fake_send(fake, (const uint8_t *)"\x04\x01\x01\x00", 4U);
			break;
		case 0x200cU:
			/* LE scan on: the answer and two reports; off: the answer. */
			fake_complete(fake, opcode, ok, sizeof(ok));
			if (packet[4] == 1U) {
				memcpy(event, "\x04\x3e\x23\x02\x01\x00\x00\x03\x0e\x0d\x0c\x0b\x0a\x17\x02\x01\x06\x0f\x09" "Loopback Mouse" "\x03\x19\xc2\x03\xce", 38U);
				fake_send(fake, event, 38U);
				memcpy(event, "\x04\x3e\x0f\x02\x01\x00\x01\x04\x0e\x0d\x0c\x0b\x4a\x03\x02\x01\x06\xba", 18U);
				fake_send(fake, event, 18U);
			}

			break;
		default:
			fake_complete(fake, opcode, ok, sizeof(ok));
			break;
		}
	}
}

/* Writes an H4 event packet to the session. */
static void
fake_send(
	struct fake *fake,
	const uint8_t *event,
	size_t length)
{
	/* One packet. */
	(void)write(fake->descriptor, event, length);
}

/* Writes a Command Complete for an opcode with its return parameters. */
static void
fake_complete(
	struct fake *fake,
	uint16_t opcode,
	const uint8_t *returned,
	size_t count)
{
	uint8_t event[260];

	/* Type, code, length, window, opcode, return parameters. */
	event[0] = 0x04U;
	event[1] = 0x0eU;
	event[2] = (uint8_t)(3U + count);
	event[3] = 1U;
	event[4] = (uint8_t)(opcode & 0xffU);
	event[5] = (uint8_t)(opcode >> 8);
	memcpy(event + 6, returned, count);
	fake_send(fake, event, 6U + count);
}

/* Writes a vendor event of one byte. */
static void
fake_vendor(
	struct fake *fake,
	uint8_t code)
{
	uint8_t event[4];

	/* Type, 0xFF, length 1, the code. */
	event[0] = 0x04U;
	event[1] = 0xffU;
	event[2] = 1U;
	event[3] = code;
	fake_send(fake, event, sizeof(event));
}

/* Stands in for the node's ioctls: the information given, every call recorded. */
static int
fake_control(
	void *context,
	unsigned long request,
	void *argument)
{
	struct fake *fake;

	/* Recorded with its value. */
	fake = context;
	if (fake->ioctl_count < 32U) {
		fake->ioctls[fake->ioctl_count] = request;
		fake->ioctl_values[fake->ioctl_count] = 0U;
		if (request == BT_IOC_SET_BOOTLOADER || request == BT_IOC_SET_ACL_MAX)
			fake->ioctl_values[fake->ioctl_count] = *(uint32_t *)argument;
		fake->ioctl_count++;
	}

	/* The information. */
	if (request == BT_IOC_GET_INFO)
		memcpy(argument, &fake->info, sizeof(fake->info));

	/* Succeeded. */
	return 0;
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

/* Tells whether a Secure Send fragment is the next one the test's plan holds: its type, length and the file's bytes. */
static int
fake_planned(
	const struct fake *fake,
	uint8_t type,
	const uint8_t *data,
	size_t length)
{
	const struct btd_intel_fragment *expected;
	int differs;

	/* A fragment past the plan's end is wrong. */
	if (fake->fragments >= fake->plan_count)
		return 1;

	/* The planned fragment's type and length. */
	expected = &fake->plan[fake->fragments];
	if (type != expected->type)
		return 1;
	if (length != expected->length)
		return 1;

	/* The file's bytes at the planned place. */
	differs = memcmp(data, fake->file + expected->offset, length);
	if (differs != 0)
		return 1;

	/* Succeeded: the fragment is the planned one. */
	return 0;
}

/*
 * The intelbt-firmware package's real files: each .sfi planned under both
 * secure-boot engines with a variant past 0x17 (the AX211's Solar), then a
 * scripted bootloader of each file's ids loaded with it and its .ddc.
 */
static void
test_real_firmware(
	const char *folder)
{
	/* The three pairs of the Solar block, by their CNVi and CNVR ids. */
	test_real_load(folder, "ibt-0040-0041", 0x00000400U, 0x00000410U);
	test_real_load(folder, "ibt-0041-0041", 0x00000410U, 0x00000410U);
	test_real_load(folder, "ibt-1040-0041", 0x00000401U, 0x00000410U);
}

/*
 * Checks one real file's plan: the header's fragments, then command
 * fragments that follow each other from byte 964 without a gap, each of
 * 252 bytes or a multiple of 4, the boot parameter found, and what is
 * left unsent at the end shorter than one fragment.
 */
static void
test_real_plan(
	const char *name,
	const uint8_t *file,
	size_t length,
	uint8_t sbe_type,
	struct btd_intel_plan *plan)
{
	struct btd_intel_version version;
	const struct btd_intel_fragment *fragment;
	const char *reason;
	size_t next;
	size_t index;
	size_t header;
	unsigned wrong;
	int error;

	/* A bootloader of variant 0x18 with the engine asked for. */
	memset(&version, 0, sizeof(version));
	version.have_image_type = 1;
	version.image_type = BTD_INTEL_IMAGE_BOOTLOADER;
	version.have_cnvi_bt = 1;
	version.cnvi_bt = 0x00180000U;
	version.have_sbe_type = 1;
	version.sbe_type = sbe_type;
	version.have_limited_cce = 1;

	/* The plan of the file (the reason is NULL on success). */
	reason = NULL;
	error = btd_intel_plan_make(file, length, &version, plan, &reason);
	if (reason == NULL)
		reason = "-";
	expect(error == 0 && plan->have_boot_parameter, "real: %s.sfi is planned under engine %u (%s)", name, sbe_type, reason);
	if (error != 0)
		return;

	/* The header's three parts come first (CSS, key, signature; RSA splits the key and the signature in two). */
	header = 3U;
	if (sbe_type == 0U)
		header = 5U;
	expect(plan->count > header && plan->fragments[0].type == BTD_INTEL_FRAGMENT_CSS,
	       "real: %s.sfi's header fragments (%zu in all)", name, plan->count);

	/* The commands' fragments, one after another from the end of both headers. */
	next = 964U;
	wrong = 0U;
	for (index = header; index < plan->count; index++) {
		fragment = &plan->fragments[index];

		/* A command fragment, just where the last one ended. */
		if (fragment->type != BTD_INTEL_FRAGMENT_COMMANDS || fragment->offset != next)
			wrong++;

		/* A full fragment, or what waited at a command's end in a multiple of 4. */
		if (fragment->length != BTD_INTEL_FRAGMENT_MAX && (fragment->length % 4U) != 0U)
			wrong++;

		/* Where the next one must start. */
		next = fragment->offset + fragment->length;
	}

	/* No fragment out of place, and the whole file sent but less than one fragment. */
	expect(wrong == 0U, "real: %s.sfi's command fragments follow each other (%u wrong)", name, wrong);
	expect(next <= length && length - next < BTD_INTEL_FRAGMENT_MAX,
	       "real: %s.sfi is sent up to %zu of %zu bytes", name, next, length);

	/* The plan's shape for the log. */
	printf("bt-daemon-host-test: real %s.sfi engine %u: %zu fragments, %zu of %zu bytes sent\n", name, sbe_type, plan->count,
	       next, length);
}

/* Plans one real .sfi under both engines, then loads it and its .ddc into a scripted bootloader of its ids. */
static void
test_real_load(
	const char *folder,
	const char *name,
	uint32_t cnvi_top,
	uint32_t cnvr_top)
{
	struct btd_intel_fragment *fragments;
	struct btd_intel_plan plan;
	struct btd_session session;
	struct fake fake;
	pthread_t thread;
	uint8_t *file;
	char path[512];
	size_t length;
	size_t capacity;
	int error;

	/* The file, read whole. */
	(void)snprintf(path, sizeof(path), "%s/%s.sfi", folder, name);
	file = read_whole(path, &length);
	expect(file != NULL, "real: %s is read", path);
	if (file == NULL)
		return;

	/* Room for the plan's fragments. */
	capacity = btd_intel_plan_capacity(length);
	fragments = calloc(capacity, sizeof(*fragments));
	if (fragments == NULL) {
		perror("calloc");
		exit(2);
	}

	/* The plan under RSA, then under ECDSA (kept for the load). */
	plan.fragments = fragments;
	plan.capacity = capacity;
	test_real_plan(name, file, length, 0U, &plan);
	test_real_plan(name, file, length, 1U, &plan);

	/* A bootloader of the file's ids, variant 0x18, ECDSA: every fragment as planned, the boot, the .ddc's two records. */
	memset(&fake, 0, sizeof(fake));
	fake.intel = 1;
	fake.bootloader = 1;
	fake.info.vendor = 0x8087U;
	fake.custom_version = 1;
	fake.cnvi_top = cnvi_top;
	fake.cnvr_top = cnvr_top;
	fake.cnvi_bt = 0x00180000U;
	fake.sbe_type = 1U;
	fake.file = file;
	fake.file_length = length;
	fake.plan = plan.fragments;
	fake.plan_count = plan.count;
	fake_start(&fake, &session, &thread);
	session.firmware_folder = folder;
	error = btd_session_start(&session);
	expect(error == 0 && session.state == BTD_STATE_READY && session.firmware_loaded,
	       "real: %s is loaded (%d %s %s)", name, error, btd_state_name(session.state), session.reason);
	expect(fake.fragments == plan.count && fake.fragment_mismatch == 0U,
	       "real: %s's %zu fragments as planned (%u sent, %u wrong)", name, plan.count, fake.fragments, fake.fragment_mismatch);
	expect(fake.resets_sent == 1U && fake.ddc_records == 2U && fake_saw(&fake, 0xfc52U),
	       "real: %s boots once, its .ddc's two records, the event mask (%u %u)", name, fake.resets_sent, fake.ddc_records);
	fake_stop(&fake, &session, thread);

	/* The file and the plan go. */
	free(fragments);
	free(file);
}

/* Reads a whole file into memory the caller frees; NULL when it cannot be read. */
static uint8_t *
read_whole(
	const char *path,
	size_t *length)
{
	struct stat status;
	uint8_t *bytes;
	ssize_t got;
	size_t done;
	int descriptor;
	int result;

	/* The file and its size. */
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return NULL;

	/* A file with something in it. */
	result = fstat(descriptor, &status);
	if (result != 0 || status.st_size <= 0) {
		(void)close(descriptor);
		return NULL;
	}

	/* Room for all of it. */
	bytes = malloc((size_t)status.st_size);
	if (bytes == NULL) {
		(void)close(descriptor);
		return NULL;
	}

	/* Every byte. */
	done = 0U;
	while (done < (size_t)status.st_size) {
		got = read(descriptor, bytes + done, (size_t)status.st_size - done);
		if (got <= 0) {
			free(bytes);
			(void)close(descriptor);
			return NULL;
		}

		/* The part read. */
		done += (size_t)got;
	}

	/* Succeeded: the file's bytes. */
	(void)close(descriptor);
	*length = done;
	return bytes;
}

/* Writes a little-endian 32-bit value. */
static void
put_le32(
	uint8_t *at,
	uint32_t value)
{
	/* The four bytes, least significant first. */
	at[0] = (uint8_t)(value & 0xffU);
	at[1] = (uint8_t)((value >> 8) & 0xffU);
	at[2] = (uint8_t)((value >> 16) & 0xffU);
	at[3] = (uint8_t)((value >> 24) & 0xffU);
}
