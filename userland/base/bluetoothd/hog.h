/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's HID over GATT client (ws143-p005 i03, plan/ws143/phase005/
 * phase.md section 4.6): the discovery of an LE HID device's attributes
 * over ATT, as a state machine without system calls.  The caller sends
 * each PDU it asks for on the ATT channel, feeds it every response,
 * notification and indication, makes the input device when it is told the
 * report map and the PnP numbers are there, and passes on the reports.
 *
 * The order: the MTU; the primary services (HID, Battery, Device
 * Information, GATT); their characteristics; the descriptors (Report
 * Reference, Client Characteristic Configuration) of each Report and of
 * the Battery Level; the Report References; the Report Map (with Read
 * Blob); the PnP ID; the report protocol (a Write Command); then, after
 * the input device is made, the notifications turned on (each Input
 * Report's, the battery's) and the battery's level read.  Notifications
 * that come before the device is open wait (32 at most, review S2) and go
 * on, in order, once it is.  One HID service is used; a second is counted
 * and left (phase005 §9.12's two devices are i03's residual work).
 */

#ifndef BLUETOOTHD_HOG_H
#define BLUETOOTHD_HOG_H

#include "userland/base/bluetoothd/att.h"
#include "userland/base/bluetoothd/hidp.h"

#include <stddef.h>
#include <stdint.h>

/* What an input asks of the caller: send out, make the input device, it is open, it failed (why), pass report on. */
#define BTD_HOG_SEND		0x01U
#define BTD_HOG_SETUP		0x02U
#define BTD_HOG_OPEN		0x04U
#define BTD_HOG_FAILED		0x08U
#define BTD_HOG_REPORT		0x10U

/* The limits: characteristics of the HID service and in all, Reports, the waiting notifications and their longest value. */
#define BTD_HOG_HID_CHARACTERISTICS	64U
#define BTD_HOG_CHARACTERISTICS		96U
#define BTD_HOG_REPORTS			32U
#define BTD_HOG_QUEUE			32U
#define BTD_HOG_QUEUE_BYTES		64U

/* The longest report map. */
#define BTD_HOG_MAP_MAX			4096U

/* How long a request (ATT's transaction) and the whole discovery may take (milliseconds). */
#define BTD_HOG_REQUEST_MS		30000U
#define BTD_HOG_DISCOVERY_MS		30000U

/* One characteristic: its declaration's and its value's handles, its 16-bit UUID (0 for a 128-bit one), its properties, its service. */
struct btd_hog_characteristic {
	uint16_t declaration;
	uint16_t value;
	uint16_t uuid;
	uint8_t properties;
	uint8_t service;
};

/* One Report: its value's handle, its Report Reference's and its CCC's (0: none), its report ID and type (1 input, 2 output, 3 feature). */
struct btd_hog_report {
	uint16_t value;
	uint16_t reference;
	uint16_t ccc;
	uint8_t id;
	uint8_t type;
};

/* One notification that came before the device was open. */
struct btd_hog_waiting {
	uint16_t handle;
	size_t length;
	uint8_t value[BTD_HOG_QUEUE_BYTES];
};

/*
 * The discovery of one LE HID device's connection, and what it found.
 * It lives in the HID host's device for the connection's life.
 */
struct btd_hog {
	unsigned state;
	uint16_t mtu;
	int waiting;
	uint8_t request;
	uint64_t request_deadline;
	uint64_t discovery_deadline;

	/* The services: the next handle to look from, each one's range (0: none), the HID services left. */
	uint16_t next;
	uint16_t hid_start;
	uint16_t hid_end;
	uint16_t battery_start;
	uint16_t battery_end;
	uint16_t info_start;
	uint16_t info_end;
	uint16_t gatt_start;
	uint16_t gatt_end;
	unsigned more_hid;

	/* The characteristics, the range being looked through, and those that matter. */
	struct btd_hog_characteristic characteristics[BTD_HOG_CHARACTERISTICS];
	unsigned characteristic_count;
	unsigned hid_characteristics;
	unsigned range;
	uint16_t report_map;
	uint16_t protocol_mode;
	uint16_t battery_level;
	uint16_t battery_ccc;
	uint16_t pnp_id;
	uint16_t service_changed;

	/* The Reports, the one being worked on, and whether the map numbers its reports. */
	struct btd_hog_report reports[BTD_HOG_REPORTS];
	unsigned report_count;
	unsigned cursor;
	int uses_ids;

	/* The report map and the PnP numbers. */
	uint8_t map[BTD_HOG_MAP_MAX];
	size_t map_size;
	uint16_t vendor;
	uint16_t product;
	uint16_t version;

	/* The battery's level (-1: not known), the notifications waiting, and those dropped or of no report. */
	int battery;
	struct btd_hog_waiting queue[BTD_HOG_QUEUE];
	unsigned queued;
	unsigned dropped;
	unsigned unknown;
	unsigned oversize;

	/* Why it failed, the PDU to send, and the report to pass on (its ID first when the map numbers them). */
	const char *why;
	uint8_t out[BTD_ATT_MTU];
	size_t out_length;
	uint8_t report[1U + BTD_HIDP_REPORT_MAX];
	size_t report_length;
};

void btd_hog_init(struct btd_hog *hog);
unsigned btd_hog_start(struct btd_hog *hog, uint64_t now);
unsigned btd_hog_input(struct btd_hog *hog, const uint8_t *pdu, size_t length, uint64_t now);
unsigned btd_hog_resume(struct btd_hog *hog, uint64_t now);
unsigned btd_hog_tick(struct btd_hog *hog, uint64_t now);
uint64_t btd_hog_deadline(const struct btd_hog *hog);
int btd_hog_next(struct btd_hog *hog);
void btd_hog_set_ids(struct btd_hog *hog, int uses_ids);

#endif
