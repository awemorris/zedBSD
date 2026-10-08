/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HID Profile's transaction header on the control and interrupt
 * channels (ws143-p005 i02, plan/ws143/phase005/phase.md section 4.4),
 * without system calls: one byte, the message type in the upper four bits
 * and its parameter in the lower four (HID 1.1.1 section 7.3, values to be
 * checked against it).
 */

#ifndef BLUETOOTHD_HIDP_H
#define BLUETOOTHD_HIDP_H

#include <stddef.h>
#include <stdint.h>

/* The message types. */
#define BTD_HIDP_HANDSHAKE		0x0U
#define BTD_HIDP_CONTROL		0x1U
#define BTD_HIDP_GET_REPORT		0x4U
#define BTD_HIDP_SET_REPORT		0x5U
#define BTD_HIDP_GET_PROTOCOL		0x6U
#define BTD_HIDP_SET_PROTOCOL		0x7U
#define BTD_HIDP_DATA			0xaU

/* HID_CONTROL's operations. */
#define BTD_HIDP_SUSPEND		0x3U
#define BTD_HIDP_EXIT_SUSPEND		0x4U
#define BTD_HIDP_VIRTUAL_CABLE_UNPLUG	0x5U

/* DATA's report types. */
#define BTD_HIDP_REPORT_OTHER		0x0U
#define BTD_HIDP_REPORT_INPUT		0x1U
#define BTD_HIDP_REPORT_OUTPUT		0x2U
#define BTD_HIDP_REPORT_FEATURE		0x3U

/* SET_PROTOCOL's protocols. */
#define BTD_HIDP_PROTOCOL_BOOT		0x0U
#define BTD_HIDP_PROTOCOL_REPORT	0x1U

/* HANDSHAKE's results. */
#define BTD_HIDP_SUCCESSFUL		0x0U
#define BTD_HIDP_NOT_READY		0x1U
#define BTD_HIDP_INVALID_REPORT_ID	0x2U
#define BTD_HIDP_UNSUPPORTED		0x3U
#define BTD_HIDP_INVALID_PARAMETER	0x4U
#define BTD_HIDP_UNKNOWN		0xeU
#define BTD_HIDP_FATAL			0xfU

/* The longest report bluetoothd passes on (INPUT_BRIDGE_REPORT_MAX, the report ID's byte included). */
#define BTD_HIDP_REPORT_MAX		512U

/* One message taken apart: its type, its parameter, and what follows the header (pointing into the frame). */
struct btd_hidp {
	unsigned type;
	unsigned parameter;
	const uint8_t *data;
	size_t length;
};

uint8_t btd_hidp_header(unsigned type, unsigned parameter);
int btd_hidp_parse(const uint8_t *frame, size_t length, struct btd_hidp *message);

#endif
