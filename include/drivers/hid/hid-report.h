/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_DRIVERS_HID_REPORT_H
#define ZEDBSD_DRIVERS_HID_REPORT_H

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
};

struct hid_report_report_info {
	uint8_t report_id;
	size_t minimum_size;
	size_t field_count;
};

int hid_report_layout_parse(const void *, size_t,
	struct hid_report_layout **);
int hid_report_layout_boot_keyboard(struct hid_report_layout **);
int hid_report_layout_boot_mouse(struct hid_report_layout **);
void hid_report_layout_destroy(struct hid_report_layout *);

int hid_report_layout_get_info(const struct hid_report_layout *,
	struct hid_report_layout_info *);
int hid_report_layout_get_report(const struct hid_report_layout *, size_t,
	struct hid_report_report_info *);
int hid_report_layout_get_capability(const struct hid_report_layout *, size_t,
	struct input_capability *);
int hid_report_layout_get_absolute_axis(const struct hid_report_layout *,
	size_t, struct input_abs_axis *);

int hid_report_decode(const struct hid_report_layout *, const void *, size_t,
	struct hid_report_input *);

#endif
