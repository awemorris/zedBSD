/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef KERN_DRIVERS_HID_REPORT_H
#define KERN_DRIVERS_HID_REPORT_H

#include "kern/input-capability.h"

#include <stddef.h>
#include <stdint.h>

#define HID_REPORT_DESCRIPTOR_SIZE_MAX 4096U
#define HID_REPORT_COLLECTION_DEPTH_MAX 16U
#define HID_REPORT_GLOBAL_DEPTH_MAX 16U
#define HID_REPORT_ID_COUNT_MAX 32U
#define HID_REPORT_FIELD_COUNT_MAX 256U
#define HID_REPORT_BITS_MAX 8192U
#define HID_REPORT_VALUE_COUNT_MAX HID_REPORT_FIELD_COUNT_MAX

/*
 * The value type of a pen switch of the Digitizer page (In Range, Invert,
 * Tip Switch, Eraser, Barrel Switch).  It lies outside the evdev event types:
 * such a value is reported every time, zero or not, and only the pen state
 * machine turns it into EV_KEY events.
 */
#define HID_REPORT_TYPE_DIGITIZER 0x8000U

/*
 * The value type of a finger field of a touch screen (Tip Switch,
 * Confidence, Contact Identifier, X, Y) and of its Contact Count.  Like a pen
 * switch it lies outside the evdev event types: its code names the finger and
 * the item (include/drivers/generic/hid-touch.h), every such field is reported
 * each time, and only the touch state machine turns the values into
 * multitouch events.
 */
#define HID_REPORT_TYPE_TOUCH 0x8001U

/* The layout has no pen collection. */
#define HID_REPORT_PEN_NONE 0U
/* The layout has a pen on a separate tablet (Digitizer collection). */
#define HID_REPORT_PEN_TABLET 1U
/* The layout has a pen on a display (Pen collection). */
#define HID_REPORT_PEN_DISPLAY 2U

struct hid_report_layout;

/*
 * EV_KEY values form the set of keys currently held in this report.  EV_ABS
 * values are current coordinates, while EV_REL values are report deltas.
 * A set keyboard_error means that the HID keyboard array contained one of
 * ErrorRollOver, POSTFail, or ErrorUndefined.  In that case no keyboard
 * EV_KEY values are present and a consumer must preserve its prior keyboard
 * state rather than treating the result as an empty held-key snapshot.
 * The decoder owns no storage referenced by this result.
 */
struct hid_report_value {
	uint16_t type;
	uint16_t code;
	int32_t value;
};

struct hid_report_input {
	uint8_t report_id;
	uint8_t keyboard_error;
	uint16_t reserved;
	size_t value_count;
	struct hid_report_value values[HID_REPORT_VALUE_COUNT_MAX];
};

struct hid_report_layout_info {
	size_t descriptor_size;
	size_t report_count;
	size_t field_count;
	size_t capability_count;
	size_t absolute_axis_count;
	int uses_report_ids;
	unsigned pen;
	/* The most fingers one report of the touch screen carries (0: none). */
	size_t touch_contacts;
};

/*
 * What a layout says about its touch screen (a Touch Screen application
 * collection with Finger collections in it).
 *
 * The capabilities and axes of the touch screen are not among the layout's
 * own: the touch state machine declares them from this description, as a
 * device of its own.  An instance is filled by the caller's request and owns
 * no storage.
 */
struct hid_report_touch_info {
	/* The most Finger collections one report carries. */
	size_t contacts;
	/* Whether the reports carry a Contact Count (and may split one frame). */
	int count_present;
	/* The position axes of the first finger (every finger shares them). */
	struct input_absinfo x;
	struct input_absinfo y;
	/*
	 * Whether the reports carry a Scan Time (the screen's own clock), its
	 * logical maximum (it wraps after it) and its unit in nanoseconds
	 * (100000 when the descriptor gives no unit of time).
	 */
	int scan_time_present;
	int32_t scan_time_maximum;
	uint32_t scan_time_unit_ns;
	/*
	 * Whether the fingers are a touch pad's (a Touch Pad application
	 * collection, a pointer device moved relatively) rather than a touch
	 * screen's, and how many buttons of the Button page the pad's reports
	 * carry beside the fingers (ws159-p003).
	 */
	int pad;
	size_t buttons;
};

/*
 * The Digitizer usages of the feature reports a Windows Precision Touchpad
 * declares (ws159-p003): the device's mode (3 makes it report its fingers),
 * the switches of its surface and its buttons, its latency mode, the most
 * fingers it reports and whether it is a click pad.
 */
#define HID_REPORT_USAGE_DEVICE_MODE		0x000d0052U
#define HID_REPORT_USAGE_CONTACT_COUNT_MAXIMUM	0x000d0055U
#define HID_REPORT_USAGE_SURFACE_SWITCH		0x000d0057U
#define HID_REPORT_USAGE_BUTTON_SWITCH		0x000d0058U
#define HID_REPORT_USAGE_PAD_TYPE		0x000d0059U
#define HID_REPORT_USAGE_LATENCY_MODE		0x000d0060U

/*
 * Where one field of a feature report is: its report's identifier (0 for a
 * descriptor without identifiers), the field's bit offset and size within
 * the report's data (after the identifier byte), and the length of that
 * data in bytes, which a GET or SET of the report carries.
 */
struct hid_report_feature_info {
	uint8_t report_id;
	uint32_t bit_offset;
	uint8_t bit_size;
	size_t data_size;
};

struct hid_report_report_info {
	uint8_t report_id;
	size_t minimum_size;
	size_t field_count;
};

int drv_hid_report_layout_parse(const void *, size_t,
	struct hid_report_layout **);
/* BUG-267: where the parser refuses a descriptor (the item's offset, or the length for the whole), and its error. */
int drv_hid_report_layout_diagnose(const void *descriptor, size_t length, size_t *item_offset);
int drv_hid_report_layout_boot_keyboard(struct hid_report_layout **);
int drv_hid_report_layout_boot_mouse(struct hid_report_layout **);
void drv_hid_report_layout_destroy(struct hid_report_layout *);

int drv_hid_report_layout_get_info(const struct hid_report_layout *,
	struct hid_report_layout_info *);
int drv_hid_report_layout_get_report(const struct hid_report_layout *, size_t,
	struct hid_report_report_info *);
int drv_hid_report_layout_get_capability(const struct hid_report_layout *, size_t,
	struct input_capability *);
int drv_hid_report_layout_get_absolute_axis(const struct hid_report_layout *,
	size_t, struct input_abs_axis *);
int drv_hid_report_layout_get_touch(const struct hid_report_layout *,
	struct hid_report_touch_info *);
int drv_hid_report_layout_get_feature(const struct hid_report_layout *,
	uint32_t, struct hid_report_feature_info *);

int drv_hid_report_decode(const struct hid_report_layout *, const void *, size_t,
	struct hid_report_input *);

#endif
