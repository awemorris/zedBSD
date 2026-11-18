/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pen state machine of a USB HID digitizer.
 */

#ifndef KERN_DRIVERS_HID_DIGITIZER_H
#define KERN_DRIVERS_HID_DIGITIZER_H

#include <drivers/generic/hid-report.h>

#include <stddef.h>
#include <stdint.h>

/*
 * The usages of the Digitizer page that the pen state machine combines into
 * a tool and a contact.  The report decoder returns each of them as a value
 * of type HID_REPORT_TYPE_DIGITIZER whose code is the usage itself.
 */
#define HID_DIGITIZER_USAGE_IN_RANGE		0x32U
#define HID_DIGITIZER_USAGE_INVERT		0x3cU
#define HID_DIGITIZER_USAGE_TIP_SWITCH		0x42U
#define HID_DIGITIZER_USAGE_BARREL_SWITCH	0x44U
#define HID_DIGITIZER_USAGE_ERASER		0x45U
#define HID_DIGITIZER_USAGE_SECONDARY_BARREL	0x5aU

/* The most events one report can turn into, including two SYN_REPORTs. */
#define HID_DIGITIZER_EVENT_MAX			48U

/*
 * What one pen device has already told its readers.
 *
 * One instance lives with each digitizer device from attach to detach.  The
 * zero value means that no tool is in range and nothing is held.
 */
struct hid_digitizer_state {
	uint16_t tool;
	uint8_t touch;
	uint8_t stylus;
	uint8_t stylus2;
	uint8_t reserved[3];
};

/*
 * One event the pen state machine asks the input layer to emit.
 */
struct hid_digitizer_event {
	uint16_t type;
	uint16_t code;
	int32_t value;
};

/*
 * The ordered events one digitizer report produced.
 *
 * The caller owns the storage; the translation fills it from the start.
 */
struct hid_digitizer_output {
	size_t event_count;
	struct hid_digitizer_event events[HID_DIGITIZER_EVENT_MAX];
};

void drv_hid_digitizer_reset(struct hid_digitizer_state *);
int drv_hid_digitizer_report_is_pen(const struct hid_report_input *);
int drv_hid_digitizer_translate(struct hid_digitizer_state *,
	const struct hid_report_input *, struct hid_digitizer_output *);

#endif
