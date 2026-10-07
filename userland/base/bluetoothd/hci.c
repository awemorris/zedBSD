/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of bluetoothd's HCI without system calls (ws143-p003, see
 * hci.h).
 *
 * The layouts are the Bluetooth Core 5.4's (Vol 4 Part E §7.7 for the
 * events, Vol 3 Part C §8 and the Core Specification Supplement for the
 * extended inquiry response and the advertising data).  An inquiry result
 * and an advertising report carrying several responses are read one whole
 * response after another, as controllers send them.
 */

#include "userland/base/bluetoothd/hci.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The H4 packet types bluetoothd writes and reads. */
#define HCI_PACKET_COMMAND		0x01U
#define HCI_PACKET_EVENT		0x04U

/* The size of an H4 command packet's header (type, opcode, length) and of an event's (type, code, length). */
#define HCI_COMMAND_HEADER		4U
#define HCI_EVENT_HEADER		3U

/* One response of Inquiry Result and of Inquiry Result with RSSI: both are 14 bytes. */
#define HCI_INQUIRY_RESPONSE		14U

/* Where a response's fields are: the address, the class of device, and (with RSSI) the RSSI. */
#define HCI_INQUIRY_CLASS		9U
#define HCI_INQUIRY_RSSI_CLASS		8U
#define HCI_INQUIRY_RSSI_RSSI		13U

/* Extended Inquiry Result: one response of 15 bytes before its 240 bytes of data. */
#define HCI_EXTENDED_HEAD		15U
#define HCI_EXTENDED_CLASS		9U
#define HCI_EXTENDED_RSSI		14U

/* An advertising report's fixed head: event type, address type, address, data length. */
#define HCI_REPORT_HEAD			9U

/* The RSSI value that means none was measured. */
#define HCI_RSSI_NONE			127

/* The data types bluetoothd reads (Assigned Numbers, Common Data Types). */
#define HCI_DATA_SHORT_NAME		0x08U
#define HCI_DATA_COMPLETE_NAME		0x09U
#define HCI_DATA_CLASS			0x0dU
#define HCI_DATA_APPEARANCE		0x19U

static int hci_inquiry(struct btd_devices *devices, const struct btd_event *event, int with_rssi);
static int hci_extended(struct btd_devices *devices, const struct btd_event *event);
static int hci_le_reports(struct btd_devices *devices, const struct btd_event *event);
static struct btd_device *hci_device(struct btd_devices *devices, const uint8_t *address, unsigned type);
static void hci_name(struct btd_device *device, const uint8_t *value, size_t length, int complete);
static uint32_t hci_class(const uint8_t *bytes);
static size_t hci_plain_run(const unsigned char *byte);

/*
 * Writes an HCI command as an H4 packet: the type, the opcode, the length
 * and the parameters.  Returns the packet's length, or 0 when it does not
 * fit or the parameters are longer than a command takes.
 */
size_t
btd_hci_command(
	uint8_t *packet,
	size_t size,
	uint16_t opcode,
	const uint8_t *parameters,
	size_t count)
{
	/* A command takes at most 255 bytes of parameters. */
	if (count > 255U)
		return 0U;

	/* The packet must hold the header and the parameters. */
	if (size < HCI_COMMAND_HEADER + count)
		return 0U;

	/* The header: the type, the opcode least significant byte first, the length. */
	packet[0] = HCI_PACKET_COMMAND;
	packet[1] = (uint8_t)(opcode & 0xffU);
	packet[2] = (uint8_t)(opcode >> 8);
	packet[3] = (uint8_t)count;

	/* The parameters after it. */
	if (count != 0U)
		memcpy(packet + HCI_COMMAND_HEADER, parameters, count);

	/* Succeeded: the packet's length. */
	return HCI_COMMAND_HEADER + count;
}

/*
 * Takes an H4 event packet apart into its code and its parameters.  Returns
 * 0, or EBADMSG for a packet that is not an event or whose length is not
 * the one its header says.
 */
int
btd_hci_event(
	const uint8_t *packet,
	size_t length,
	struct btd_event *event)
{
	/* An event has its type and two header bytes. */
	if (length < HCI_EVENT_HEADER)
		return EBADMSG;
	if (packet[0] != HCI_PACKET_EVENT)
		return EBADMSG;

	/* The header's length must be what follows it. */
	if ((size_t)packet[2] != length - HCI_EVENT_HEADER)
		return EBADMSG;

	/* Succeeded: the code and the parameters. */
	event->code = packet[1];
	event->parameters = packet + HCI_EVENT_HEADER;
	event->length = length - HCI_EVENT_HEADER;
	return 0;
}

/*
 * Reads the answer to a command from a Command Complete or a Command
 * Status.  Returns 1 for an answer, 0 for another event, or -1 for an
 * answer too short to be one.
 */
int
btd_hci_answer(
	const struct btd_event *event,
	struct btd_answer *answer)
{
	const uint8_t *bytes;

	/* The parameters. */
	bytes = event->parameters;
	memset(answer, 0, sizeof(*answer));

	/* Chooses by the event's code. */
	if (event->code == BTD_EVENT_COMMAND_COMPLETE) {
		/* The window and the opcode, then the return parameters, which start with the status. */
		if (event->length < 3U)
			return -1;
		answer->window = bytes[0];
		answer->opcode = (uint16_t)(bytes[1] | (bytes[2] << 8));
		answer->returned = bytes + 3;
		answer->returned_length = event->length - 3U;
		if (answer->returned_length != 0U)
			answer->status = bytes[3];
	} else if (event->code == BTD_EVENT_COMMAND_STATUS) {
		/* The status, the window and the opcode, nothing more. */
		if (event->length != 4U)
			return -1;
		answer->is_status = 1;
		answer->status = bytes[0];
		answer->window = bytes[1];
		answer->opcode = (uint16_t)(bytes[2] | (bytes[3] << 8));
	} else {
		/* Any other event answers nothing. */
		return 0;
	}

	/* Succeeded: an answer. */
	return 1;
}

/*
 * Tells whether Read Local Supported Commands' field has a command's bit
 * (octet and bit as the Core's table names them).  A field too short to
 * have it says no.
 */
int
btd_hci_supported(
	const uint8_t *commands,
	size_t length,
	unsigned octet,
	unsigned bit)
{
	/* A field that ends before the octet does not have the command. */
	if (octet >= length || bit > 7U)
		return 0;

	/* The bit decides. */
	if ((commands[octet] & (1U << bit)) == 0U)
		return 0;

	/* Succeeded: the controller has the command. */
	return 1;
}

/*
 * Empties the devices' table for a new scan.
 */
void
btd_devices_clear(
	struct btd_devices *devices)
{
	/* No device, nothing dropped. */
	memset(devices, 0, sizeof(*devices));
}

/*
 * Takes the devices an inquiry result or an LE advertising report names
 * into the table.  Returns 1 for such an event, 0 for any other, or -1 for
 * one whose lengths do not add up (the responses read before it are kept).
 */
int
btd_devices_take(
	struct btd_devices *devices,
	const struct btd_event *event)
{
	int taken;

	/* Chooses the layout by the event. */
	switch (event->code) {
	case BTD_EVENT_INQUIRY_RESULT:
		taken = hci_inquiry(devices, event, 0);
		break;
	case BTD_EVENT_INQUIRY_RSSI:
		taken = hci_inquiry(devices, event, 1);
		break;
	case BTD_EVENT_EXTENDED_INQUIRY:
		taken = hci_extended(devices, event);
		break;
	case BTD_EVENT_LE_META:
		taken = hci_le_reports(devices, event);
		break;
	default:
		taken = 0;
		break;
	}

	/* Succeeded: what the event was. */
	return taken;
}

/*
 * Reads a device's extended inquiry response or advertising data: a run of
 * [length][type][value] structures, ended by a length of 0 or by the data's
 * end.  A structure that runs past the end ends the reading.
 */
void
btd_device_data(
	struct btd_device *device,
	const uint8_t *data,
	size_t length)
{
	const uint8_t *value;
	size_t offset;
	size_t size;
	uint8_t type;

	/* Each structure in turn. */
	offset = 0U;
	while (offset < length) {
		/* Its length, which counts the type byte; 0 ends the data. */
		size = data[offset];
		if (size == 0U)
			break;

		/* A structure that runs past the end is not read. */
		if (offset + 1U + size > length)
			break;

		/* Its type and value. */
		type = data[offset + 1U];
		value = data + offset + 2U;

		/* Takes the fields bluetoothd shows. */
		switch (type) {
		case HCI_DATA_SHORT_NAME:
			hci_name(device, value, size - 1U, 0);
			break;
		case HCI_DATA_COMPLETE_NAME:
			hci_name(device, value, size - 1U, 1);
			break;
		case HCI_DATA_CLASS:
			if (size - 1U == 3U) {
				device->class_of_device = hci_class(value);
				device->has_class = 1;
			}

			break;
		case HCI_DATA_APPEARANCE:
			if (size - 1U == 2U) {
				device->appearance = (uint16_t)(value[0] | (value[1] << 8));
				device->has_appearance = 1;
			}

			break;
		default:
			break;
		}

		/* The next structure. */
		offset += 1U + size;
	}
}

/*
 * Writes an address as the usual text, most significant byte first
 * (00:11:22:33:44:55).
 */
void
btd_format_address(
	const uint8_t *address,
	char *text,
	size_t size)
{
	/* The bytes from the last, as people read an address. */
	(void)snprintf(text,
		       size,
		       "%02X:%02X:%02X:%02X:%02X:%02X",
		       address[5],
		       address[4],
		       address[3],
		       address[2],
		       address[1],
		       address[0]);
}

/*
 * Writes text for a quoted word of a line: printable ASCII and well-formed
 * UTF-8 stay, and every other byte (a control byte, DEL, a C1 control, a
 * byte of a broken or overlong sequence, the quote and the backslash)
 * becomes \xNN, so that a name from the air cannot break the line or reach
 * a terminal as a control sequence.  Returns 0, or ENAMETOOLONG when it
 * does not fit (the output is then empty).
 */
int
btd_escape(
	const char *text,
	char *output,
	size_t size)
{
	const unsigned char *byte;
	size_t used;
	size_t run;
	size_t index;

	/* Somewhere to write, at least the NUL. */
	if (output == NULL || size == 0U)
		return EINVAL;

	/* Each byte, or each whole UTF-8 sequence, as itself or as \xNN. */
	used = 0U;
	byte = (const unsigned char *)text;
	while (*byte != '\0') {
		/* How many bytes stay as they are from here (0: the byte is escaped). */
		run = hci_plain_run(byte);

		/* A plain run is copied. */
		if (run != 0U && used + run < size) {
			for (index = 0U; index < run; index++)
				output[used + index] = (char)byte[index];
			used += run;
			byte += run;
			continue;
		}

		/* An escaped byte takes four. */
		if (run == 0U && used + 4U < size) {
			(void)snprintf(output + used, 5U, "\\x%02x", (unsigned)*byte);
			used += 4U;
			byte++;
			continue;
		}

		/* It does not fit. */
		output[0] = '\0';
		return ENAMETOOLONG;
	}

	/* Succeeded: the text, ended. */
	output[used] = '\0';
	return 0;
}

/*
 * Names a kind of address for the socket's lines.
 */
const char *
btd_address_type_name(
	unsigned type)
{
	/* The kinds bluetoothd keeps. */
	if (type == BTD_ADDRESS_LE_PUBLIC)
		return "le-public";
	if (type == BTD_ADDRESS_LE_RANDOM)
		return "le-random";

	/* Succeeded: BR/EDR. */
	return "bredr";
}

/* Takes the responses of Inquiry Result (with or without RSSI): whole 14-byte responses one after another. */
static int
hci_inquiry(
	struct btd_devices *devices,
	const struct btd_event *event,
	int with_rssi)
{
	struct btd_device *device;
	const uint8_t *response;
	unsigned count;
	unsigned index;

	/* The number of responses, and room for all of them. */
	if (event->length < 1U)
		return -1;
	count = event->parameters[0];
	if (event->length < 1U + (size_t)count * HCI_INQUIRY_RESPONSE)
		return -1;

	/* Each response: its address, its class of device, and the RSSI when there is one. */
	for (index = 0U; index < count; index++) {
		response = event->parameters + 1U + (size_t)index * HCI_INQUIRY_RESPONSE;
		device = hci_device(devices, response, BTD_ADDRESS_BREDR);
		if (device == NULL)
			continue;

		/* The class sits one byte earlier when the response carries an RSSI (one reserved byte, not two). */
		if (with_rssi) {
			device->class_of_device = hci_class(response + HCI_INQUIRY_RSSI_CLASS);
			device->rssi = (int)(int8_t)response[HCI_INQUIRY_RSSI_RSSI];
			device->has_rssi = 1;
		} else {
			device->class_of_device = hci_class(response + HCI_INQUIRY_CLASS);
		}

		/* Either way the class was seen. */
		device->has_class = 1;
	}

	/* Succeeded: an inquiry result. */
	return 1;
}

/* Takes Extended Inquiry Result: one response and its extended inquiry response. */
static int
hci_extended(
	struct btd_devices *devices,
	const struct btd_event *event)
{
	struct btd_device *device;
	const uint8_t *response;

	/* One response and its head. */
	if (event->length < HCI_EXTENDED_HEAD)
		return -1;
	response = event->parameters + 1U;

	/* Its address, class and RSSI. */
	device = hci_device(devices, response, BTD_ADDRESS_BREDR);
	if (device == NULL)
		return 1;
	device->class_of_device = hci_class(event->parameters + HCI_EXTENDED_CLASS);
	device->has_class = 1;
	device->rssi = (int)(int8_t)event->parameters[HCI_EXTENDED_RSSI];
	device->has_rssi = 1;

	/* Its name and the rest, from the extended inquiry response. */
	btd_device_data(device, event->parameters + HCI_EXTENDED_HEAD, event->length - HCI_EXTENDED_HEAD);

	/* Succeeded: an extended inquiry result. */
	return 1;
}

/* Takes LE Advertising Report: whole reports one after another, each with its data and RSSI. */
static int
hci_le_reports(
	struct btd_devices *devices,
	const struct btd_event *event)
{
	struct btd_device *device;
	const uint8_t *report;
	size_t offset;
	size_t data_length;
	unsigned count;
	unsigned index;
	unsigned type;
	int rssi;

	/* Only the advertising report of the LE meta events. */
	if (event->length < 1U)
		return -1;
	if (event->parameters[0] != BTD_LE_ADVERTISING_REPORT)
		return 0;

	/* The number of reports. */
	if (event->length < 2U)
		return -1;
	count = event->parameters[1];

	/* Each report: its head, its data and its RSSI must be there. */
	offset = 2U;
	for (index = 0U; index < count; index++) {
		/* The head. */
		if (offset + HCI_REPORT_HEAD > event->length)
			return -1;
		report = event->parameters + offset;
		data_length = report[8];

		/* The data and the RSSI after it. */
		if (offset + HCI_REPORT_HEAD + data_length + 1U > event->length)
			return -1;

		/* Public (0, and 2 for a resolved public identity) or random (1, 3). */
		type = BTD_ADDRESS_LE_PUBLIC;
		if ((report[1] & 1U) != 0U)
			type = BTD_ADDRESS_LE_RANDOM;

		/* The device, its data and its RSSI. */
		device = hci_device(devices, report + 2U, type);
		if (device != NULL) {
			btd_device_data(device, report + HCI_REPORT_HEAD, data_length);
			rssi = (int)(int8_t)report[HCI_REPORT_HEAD + data_length];
			if (rssi != HCI_RSSI_NONE) {
				device->rssi = rssi;
				device->has_rssi = 1;
			}
		}

		/* The next report. */
		offset += HCI_REPORT_HEAD + data_length + 1U;
	}

	/* Succeeded: an advertising report. */
	return 1;
}

/* Finds a device by its address and kind, adding it when there is room; NULL when the table is full. */
static struct btd_device *
hci_device(
	struct btd_devices *devices,
	const uint8_t *address,
	unsigned type)
{
	struct btd_device *device;
	unsigned index;
	int differs;

	/* A device seen before. */
	for (index = 0U; index < devices->count; index++) {
		device = &devices->entries[index];
		if (device->type != type)
			continue;
		differs = memcmp(device->address, address, BTD_ADDRESS_BYTES);
		if (differs == 0)
			return device;
	}

	/* A full table counts the device it could not keep. */
	if (devices->count >= BTD_DEVICES_MAX) {
		devices->dropped++;
		return NULL;
	}

	/* A new device. */
	device = &devices->entries[devices->count];
	memset(device, 0, sizeof(*device));
	memcpy(device->address, address, BTD_ADDRESS_BYTES);
	device->type = type;
	devices->count++;

	/* Succeeded: the device's entry. */
	return device;
}

/* Keeps a name: a complete name always, a shortened one only while no complete one was seen. */
static void
hci_name(
	struct btd_device *device,
	const uint8_t *value,
	size_t length,
	int complete)
{
	size_t used;

	/* A shortened name does not replace a complete one. */
	if (!complete && device->name_complete)
		return;

	/* The bytes up to a NUL, as many as the name holds. */
	used = 0U;
	while (used < length && used + 1U < BTD_NAME_MAX && value[used] != 0U) {
		device->name[used] = (char)value[used];
		used++;
	}

	/* The name, ended. */
	device->name[used] = '\0';

	/* Whether it is the whole name. */
	if (complete)
		device->name_complete = 1;
}

/* Reads a class of device: three bytes, least significant first. */
static uint32_t
hci_class(
	const uint8_t *bytes)
{
	uint32_t class_of_device;

	/* The three bytes. */
	class_of_device = (uint32_t)bytes[0];
	class_of_device |= (uint32_t)bytes[1] << 8;
	class_of_device |= (uint32_t)bytes[2] << 16;

	/* Succeeded: the class. */
	return class_of_device;
}

/*
 * Tells how many bytes from here stay as they are on a line: 1 for
 * printable ASCII other than the quote and the backslash, the length of a
 * well-formed UTF-8 sequence of a character from U+00A0 (no C1 control, no
 * overlong form, no surrogate, nothing past U+10FFFF), and 0 for a byte to
 * escape.
 */
static size_t
hci_plain_run(
	const unsigned char *byte)
{
	uint32_t character;
	size_t length;
	size_t index;

	/* ASCII: printable, but not the quote nor the backslash. */
	if (byte[0] < 0x80U) {
		if (byte[0] < 0x20U || byte[0] == 0x7fU || byte[0] == '"' || byte[0] == '\\')
			return 0U;
		return 1U;
	}

	/* The sequence's length from its lead byte, and the lead's bits. */
	if ((byte[0] & 0xe0U) == 0xc0U) {
		length = 2U;
		character = byte[0] & 0x1fU;
	} else if ((byte[0] & 0xf0U) == 0xe0U) {
		length = 3U;
		character = byte[0] & 0x0fU;
	} else if ((byte[0] & 0xf8U) == 0xf0U) {
		length = 4U;
		character = byte[0] & 0x07U;
	} else {
		/* A continuation byte, or a lead byte UTF-8 does not have. */
		return 0U;
	}

	/* Each continuation byte (the NUL that ends the text is not one). */
	for (index = 1U; index < length; index++) {
		if ((byte[index] & 0xc0U) != 0x80U)
			return 0U;
		character = (character << 6) | (byte[index] & 0x3fU);
	}

	/* No overlong form: each length has its smallest character. */
	if (length == 2U && character < 0x80U)
		return 0U;
	if (length == 3U && character < 0x800U)
		return 0U;
	if (length == 4U && character < 0x10000U)
		return 0U;

	/* No C1 control, no surrogate, nothing past Unicode's end. */
	if (character < 0xa0U)
		return 0U;
	if (character >= 0xd800U && character <= 0xdfffU)
		return 0U;
	if (character > 0x10ffffU)
		return 0U;

	/* Succeeded: the whole sequence stays. */
	return length;
}
