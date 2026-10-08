/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's SDP client (ws143-p005 i02, plan/ws143/phase005/phase.md
 * section 4.3), without system calls: one ServiceSearchAttributeRequest
 * for a service class (HID 0x1124 or PnP Information 0x1200) asking for
 * every attribute, its response's fragments put together over the
 * continuation states, and the HID record's and the PnP record's
 * attributes read from what came.  SDP is big-endian, unlike HCI and
 * L2CAP.  Every data element's length is checked against what holds it.
 */

#ifndef BLUETOOTHD_SDP_H
#define BLUETOOTHD_SDP_H

#include <stddef.h>
#include <stdint.h>

/* The service classes bluetoothd looks for. */
#define BTD_SDP_UUID_HID	0x1124U
#define BTD_SDP_UUID_PNP	0x1200U

/* The HID channels' PSMs (HID 1.1.1 fixes them). */
#define BTD_SDP_PSM_SDP		0x0001U
#define BTD_SDP_PSM_CONTROL	0x0011U
#define BTD_SDP_PSM_INTERRUPT	0x0013U

/* The most of the attribute lists kept (fragments put together), and of a report descriptor. */
#define BTD_SDP_MAX		8192U
#define BTD_SDP_DESCRIPTOR_MAX	4096U

/* The MaximumAttributeByteCount asked for: header 5 + count 2 + 640 + continuation 17 fits the MTU 672 (review M4). */
#define BTD_SDP_BYTES_ASKED	0x0280U

/* The longest request bluetoothd builds. */
#define BTD_SDP_REQUEST_MAX	48U

/* What a response meant: send the next request, the lists are whole, or the query failed. */
#define BTD_SDP_MORE		1
#define BTD_SDP_DONE		2
#define BTD_SDP_FAILED		3

/* The longest service name kept. */
#define BTD_SDP_NAME_MAX	64U

/*
 * One query: the service class, the transaction of the request out, the
 * continuation state the next request carries, how often the same state
 * came back, the attribute lists put together, and why it failed.
 */
struct btd_sdp {
	uint16_t uuid;
	uint16_t transaction;
	uint8_t continuation[17];
	size_t continuation_length;
	unsigned repeats;
	size_t used;
	const char *why;
	uint8_t lists[BTD_SDP_MAX];
};

/*
 * What bluetoothd reads of a HID device's records: the HID record's flags,
 * country, subclass, name and report descriptor (pointing into the
 * query's lists), and the PnP record's vendor, product and version.
 */
struct btd_hid_record {
	int hid;
	int reconnect_initiate;
	int normally_connectable;
	int virtual_cable;
	int boot_device;
	uint8_t subclass;
	uint8_t country;
	char name[BTD_SDP_NAME_MAX];
	const uint8_t *descriptor;
	size_t descriptor_size;
	int pnp;
	uint16_t vendor_source;
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
};

void btd_sdp_init(struct btd_sdp *sdp, uint16_t uuid, uint16_t transaction);
int btd_sdp_request(struct btd_sdp *sdp, uint8_t *out, size_t size, size_t *length);
int btd_sdp_input(struct btd_sdp *sdp, const uint8_t *pdu, size_t length);
int btd_sdp_hid(const struct btd_sdp *sdp, struct btd_hid_record *record);
int btd_sdp_pnp(const struct btd_sdp *sdp, struct btd_hid_record *record);

#endif
