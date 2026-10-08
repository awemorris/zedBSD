/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's HID over GATT client (ws143-p005 i03, see hog.h).  The
 * UUIDs are the Bluetooth SIG's assigned numbers (GATT's declarations
 * 0x2800 and 0x2803, the HID service 0x1812 and its characteristics, the
 * Battery and Device Information services); the values are to be checked
 * against the assigned numbers' document (phase005 section 8).
 */

#include "userland/base/bluetoothd/hog.h"

#include <errno.h>
#include <string.h>

/* Where the discovery is. */
#define HOG_IDLE		0U
#define HOG_MTU			1U
#define HOG_SERVICES		2U
#define HOG_CHARACTERISTICS	3U
#define HOG_DESCRIPTORS		4U
#define HOG_REFERENCES		5U
#define HOG_MAP			6U
#define HOG_PNP			7U
#define HOG_SETUP		8U
#define HOG_SUBSCRIBE		9U
#define HOG_BATTERY		10U
#define HOG_BATTERY_SUBSCRIBE	11U
#define HOG_OPEN		12U
#define HOG_FAILED		13U

/* GATT's declarations and descriptors. */
#define HOG_PRIMARY		0x2800U
#define HOG_CHARACTERISTIC	0x2803U
#define HOG_REPORT_REFERENCE	0x2908U
#define HOG_CCC			0x2902U

/* The services. */
#define HOG_SERVICE_HID		0x1812U
#define HOG_SERVICE_BATTERY	0x180fU
#define HOG_SERVICE_INFO	0x180aU
#define HOG_SERVICE_GATT	0x1801U

/* The characteristics. */
#define HOG_REPORT_MAP		0x2a4bU
#define HOG_REPORT		0x2a4dU
#define HOG_PROTOCOL_MODE	0x2a4eU
#define HOG_BATTERY_LEVEL	0x2a19U
#define HOG_PNP_ID		0x2a50U
#define HOG_SERVICE_CHANGED	0x2a05U

/* Which service's range a characteristic is of (the order they are looked through in). */
#define HOG_RANGE_HID		0U
#define HOG_RANGE_BATTERY	1U
#define HOG_RANGE_INFO		2U
#define HOG_RANGE_GATT		3U
#define HOG_RANGES		4U

/* A Report's types, and the report protocol mode. */
#define HOG_TYPE_INPUT		1U
#define HOG_PROTOCOL_REPORT	0x01U

/* The last handle. */
#define HOG_HANDLE_LAST		0xffffU

static unsigned hog_request(struct btd_hog *hog, size_t length, uint64_t now);
static unsigned hog_fail(struct btd_hog *hog, const char *why);
static unsigned hog_response(struct btd_hog *hog, const struct btd_att_pdu *parsed, uint64_t now);
static unsigned hog_error(struct btd_hog *hog, const struct btd_att_pdu *parsed, uint64_t now);
static unsigned hog_notification(struct btd_hog *hog, const struct btd_att_pdu *parsed);
static unsigned hog_services(struct btd_hog *hog, uint64_t now);
static unsigned hog_services_answer(struct btd_hog *hog, const struct btd_att_pdu *parsed, uint64_t now);
static unsigned hog_characteristics(struct btd_hog *hog, uint64_t now);
static unsigned hog_characteristics_answer(struct btd_hog *hog, const struct btd_att_pdu *parsed, uint64_t now);
static unsigned hog_characteristics_done(struct btd_hog *hog, uint64_t now);
static unsigned hog_descriptors(struct btd_hog *hog, uint64_t now);
static unsigned hog_descriptors_answer(struct btd_hog *hog, const struct btd_att_pdu *parsed, uint64_t now);
static unsigned hog_references(struct btd_hog *hog, uint64_t now);
static unsigned hog_map(struct btd_hog *hog, uint64_t now);
static unsigned hog_map_answer(struct btd_hog *hog, const struct btd_att_pdu *parsed, uint64_t now);
static unsigned hog_pnp(struct btd_hog *hog, uint64_t now);
static unsigned hog_setup(struct btd_hog *hog);
static unsigned hog_subscribe(struct btd_hog *hog, uint64_t now);
static unsigned hog_battery(struct btd_hog *hog, uint64_t now);
static unsigned hog_open(struct btd_hog *hog);
static int hog_range(const struct btd_hog *hog, unsigned range, uint16_t *start, uint16_t *end);
static uint16_t hog_descriptor_end(const struct btd_hog *hog, uint16_t value);
static int hog_descriptor_item(const struct btd_hog *hog, unsigned item, uint16_t *value);
static struct btd_hog_report *hog_report_of(struct btd_hog *hog, uint16_t handle);
static int hog_format(struct btd_hog *hog, const struct btd_hog_report *report, const uint8_t *value, size_t length);
static uint16_t hog_le16(const uint8_t *bytes);

/*
 * Prepares a discovery: nothing found, the default MTU, the battery's
 * level not known.
 */
void
btd_hog_init(
	struct btd_hog *hog)
{
	/* Nothing yet. */
	memset(hog, 0, sizeof(*hog));
	hog->state = HOG_IDLE;
	hog->mtu = BTD_ATT_MTU_DEFAULT;
	hog->battery = -1;
}

/*
 * Starts the discovery on an encrypted connection: Exchange MTU first.
 * Returns the actions (BTD_HOG_SEND).
 */
unsigned
btd_hog_start(
	struct btd_hog *hog,
	uint64_t now)
{
	size_t length;

	/* The whole discovery's time from now. */
	hog->discovery_deadline = now + BTD_HOG_DISCOVERY_MS;

	/* Succeeded: Exchange MTU Request with bluetoothd's MTU. */
	hog->state = HOG_MTU;
	length = btd_att_build_mtu(hog->out, sizeof(hog->out), BTD_ATT_MTU_REQUEST, BTD_ATT_MTU);
	return hog_request(hog, length, now);
}

/*
 * Takes a PDU of the device that is not a request to bluetoothd (the
 * caller's server answers those): a response, an error, a notification,
 * an indication.  Returns the actions.
 */
unsigned
btd_hog_input(
	struct btd_hog *hog,
	const uint8_t *pdu,
	size_t length,
	uint64_t now)
{
	struct btd_att_pdu parsed;
	unsigned actions;
	int error;

	/* A PDU of a known form. */
	error = btd_att_parse(pdu, length, &parsed);
	if (error != 0)
		return 0U;

	/* A notification goes to the reports. */
	if (parsed.opcode == BTD_ATT_NOTIFICATION) {
		actions = hog_notification(hog, &parsed);
		return actions;
	}

	/* An indication is confirmed (a changed service ends the connection, its discovery to do again). */
	if (parsed.opcode == BTD_ATT_INDICATION) {
		actions = hog_notification(hog, &parsed);
		hog->out_length = btd_att_build_confirmation(hog->out, sizeof(hog->out));
		actions |= BTD_HOG_SEND;
		if (hog->service_changed != 0U && parsed.handle == hog->service_changed)
			actions |= hog_fail(hog, "changed");
		return actions;
	}

	/* A response is to the request out, or comes for nothing. */
	if (!hog->waiting)
		return 0U;
	hog->waiting = 0;

	/* An error, or the response. */
	if (parsed.opcode == BTD_ATT_ERROR) {
		actions = hog_error(hog, &parsed, now);
		return actions;
	}

	/* Succeeded: the response's meaning. */
	actions = hog_response(hog, &parsed, now);
	return actions;
}

/*
 * Goes on once the caller made the input device (after BTD_HOG_SETUP):
 * the notifications turned on, the battery read.  Returns the actions.
 */
unsigned
btd_hog_resume(
	struct btd_hog *hog,
	uint64_t now)
{
	unsigned actions;

	/* Only after the setup. */
	if (hog->state != HOG_SETUP)
		return 0U;

	/* Succeeded: the first notification turned on. */
	hog->state = HOG_SUBSCRIBE;
	hog->cursor = 0U;
	actions = hog_subscribe(hog, now);
	return actions;
}

/*
 * Ends a discovery whose request or whole time ran out.  Returns the
 * actions (BTD_HOG_FAILED with why "timeout"), or 0.
 */
unsigned
btd_hog_tick(
	struct btd_hog *hog,
	uint64_t now)
{
	unsigned actions;

	/* Only while discovering. */
	if (hog->state == HOG_IDLE || hog->state == HOG_OPEN || hog->state == HOG_FAILED)
		return 0U;

	/* A request without its response, or the whole discovery too long. */
	if (hog->waiting && now >= hog->request_deadline) {
		actions = hog_fail(hog, "timeout");
		return actions;
	} else if (hog->discovery_deadline != 0U && now >= hog->discovery_deadline) {
		actions = hog_fail(hog, "timeout");
		return actions;
	}

	/* Succeeded: nothing ran out. */
	return 0U;
}

/*
 * Gives the earliest deadline of the discovery, or 0.
 */
uint64_t
btd_hog_deadline(
	const struct btd_hog *hog)
{
	uint64_t earliest;

	/* None unless discovering. */
	if (hog->state == HOG_IDLE || hog->state == HOG_OPEN || hog->state == HOG_FAILED)
		return 0U;

	/* The request's and the whole one's. */
	earliest = hog->discovery_deadline;
	if (hog->waiting && (earliest == 0U || hog->request_deadline < earliest))
		earliest = hog->request_deadline;

	/* Succeeded: the earliest. */
	return earliest;
}

/*
 * Takes the oldest notification that came before the device was open, as
 * a report to pass on (hog->report).  Returns 1 with a report, 0 when
 * none waits.
 */
int
btd_hog_next(
	struct btd_hog *hog)
{
	struct btd_hog_waiting oldest;
	struct btd_hog_report *report;
	int formatted;

	/* Each waiting notification, oldest first, until one is a report. */
	while (hog->queued != 0U) {
		oldest = hog->queue[0];
		memmove(&hog->queue[0], &hog->queue[1], (hog->queued - 1U) * sizeof(hog->queue[0]));
		hog->queued--;

		/* The battery's level. */
		if (hog->battery_level != 0U && oldest.handle == hog->battery_level) {
			if (oldest.length >= 1U)
				hog->battery = oldest.value[0];
			continue;
		}

		/* A report's, else of nothing known. */
		report = hog_report_of(hog, oldest.handle);
		if (report == NULL) {
			hog->unknown++;
			continue;
		}

		/* Succeeded: one report. */
		formatted = hog_format(hog, report, oldest.value, oldest.length);
		if (formatted)
			return 1;
	}

	/* None waits. */
	return 0;
}

/*
 * Follows the kernel's reading of the report map (INPUT_BRIDGE_GET_DEVICE's
 * flags, review M8) when it differs from the Report References'.
 */
void
btd_hog_set_ids(
	struct btd_hog *hog,
	int uses_ids)
{
	/* The kernel's word is the one the input device keeps. */
	hog->uses_ids = uses_ids;
}

/* Marks the request in out as sent, its response awaited; returns BTD_HOG_SEND (or the failure of a request that did not fit). */
static unsigned
hog_request(
	struct btd_hog *hog,
	size_t length,
	uint64_t now)
{
	unsigned actions;

	/* A request that did not fit is the program's error. */
	if (length == 0U) {
		actions = hog_fail(hog, "protocol");
		return actions;
	}

	/* Succeeded: one request out at a time (ATT's rule). */
	hog->out_length = length;
	hog->request = hog->out[0];
	hog->waiting = 1;
	hog->request_deadline = now + BTD_HOG_REQUEST_MS;
	return BTD_HOG_SEND;
}

/* Ends the discovery with why; returns BTD_HOG_FAILED. */
static unsigned
hog_fail(
	struct btd_hog *hog,
	const char *why)
{
	/* Nothing more is asked. */
	hog->state = HOG_FAILED;
	hog->waiting = 0;
	hog->why = why;

	/* Failed. */
	return BTD_HOG_FAILED;
}

/* Takes the response to the request out, by the state. */
static unsigned
hog_response(
	struct btd_hog *hog,
	const struct btd_att_pdu *parsed,
	uint64_t now)
{
	unsigned actions;
	uint16_t mtu;

	/* Each state's response. */
	actions = 0U;
	switch (hog->state) {
	case HOG_MTU:
		/* The smaller MTU, at least the default. */
		if (parsed->opcode != BTD_ATT_MTU_RESPONSE) {
			actions = hog_fail(hog, "protocol");
			return actions;
		}

		/* The smaller of the two. */
		mtu = parsed->mtu;
		if (mtu > BTD_ATT_MTU)
			mtu = BTD_ATT_MTU;
		if (mtu < BTD_ATT_MTU_DEFAULT)
			mtu = BTD_ATT_MTU_DEFAULT;
		hog->mtu = mtu;
		actions = hog_services(hog, now);
		break;
	case HOG_SERVICES:
		actions = hog_services_answer(hog, parsed, now);
		break;
	case HOG_CHARACTERISTICS:
		actions = hog_characteristics_answer(hog, parsed, now);
		break;
	case HOG_DESCRIPTORS:
		actions = hog_descriptors_answer(hog, parsed, now);
		break;
	case HOG_REFERENCES:
		/* The report's ID and type. */
		if (parsed->opcode == BTD_ATT_READ_RESPONSE && parsed->length >= 2U) {
			hog->reports[hog->cursor].id = parsed->value[0];
			hog->reports[hog->cursor].type = parsed->value[1];
		}

		/* The next reference. */
		hog->cursor++;
		actions = hog_references(hog, now);
		break;
	case HOG_MAP:
		actions = hog_map_answer(hog, parsed, now);
		break;
	case HOG_PNP:
		/* Vendor ID Source, Vendor ID, Product ID, Product Version. */
		if (parsed->opcode == BTD_ATT_READ_RESPONSE && parsed->length >= 7U) {
			hog->vendor = hog_le16(parsed->value + 1);
			hog->product = hog_le16(parsed->value + 3);
			hog->version = hog_le16(parsed->value + 5);
		}

		/* The caller's turn. */
		actions = hog_setup(hog);
		break;
	case HOG_SUBSCRIBE:
		hog->cursor++;
		actions = hog_subscribe(hog, now);
		break;
	case HOG_BATTERY:
		/* The level, then its notifications. */
		if (parsed->opcode == BTD_ATT_READ_RESPONSE && parsed->length >= 1U)
			hog->battery = parsed->value[0];
		actions = hog_battery(hog, now);
		break;
	case HOG_BATTERY_SUBSCRIBE:
		actions = hog_open(hog);
		break;
	default:
		break;
	}

	/* Succeeded: the next step's actions. */
	return actions;
}

/*
 * Takes an Error Response: a security error ends the connection (the bond
 * is short of the device's needs); Attribute Not Found ends a search; the
 * optional steps go on; any other ends it.
 */
static unsigned
hog_error(
	struct btd_hog *hog,
	const struct btd_att_pdu *parsed,
	uint64_t now)
{
	unsigned actions;

	/* The bond is not enough for the device (Settings asks to pair again). */
	if (parsed->error == BTD_ATT_INSUFFICIENT_AUTHENTICATION ||
	    parsed->error == BTD_ATT_INSUFFICIENT_ENCRYPTION ||
	    parsed->error == BTD_ATT_INSUFFICIENT_KEY_SIZE) {
		actions = hog_fail(hog, "security");
		return actions;
	}

	/* Each state's way on. */
	switch (hog->state) {
	case HOG_MTU:
		/* No exchange: the default MTU. */
		actions = hog_services(hog, now);
		break;
	case HOG_SERVICES:
		/* The end of the services. */
		if (parsed->error != BTD_ATT_NOT_FOUND) {
			actions = hog_fail(hog, "protocol");
			return actions;
		}

		/* The characteristics next. */
		hog->next = 0U;
		actions = hog_characteristics(hog, now);
		break;
	case HOG_CHARACTERISTICS:
		/* The end of a range's characteristics. */
		if (parsed->error != BTD_ATT_NOT_FOUND) {
			actions = hog_fail(hog, "protocol");
			return actions;
		}

		/* The next range. */
		hog->range++;
		hog->next = 0U;
		actions = hog_characteristics(hog, now);
		break;
	case HOG_DESCRIPTORS:
		/* The end of an item's descriptors. */
		hog->cursor++;
		hog->next = 0U;
		actions = hog_descriptors(hog, now);
		break;
	case HOG_REFERENCES:
		/* The report keeps its defaults. */
		hog->cursor++;
		actions = hog_references(hog, now);
		break;
	case HOG_MAP:
		/* A map that is a whole number of reads ends with one of these (review S3). */
		if ((parsed->error == BTD_ATT_INVALID_OFFSET || parsed->error == BTD_ATT_NOT_LONG) && hog->map_size != 0U) {
			actions = hog_pnp(hog, now);
			return actions;
		}

		/* Any other: no map. */
		actions = hog_fail(hog, "descriptor");
		break;
	case HOG_PNP:
		/* No numbers. */
		actions = hog_setup(hog);
		break;
	case HOG_SUBSCRIBE:
		/* That report stays silent; the next. */
		hog->cursor++;
		actions = hog_subscribe(hog, now);
		break;
	case HOG_BATTERY:
		actions = hog_battery(hog, now);
		break;
	case HOG_BATTERY_SUBSCRIBE:
		actions = hog_open(hog);
		break;
	default:
		actions = 0U;
		break;
	}

	/* Succeeded: the next step's actions. */
	return actions;
}

/*
 * Takes a notification (or an indication's value): the battery's level, a
 * report passed on when open, or kept until the device is (review S2).
 */
static unsigned
hog_notification(
	struct btd_hog *hog,
	const struct btd_att_pdu *parsed)
{
	struct btd_hog_waiting *kept;
	struct btd_hog_report *report;
	int formatted;

	/* The battery's level. */
	if (hog->battery_level != 0U && parsed->handle == hog->battery_level) {
		if (parsed->length >= 1U)
			hog->battery = parsed->value[0];
		return 0U;
	}

	/* Before the device is open: kept, while there is room. */
	if (hog->state != HOG_OPEN) {
		if (hog->queued >= BTD_HOG_QUEUE || parsed->length > BTD_HOG_QUEUE_BYTES) {
			hog->dropped++;
			return 0U;
		}

		/* Kept in order. */
		kept = &hog->queue[hog->queued];
		kept->handle = parsed->handle;
		kept->length = parsed->length;
		memcpy(kept->value, parsed->value, parsed->length);
		hog->queued++;
		return 0U;
	}

	/* A report's, else of nothing known. */
	report = hog_report_of(hog, parsed->handle);
	if (report == NULL) {
		hog->unknown++;
		return 0U;
	}

	/* Succeeded: passed on. */
	formatted = hog_format(hog, report, parsed->value, parsed->length);
	if (!formatted)
		return 0U;
	return BTD_HOG_REPORT;
}

/* Asks for the next primary services (Read By Group Type of 0x2800 from hog->next). */
static unsigned
hog_services(
	struct btd_hog *hog,
	uint64_t now)
{
	size_t length;
	unsigned actions;

	/* From the first handle at first. */
	if (hog->state != HOG_SERVICES) {
		hog->state = HOG_SERVICES;
		hog->next = 0x0001U;
	}

	/* Succeeded: the request. */
	length = btd_att_build_range(hog->out, sizeof(hog->out), BTD_ATT_READ_GROUP_REQUEST, hog->next, HOG_HANDLE_LAST, HOG_PRIMARY);
	actions = hog_request(hog, length, now);
	return actions;
}

/* Takes the primary services found (each its range and 16-bit UUID), and asks for more or goes on. */
static unsigned
hog_services_answer(
	struct btd_hog *hog,
	const struct btd_att_pdu *parsed,
	uint64_t now)
{
	const uint8_t *element;
	unsigned index;
	uint16_t start;
	uint16_t end;
	uint16_t uuid;
	unsigned actions;

	/* A list of services. */
	if (parsed->opcode != BTD_ATT_READ_GROUP_RESPONSE || parsed->count == 0U) {
		actions = hog_fail(hog, "protocol");
		return actions;
	}

	/* Each service: the first HID one and the others that matter are kept (a 128-bit UUID is none of them). */
	end = 0U;
	for (index = 0U; index < parsed->count; index++) {
		element = parsed->list + index * parsed->element;
		start = hog_le16(element);
		end = hog_le16(element + 2);
		if (end < start || start < hog->next) {
			actions = hog_fail(hog, "protocol");
			return actions;
		}

		/* Its class (a 128-bit UUID is none that matters), kept when it matters. */
		uuid = 0U;
		if (parsed->element == 6U)
			uuid = hog_le16(element + 4);
		if (uuid == HOG_SERVICE_HID && hog->hid_start != 0U) {
			hog->more_hid++;
		} else if (uuid == HOG_SERVICE_HID) {
			hog->hid_start = start;
			hog->hid_end = end;
		} else if (uuid == HOG_SERVICE_BATTERY && hog->battery_start == 0U) {
			hog->battery_start = start;
			hog->battery_end = end;
		} else if (uuid == HOG_SERVICE_INFO && hog->info_start == 0U) {
			hog->info_start = start;
			hog->info_end = end;
		} else if (uuid == HOG_SERVICE_GATT && hog->gatt_start == 0U) {
			hog->gatt_start = start;
			hog->gatt_end = end;
		}
	}

	/* The last handle ends the search; else the next. */
	if (end == HOG_HANDLE_LAST) {
		hog->next = 0U;
		actions = hog_characteristics(hog, now);
		return actions;
	}

	/* Succeeded: more to look for. */
	hog->next = (uint16_t)(end + 1U);
	actions = hog_services(hog, now);
	return actions;
}

/*
 * Asks for the characteristics of the range being looked through (Read
 * By Type of 0x2803), going on to the next range when one is done or
 * absent; after the last, takes what was found.
 */
static unsigned
hog_characteristics(
	struct btd_hog *hog,
	uint64_t now)
{
	uint16_t start;
	uint16_t end;
	size_t length;
	unsigned actions;
	int present;

	/* A HID service at all. */
	if (hog->state != HOG_CHARACTERISTICS) {
		if (hog->hid_start == 0U) {
			actions = hog_fail(hog, "no-hid");
			return actions;
		}

		/* The HID service's first. */
		hog->state = HOG_CHARACTERISTICS;
		hog->range = HOG_RANGE_HID;
		hog->next = 0U;
	}

	/* The next range that is there and not done. */
	for (;;) {
		if (hog->range >= HOG_RANGES) {
			actions = hog_characteristics_done(hog, now);
			return actions;
		}

		/* This range, from where it is (its start at first). */
		present = hog_range(hog, hog->range, &start, &end);
		if (hog->next == 0U)
			hog->next = start;
		if (present && hog->next <= end && hog->next >= start)
			break;
		hog->range++;
		hog->next = 0U;
	}

	/* Succeeded: the request. */
	length = btd_att_build_range(hog->out, sizeof(hog->out), BTD_ATT_READ_BY_TYPE_REQUEST, hog->next, end, HOG_CHARACTERISTIC);
	actions = hog_request(hog, length, now);
	return actions;
}

/* Takes the characteristics found (declaration, properties, value handle, UUID), and asks for more. */
static unsigned
hog_characteristics_answer(
	struct btd_hog *hog,
	const struct btd_att_pdu *parsed,
	uint64_t now)
{
	struct btd_hog_characteristic *characteristic;
	const uint8_t *element;
	unsigned index;
	uint16_t declaration;
	unsigned actions;

	/* A list of declarations. */
	if (parsed->opcode != BTD_ATT_READ_BY_TYPE_RESPONSE || parsed->count == 0U) {
		actions = hog_fail(hog, "protocol");
		return actions;
	}

	/* Of a declaration's length (a 16-bit or a 128-bit UUID). */
	if (parsed->element != 7U && parsed->element != 21U) {
		actions = hog_fail(hog, "protocol");
		return actions;
	}

	/* Each one, within the limits (more are counted and left, review S3). */
	declaration = hog->next;
	for (index = 0U; index < parsed->count; index++) {
		element = parsed->list + index * parsed->element;
		declaration = hog_le16(element);
		if (declaration < hog->next) {
			actions = hog_fail(hog, "protocol");
			return actions;
		}

		/* Past the limits: left. */
		if (hog->characteristic_count >= BTD_HOG_CHARACTERISTICS)
			continue;
		if (hog->range == HOG_RANGE_HID && hog->hid_characteristics >= BTD_HOG_HID_CHARACTERISTICS)
			continue;
		characteristic = &hog->characteristics[hog->characteristic_count];
		characteristic->declaration = declaration;
		characteristic->properties = element[2];
		characteristic->value = hog_le16(element + 3);
		characteristic->uuid = 0U;
		if (parsed->element == 7U)
			characteristic->uuid = hog_le16(element + 5);
		characteristic->service = (uint8_t)hog->range;
		hog->characteristic_count++;
		if (hog->range == HOG_RANGE_HID)
			hog->hid_characteristics++;
	}

	/* Succeeded: the rest of the range from after the last. */
	hog->next = (uint16_t)(declaration + 1U);
	if (declaration == HOG_HANDLE_LAST)
		hog->range++;
	if (declaration == HOG_HANDLE_LAST)
		hog->next = 0U;
	actions = hog_characteristics(hog, now);
	return actions;
}

/* Picks the characteristics that matter, then looks for the Reports' and the battery's descriptors. */
static unsigned
hog_characteristics_done(
	struct btd_hog *hog,
	uint64_t now)
{
	const struct btd_hog_characteristic *characteristic;
	struct btd_hog_report *report;
	unsigned index;
	unsigned actions;

	/* Each one by its service and UUID. */
	for (index = 0U; index < hog->characteristic_count; index++) {
		characteristic = &hog->characteristics[index];
		if (characteristic->service == HOG_RANGE_HID && characteristic->uuid == HOG_REPORT_MAP && hog->report_map == 0U) {
			hog->report_map = characteristic->value;
		} else if (characteristic->service == HOG_RANGE_HID && characteristic->uuid == HOG_PROTOCOL_MODE) {
			hog->protocol_mode = characteristic->value;
		} else if (characteristic->service == HOG_RANGE_HID && characteristic->uuid == HOG_REPORT && hog->report_count < BTD_HOG_REPORTS) {
			report = &hog->reports[hog->report_count];
			memset(report, 0, sizeof(*report));
			report->value = characteristic->value;
			report->type = HOG_TYPE_INPUT;
			hog->report_count++;
		} else if (characteristic->service == HOG_RANGE_BATTERY && characteristic->uuid == HOG_BATTERY_LEVEL) {
			hog->battery_level = characteristic->value;
		} else if (characteristic->service == HOG_RANGE_INFO && characteristic->uuid == HOG_PNP_ID) {
			hog->pnp_id = characteristic->value;
		} else if (characteristic->service == HOG_RANGE_GATT && characteristic->uuid == HOG_SERVICE_CHANGED) {
			hog->service_changed = characteristic->value;
		}
	}

	/* A HID service without its map is none. */
	if (hog->report_map == 0U) {
		actions = hog_fail(hog, "no-hid");
		return actions;
	}

	/* Succeeded: the descriptors from the first item. */
	hog->state = HOG_DESCRIPTORS;
	hog->cursor = 0U;
	hog->next = 0U;
	actions = hog_descriptors(hog, now);
	return actions;
}

/*
 * Asks for the descriptors of the item being looked at (each Report, then
 * the Battery Level): Find Information from after its value to the next
 * declaration of its service.  After the last, reads the references.
 */
static unsigned
hog_descriptors(
	struct btd_hog *hog,
	uint64_t now)
{
	uint16_t value;
	uint16_t end;
	size_t length;
	unsigned actions;
	int present;

	/* The next item whose range holds a handle. */
	for (;;) {
		present = hog_descriptor_item(hog, hog->cursor, &value);
		if (!present) {
			hog->cursor = 0U;
			actions = hog_references(hog, now);
			return actions;
		}

		/* Its range, from where it is (after its value at first); an empty one is passed. */
		end = hog_descriptor_end(hog, value);
		if (hog->next == 0U)
			hog->next = (uint16_t)(value + 1U);
		if (value != HOG_HANDLE_LAST && hog->next <= end)
			break;
		hog->cursor++;
		hog->next = 0U;
	}

	/* Succeeded: the request. */
	length = btd_att_build_range(hog->out, sizeof(hog->out), BTD_ATT_FIND_INFO_REQUEST, hog->next, end, 0U);
	actions = hog_request(hog, length, now);
	return actions;
}

/* Takes the descriptors found (handle and 16-bit UUID): the Report Reference and the CCC are kept. */
static unsigned
hog_descriptors_answer(
	struct btd_hog *hog,
	const struct btd_att_pdu *parsed,
	uint64_t now)
{
	const uint8_t *element;
	unsigned index;
	uint16_t handle;
	uint16_t uuid;
	unsigned actions;

	/* A list of descriptors. */
	if (parsed->opcode != BTD_ATT_FIND_INFO_RESPONSE || parsed->count == 0U) {
		actions = hog_fail(hog, "protocol");
		return actions;
	}

	/* Each one: a 128-bit one is not a descriptor that matters. */
	handle = hog->next;
	for (index = 0U; index < parsed->count; index++) {
		element = parsed->list + index * parsed->element;
		handle = hog_le16(element);
		if (handle < hog->next) {
			actions = hog_fail(hog, "protocol");
			return actions;
		}

		/* Its UUID, and whose it is. */
		uuid = 0U;
		if (parsed->format == BTD_ATT_FORMAT_16)
			uuid = hog_le16(element + 2);
		if (hog->cursor < hog->report_count && uuid == HOG_REPORT_REFERENCE)
			hog->reports[hog->cursor].reference = handle;
		else if (hog->cursor < hog->report_count && uuid == HOG_CCC)
			hog->reports[hog->cursor].ccc = handle;
		else if (uuid == HOG_CCC)
			hog->battery_ccc = handle;
	}

	/* Succeeded: the rest of the item's range, or the next item. */
	hog->next = (uint16_t)(handle + 1U);
	if (handle == HOG_HANDLE_LAST)
		hog->cursor++;
	if (handle == HOG_HANDLE_LAST)
		hog->next = 0U;
	actions = hog_descriptors(hog, now);
	return actions;
}

/* Reads the next Report Reference; after the last, decides whether reports carry an ID and reads the map. */
static unsigned
hog_references(
	struct btd_hog *hog,
	uint64_t now)
{
	unsigned index;
	size_t length;
	unsigned actions;

	/* The next report with a reference. */
	hog->state = HOG_REFERENCES;
	while (hog->cursor < hog->report_count && hog->reports[hog->cursor].reference == 0U)
		hog->cursor++;

	/* One to read. */
	if (hog->cursor < hog->report_count) {
		length = btd_att_build_read(hog->out, sizeof(hog->out), hog->reports[hog->cursor].reference, 0U, 0);
		actions = hog_request(hog, length, now);
		return actions;
	}

	/* The map numbers its reports when an input report's ID is not 0 (phase005 Q12). */
	hog->uses_ids = 0;
	for (index = 0U; index < hog->report_count; index++) {
		if (hog->reports[index].type == HOG_TYPE_INPUT && hog->reports[index].id != 0U)
			hog->uses_ids = 1;
	}

	/* Succeeded: the map next. */
	actions = hog_map(hog, now);
	return actions;
}

/* Reads the report map (from the start, or on from what came with Read Blob). */
static unsigned
hog_map(
	struct btd_hog *hog,
	uint64_t now)
{
	size_t length;
	unsigned actions;

	/* Read, or Read Blob at the offset. */
	hog->state = HOG_MAP;
	if (hog->map_size == 0U) {
		length = btd_att_build_read(hog->out, sizeof(hog->out), hog->report_map, 0U, 0);
	} else {
		length = btd_att_build_read(hog->out, sizeof(hog->out), hog->report_map, (uint16_t)hog->map_size, 1);
	}

	/* Succeeded: the request. */
	actions = hog_request(hog, length, now);
	return actions;
}

/*
 * Takes a part of the map: a full one (MTU - 1 bytes) asks for more, a
 * shorter or empty one ends it.  A map past BTD_HOG_MAP_MAX is refused.
 */
static unsigned
hog_map_answer(
	struct btd_hog *hog,
	const struct btd_att_pdu *parsed,
	uint64_t now)
{
	unsigned actions;

	/* A value. */
	if (parsed->opcode != BTD_ATT_READ_RESPONSE && parsed->opcode != BTD_ATT_READ_BLOB_RESPONSE) {
		actions = hog_fail(hog, "protocol");
		return actions;
	}

	/* Added, within the limit. */
	if (parsed->length > BTD_HOG_MAP_MAX - hog->map_size) {
		actions = hog_fail(hog, "descriptor");
		return actions;
	}

	/* The part, at the end. */
	memcpy(hog->map + hog->map_size, parsed->value, parsed->length);
	hog->map_size += parsed->length;

	/* A full part asks for more. */
	if (parsed->length == (size_t)hog->mtu - 1U && hog->map_size < BTD_HOG_MAP_MAX) {
		actions = hog_map(hog, now);
		return actions;
	}

	/* An empty map is no map. */
	if (hog->map_size == 0U) {
		actions = hog_fail(hog, "descriptor");
		return actions;
	}

	/* Succeeded: the PnP numbers next. */
	actions = hog_pnp(hog, now);
	return actions;
}

/* Reads the PnP ID, or goes on without it. */
static unsigned
hog_pnp(
	struct btd_hog *hog,
	uint64_t now)
{
	size_t length;
	unsigned actions;

	/* None: the numbers stay 0. */
	hog->state = HOG_PNP;
	if (hog->pnp_id == 0U) {
		actions = hog_setup(hog);
		return actions;
	}

	/* Succeeded: the request. */
	length = btd_att_build_read(hog->out, sizeof(hog->out), hog->pnp_id, 0U, 0);
	actions = hog_request(hog, length, now);
	return actions;
}

/*
 * Ends the discovery's first part: the report protocol is asked for (a
 * Write Command, no answer) and the caller makes the input device.
 */
static unsigned
hog_setup(
	struct btd_hog *hog)
{
	static const uint8_t report_protocol[1] = { HOG_PROTOCOL_REPORT };
	unsigned actions;

	/* The caller's turn. */
	hog->state = HOG_SETUP;
	actions = BTD_HOG_SETUP;

	/* With the protocol's command when the device has the characteristic. */
	if (hog->protocol_mode != 0U) {
		hog->out_length = btd_att_build_write(hog->out, sizeof(hog->out), BTD_ATT_WRITE_COMMAND, hog->protocol_mode, report_protocol, 1U);
		if (hog->out_length != 0U)
			actions |= BTD_HOG_SEND;
	}

	/* Succeeded: the actions. */
	return actions;
}

/* Turns on the notifications of the next Input Report that has a CCC; after the last, the battery. */
static unsigned
hog_subscribe(
	struct btd_hog *hog,
	uint64_t now)
{
	static const uint8_t notify[2] = { 0x01U, 0x00U };
	const struct btd_hog_report *report;
	size_t length;
	unsigned actions;

	/* The next input report with a CCC. */
	while (hog->cursor < hog->report_count) {
		report = &hog->reports[hog->cursor];
		if (report->type == HOG_TYPE_INPUT && report->ccc != 0U)
			break;
		hog->cursor++;
	}

	/* One to write. */
	if (hog->cursor < hog->report_count) {
		length = btd_att_build_write(hog->out, sizeof(hog->out), BTD_ATT_WRITE_REQUEST, hog->reports[hog->cursor].ccc, notify, sizeof(notify));
		actions = hog_request(hog, length, now);
		return actions;
	}

	/* The battery next (read, then its notifications), or open. */
	if (hog->battery_level == 0U) {
		actions = hog_open(hog);
		return actions;
	}

	/* The level's read. */
	hog->state = HOG_BATTERY;
	length = btd_att_build_read(hog->out, sizeof(hog->out), hog->battery_level, 0U, 0);
	actions = hog_request(hog, length, now);
	return actions;
}

/* Turns on the battery's notifications after its level was read, or opens. */
static unsigned
hog_battery(
	struct btd_hog *hog,
	uint64_t now)
{
	static const uint8_t notify[2] = { 0x01U, 0x00U };
	size_t length;
	unsigned actions;

	/* No CCC: open. */
	if (hog->battery_ccc == 0U) {
		actions = hog_open(hog);
		return actions;
	}

	/* Succeeded: the request. */
	hog->state = HOG_BATTERY_SUBSCRIBE;
	length = btd_att_build_write(hog->out, sizeof(hog->out), BTD_ATT_WRITE_REQUEST, hog->battery_ccc, notify, sizeof(notify));
	actions = hog_request(hog, length, now);
	return actions;
}

/* Ends the discovery: open (the waiting notifications go on through btd_hog_next). */
static unsigned
hog_open(
	struct btd_hog *hog)
{
	/* Nothing more is asked or timed. */
	hog->state = HOG_OPEN;
	hog->waiting = 0;
	hog->discovery_deadline = 0U;

	/* Open. */
	return BTD_HOG_OPEN;
}

/* Gives a service's range by the order they are looked through in; returns whether the device has it. */
static int
hog_range(
	const struct btd_hog *hog,
	unsigned range,
	uint16_t *start,
	uint16_t *end)
{
	/* Each range. */
	*start = 0U;
	*end = 0U;
	switch (range) {
	case HOG_RANGE_HID:
		*start = hog->hid_start;
		*end = hog->hid_end;
		break;
	case HOG_RANGE_BATTERY:
		*start = hog->battery_start;
		*end = hog->battery_end;
		break;
	case HOG_RANGE_INFO:
		*start = hog->info_start;
		*end = hog->info_end;
		break;
	case HOG_RANGE_GATT:
		*start = hog->gatt_start;
		*end = hog->gatt_end;
		break;
	default:
		break;
	}

	/* Not there. */
	if (*start == 0U)
		return 0;

	/* Succeeded: there. */
	return 1;
}

/* Gives the last handle of a characteristic's descriptors: before the next declaration of its service, else its service's end. */
static uint16_t
hog_descriptor_end(
	const struct btd_hog *hog,
	uint16_t value)
{
	const struct btd_hog_characteristic *characteristic;
	uint16_t start;
	uint16_t end;
	unsigned service;
	unsigned index;

	/* The characteristic of that value. */
	service = HOG_RANGES;
	for (index = 0U; index < hog->characteristic_count; index++) {
		if (hog->characteristics[index].value == value)
			service = hog->characteristics[index].service;
	}

	/* Its service's range. */
	(void)hog_range(hog, service, &start, &end);

	/* The nearest declaration after it in its service. */
	for (index = 0U; index < hog->characteristic_count; index++) {
		characteristic = &hog->characteristics[index];
		if (characteristic->service == service && characteristic->declaration > value && characteristic->declaration - 1U < end)
			end = (uint16_t)(characteristic->declaration - 1U);
	}

	/* Succeeded: the end. */
	return end;
}

/* Gives the value handle of a descriptors' item (each Report, then the Battery Level); returns whether there is one. */
static int
hog_descriptor_item(
	const struct btd_hog *hog,
	unsigned item,
	uint16_t *value)
{
	/* A Report. */
	if (item < hog->report_count) {
		*value = hog->reports[item].value;
		return 1;
	}

	/* The Battery Level, last. */
	if (item == hog->report_count && hog->battery_level != 0U) {
		*value = hog->battery_level;
		return 1;
	}

	/* No more. */
	return 0;
}

/* Finds the Input Report of a value handle, or NULL. */
static struct btd_hog_report *
hog_report_of(
	struct btd_hog *hog,
	uint16_t handle)
{
	unsigned index;

	/* Each input report. */
	for (index = 0U; index < hog->report_count; index++) {
		if (hog->reports[index].value == handle && hog->reports[index].type == HOG_TYPE_INPUT)
			return &hog->reports[index];
	}

	/* None. */
	return NULL;
}

/*
 * Writes a report to pass on: its ID first when the map numbers its
 * reports (a notification carries none, design section 5.2 [F7]), then
 * the value.  Returns 1, or 0 for one too long for the bridge (counted).
 */
static int
hog_format(
	struct btd_hog *hog,
	const struct btd_hog_report *report,
	const uint8_t *value,
	size_t length)
{
	size_t at;

	/* The ID's byte, when there is one. */
	at = 0U;
	if (hog->uses_ids)
		at = 1U;
	if (length == 0U || length + at > BTD_HIDP_REPORT_MAX) {
		hog->oversize++;
		return 0;
	}

	/* Succeeded: the report. */
	if (at != 0U)
		hog->report[0] = report->id;
	memcpy(hog->report + at, value, length);
	hog->report_length = length + at;
	return 1;
}

/* Reads a 16-bit value least significant byte first. */
static uint16_t
hog_le16(
	const uint8_t *bytes)
{
	/* The two bytes. */
	return (uint16_t)(bytes[0] | (bytes[1] << 8));
}
