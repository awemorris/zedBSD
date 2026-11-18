/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of bluetoothd's HCI without system calls (ws143-p003): an HCI
 * command built as an H4 packet, an event taken apart, the devices an
 * inquiry or an LE scan found, with their names from the extended inquiry
 * response or the advertising data, and the bits of Read Local Supported
 * Commands.  The host tests build them.
 *
 * Everything here reads bytes that came over the air: every length is
 * checked against what is there before it is used.
 */

#ifndef BLUETOOTHD_HCI_H
#define BLUETOOTHD_HCI_H

#include <stddef.h>
#include <stdint.h>

/* The length of a Bluetooth device address. */
#define BTD_ADDRESS_BYTES	6U

/* The longest device name kept (the Core's 248 bytes) and its NUL. */
#define BTD_NAME_MAX		249U

/* How many devices one scan keeps. */
#define BTD_DEVICES_MAX		64U

/* The length of Read Local Supported Commands' bit field. */
#define BTD_COMMANDS_BYTES	64U

/* The events bluetoothd takes apart. */
#define BTD_EVENT_INQUIRY_COMPLETE	0x01U
#define BTD_EVENT_INQUIRY_RESULT	0x02U
#define BTD_EVENT_COMMAND_COMPLETE	0x0eU
#define BTD_EVENT_COMMAND_STATUS	0x0fU
#define BTD_EVENT_INQUIRY_RSSI		0x22U
#define BTD_EVENT_EXTENDED_INQUIRY	0x2fU
#define BTD_EVENT_LE_META		0x3eU
#define BTD_EVENT_VENDOR		0xffU

/* The LE meta event's subevent of an advertising report. */
#define BTD_LE_ADVERTISING_REPORT	0x02U

/* The kinds of address a device is found with. */
#define BTD_ADDRESS_BREDR	0U
#define BTD_ADDRESS_LE_PUBLIC	1U
#define BTD_ADDRESS_LE_RANDOM	2U

/*
 * One device a scan found: its address (as on the air, least significant
 * byte first) and its kind, and what the scan learnt of it.  A has_ flag
 * says whether the field was seen.
 */
struct btd_device {
	uint8_t address[BTD_ADDRESS_BYTES];
	unsigned type;
	int has_rssi;
	int rssi;
	int has_class;
	uint32_t class_of_device;
	int has_appearance;
	uint16_t appearance;
	int name_complete;
	char name[BTD_NAME_MAX];
};

/*
 * The devices of one scan, each address and kind once.  It lives in the
 * daemon's session; a new scan empties it.
 */
struct btd_devices {
	struct btd_device entries[BTD_DEVICES_MAX];
	unsigned count;
	unsigned dropped;
};

/*
 * An event taken apart: its code and its parameters, which point into the
 * packet the event came in (they live as long as it).
 */
struct btd_event {
	uint8_t code;
	const uint8_t *parameters;
	size_t length;
};

/*
 * The answer to a command, from a Command Complete or a Command Status: the
 * command's opcode, how many more commands the controller takes, whether it
 * was a Command Status, the status, and the return parameters after the
 * status (Command Complete only; they point into the event).
 */
struct btd_answer {
	uint16_t opcode;
	unsigned window;
	int is_status;
	uint8_t status;
	const uint8_t *returned;
	size_t returned_length;
};

size_t btd_hci_command(uint8_t *packet, size_t size, uint16_t opcode, const uint8_t *parameters, size_t count);
int btd_hci_event(const uint8_t *packet, size_t length, struct btd_event *event);
int btd_hci_answer(const struct btd_event *event, struct btd_answer *answer);
int btd_hci_supported(const uint8_t *commands, size_t length, unsigned octet, unsigned bit);
void btd_devices_clear(struct btd_devices *devices);
int btd_devices_take(struct btd_devices *devices, const struct btd_event *event);
void btd_device_data(struct btd_device *device, const uint8_t *data, size_t length);
void btd_format_address(const uint8_t *address, char *text, size_t size);
int btd_escape(const char *text, char *output, size_t size);
const char *btd_address_type_name(unsigned type);

#endif
