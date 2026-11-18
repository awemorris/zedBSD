/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of bluetoothd's Broadcom firmware load without system calls
 * (ws215-p005, see broadcom.h and plan/ws215/phase001/phase.md section 4).
 *
 * A .hcd file is nothing but HCI commands one after the other, each its
 * opcode (two bytes, least significant first), its parameters' length
 * (one byte) and its parameters: Write RAM records, then one Launch RAM,
 * which starts the patch.  The daemon sends them as they are; nothing in
 * the parameters is interpreted (the licence of the file forbids more).
 */

#include "userland/base/bluetoothd/broadcom.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* An opcode's group (its top six bits), and the vendor's group. */
#define BROADCOM_OGF_SHIFT		10U
#define BROADCOM_OGF_VENDOR		0x3fU

/* The bytes of a Bluetooth device address. */
#define BROADCOM_ADDRESS_BYTES		6U

/* The longest name Read Local Name answers (Core Vol 4 Part E §7.3.12). */
#define BROADCOM_NAME_MAX		248U

/* The first and last printable bytes of a name shown in a reason. */
#define BROADCOM_PRINTABLE_FIRST	0x20U
#define BROADCOM_PRINTABLE_LAST		0x7eU

/*
 * A chip this daemon knows: the name its ROM answers Read Local Name
 * with, and the patch file it is loaded from.  The name is never made
 * into a path itself.
 */
struct broadcom_chip {
	const char *name;
	const char *file;
};

/*
 * The chips known, looked up by their exact name: the Raspberry Pi 4's
 * BCM43455 answers "BCM4345C0" (to be confirmed on the board), and its
 * patch is the brcmbt-firmware package's BCM4345C0.hcd.
 */
static const struct broadcom_chip broadcom_chips[] = {
	{ "BCM4345C0", "BCM4345C0.hcd" }
};

/*
 * The addresses a controller has when nothing gave it one, in the HCI's
 * order (least significant byte first): none at all, and the BCM4345C0
 * ROM's default (43:45:C0:00:1F:AC as reported by users of boards without
 * an address set; to be confirmed on the board, ws215-p001 D3).
 */
static const uint8_t broadcom_default_addresses[][BROADCOM_ADDRESS_BYTES] = {
	{ 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U },
	{ 0xacU, 0x1fU, 0x00U, 0xc0U, 0x45U, 0x43U }
};

static void broadcom_printable(const uint8_t *name, size_t length, char *chip, size_t chip_size);
static int broadcom_default_address(const uint8_t *address);

/*
 * Tells whether a controller's manufacturer (Read Local Version) is
 * Broadcom or Cypress, whose controllers are loaded the same way.
 */
int
btd_broadcom_manufacturer(
	uint16_t manufacturer)
{
	/* Broadcom's own. */
	if (manufacturer == BTD_BROADCOM_MANUFACTURER)
		return 1;

	/* Cypress's (now Infineon's), the same chips. */
	if (manufacturer == BTD_CYPRESS_MANUFACTURER)
		return 1;

	/* Someone else's. */
	return 0;
}

/*
 * Writes the path of the patch file of a chip whose Read Local Name
 * answer (the name's bytes, up to their NUL) is given, and the name as
 * the log may show it (bytes that cannot be printed become '?').
 * Returns 0, ENOENT for a chip the table does not know, or ENAMETOOLONG.
 */
int
btd_broadcom_file_name(
	const uint8_t *name,
	size_t length,
	const char *folder,
	char *path,
	size_t size,
	char *chip,
	size_t chip_size)
{
	size_t name_length;
	size_t index;
	size_t known;
	int written;
	int same;

	/* No name is longer than Read Local Name's field. */
	if (length > BROADCOM_NAME_MAX)
		length = BROADCOM_NAME_MAX;

	/* The name ends at its NUL, or at the answer's end. */
	name_length = 0U;
	while (name_length < length && name[name_length] != 0U)
		name_length++;

	/* The name as the log shows it. */
	broadcom_printable(name, name_length, chip, chip_size);

	/* Each chip known, compared with the whole name. */
	for (index = 0U; index < sizeof(broadcom_chips) / sizeof(broadcom_chips[0]); index++) {
		known = strlen(broadcom_chips[index].name);
		if (known != name_length)
			continue;
		same = memcmp(broadcom_chips[index].name, name, known);
		if (same != 0)
			continue;

		/* The chip's file in the folder. */
		written = snprintf(path, size, "%s/%s", folder, broadcom_chips[index].file);
		if (written < 0 || (size_t)written >= size)
			return ENAMETOOLONG;

		/* Succeeded: the path. */
		return 0;
	}

	/* A chip this daemon has no file for. */
	return ENOENT;
}

/*
 * Checks a .hcd file before anything of it is sent: every record whole
 * and a vendor command, Launch RAM last and nowhere else.  Returns 0 with
 * the records' count, or EBADMSG with the reason.
 */
int
btd_broadcom_hcd_check(
	const uint8_t *file,
	size_t length,
	size_t *records,
	const char **reason)
{
	struct btd_broadcom_record record;
	size_t offset;
	size_t count;
	int more;

	/* No records yet. */
	*records = 0U;

	/* A file with nothing in it. */
	if (length == 0U) {
		*reason = "an empty file";
		return EBADMSG;
	}

	/* Each record in turn, up to the file's end. */
	offset = 0U;
	count = 0U;
	for (;;) {
		more = btd_broadcom_hcd_next(file, length, &offset, &record);
		if (more == 0)
			break;
		if (more < 0) {
			*reason = "a record cut short";
			return EBADMSG;
		}

		/* A record must be one of the vendor's commands. */
		if ((record.opcode >> BROADCOM_OGF_SHIFT) != BROADCOM_OGF_VENDOR) {
			*reason = "a record that is not a vendor command";
			return EBADMSG;
		}

		/* Launch RAM starts the patch, so nothing may come after it. */
		if (record.opcode == BTD_BROADCOM_LAUNCH_RAM && offset != length) {
			*reason = "launch-ram before the end";
			return EBADMSG;
		}

		/* One more record. */
		count++;

		/* The last record must be Launch RAM. */
		if (offset == length && record.opcode != BTD_BROADCOM_LAUNCH_RAM) {
			*reason = "no launch-ram at the end";
			return EBADMSG;
		}
	}

	/* Succeeded: the records' count. */
	*records = count;
	return 0;
}

/*
 * Takes the next record of a .hcd file at *offset, which moves past it.
 * Returns 1 with the record, 0 at the file's end, or -1 for a record that
 * runs past the end.
 */
int
btd_broadcom_hcd_next(
	const uint8_t *file,
	size_t length,
	size_t *offset,
	struct btd_broadcom_record *record)
{
	size_t parameters;

	/* The end of the file. */
	if (*offset >= length)
		return 0;

	/* The header must lie inside the file. */
	if (length - *offset < BTD_BROADCOM_RECORD_HEADER)
		return -1;

	/* The parameters must too. */
	parameters = file[*offset + 2U];
	if (length - *offset - BTD_BROADCOM_RECORD_HEADER < parameters)
		return -1;

	/* The record: its opcode, least significant byte first, and its parameters. */
	record->opcode = (uint16_t)file[*offset];
	record->opcode |= (uint16_t)(file[*offset + 1U] << 8);
	record->parameters = file + *offset + BTD_BROADCOM_RECORD_HEADER;
	record->length = parameters;
	*offset += BTD_BROADCOM_RECORD_HEADER + parameters;

	/* Succeeded: a record. */
	return 1;
}

/*
 * Writes Update UART Baud Rate's parameters for a speed: two zero bytes,
 * then the speed in baud, least significant byte first.
 */
void
btd_broadcom_baud(
	uint32_t baud,
	uint8_t *parameters)
{
	/* The two bytes that come before the speed. */
	parameters[0] = 0x00U;
	parameters[1] = 0x00U;

	/* The speed. */
	parameters[2] = (uint8_t)(baud & 0xffU);
	parameters[3] = (uint8_t)((baud >> 8) & 0xffU);
	parameters[4] = (uint8_t)((baud >> 16) & 0xffU);
	parameters[5] = (uint8_t)((baud >> 24) & 0xffU);
}

/*
 * Decides what becomes of the controller's address (Read BD_ADDR's, in
 * the HCI's order) when the board gives one or not: an address of the
 * controller's own stays, as the device tree's fallback-bd-address asks;
 * a default one is replaced by the board's, or kept when the board has
 * none.
 */
enum btd_broadcom_address
btd_broadcom_address_choice(
	const uint8_t *controller,
	const uint8_t *board,
	int have_board)
{
	int is_default;
	int same;

	/* The board's address already: nothing to write. */
	if (have_board) {
		same = memcmp(controller, board, BROADCOM_ADDRESS_BYTES);
		if (same == 0)
			return BTD_BROADCOM_ADDRESS_KEEP;
	}

	/* An address of the controller's own stays. */
	is_default = broadcom_default_address(controller);
	if (!is_default)
		return BTD_BROADCOM_ADDRESS_KEEP;

	/* A default address and no board's to put in its place. */
	if (!have_board)
		return BTD_BROADCOM_ADDRESS_DEFAULT;

	/* Succeeded: the board's address replaces the default one. */
	return BTD_BROADCOM_ADDRESS_WRITE;
}

/* Copies a name for the log, each byte that cannot be printed as '?', cut to the room. */
static void
broadcom_printable(
	const uint8_t *name,
	size_t length,
	char *chip,
	size_t chip_size)
{
	size_t index;

	/* No room at all. */
	if (chip_size == 0U)
		return;

	/* Each byte that fits, with the NUL's place kept. */
	for (index = 0U; index < length && index + 1U < chip_size; index++) {
		if (name[index] < BROADCOM_PRINTABLE_FIRST || name[index] > BROADCOM_PRINTABLE_LAST) {
			chip[index] = '?';
		} else {
			chip[index] = (char)name[index];
		}
	}

	/* The end of the copy. */
	chip[index] = '\0';
}

/* Tells whether an address is one a controller has when nothing gave it one. */
static int
broadcom_default_address(
	const uint8_t *address)
{
	size_t index;
	int same;

	/* Each default address known. */
	for (index = 0U; index < sizeof(broadcom_default_addresses) / sizeof(broadcom_default_addresses[0]); index++) {
		same = memcmp(address, broadcom_default_addresses[index], BROADCOM_ADDRESS_BYTES);
		if (same == 0)
			return 1;
	}

	/* An address of the controller's own. */
	return 0;
}
