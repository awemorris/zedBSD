/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The oracle of the HID input glue's host test (ws143-p005 i01a, i01c):
 * the old usb-hid and i2c-hid report handling, copied in hid-input-old.c.
 */

#ifndef WS143_TESTS_HID_INPUT_OLD_H
#define WS143_TESTS_HID_INPUT_OLD_H

#include <drivers/generic/hid-digitizer.h>
#include <drivers/generic/hid-report.h>
#include <drivers/generic/hid-touch.h>
#include <kern/input-device.h>

#include <stddef.h>
#include <stdint.h>

/* The keys one report ID of the old driver held. */
struct old_hid_report_state {
	uint8_t id;
	unsigned long held[INPUT_BIT_WORDS(KEY_MAX)];
};

/* The fields of the old struct usb_hid that its report handling used. */
struct old_hid {
	struct hid_report_layout *layout;
	struct input_device *input;
	size_t buffer_size;
	struct input_capability capabilities[HID_REPORT_FIELD_COUNT_MAX + 1U];
	struct input_abs_axis absolute_axes[ABS_MAX + 1U];
	struct old_hid_report_state reports[HID_REPORT_ID_COUNT_MAX];
	unsigned long held[INPUT_BIT_WORDS(KEY_MAX)];
	struct hid_digitizer_state digitizer;
	unsigned pen;
	struct input_device *touch_input;
	struct hid_touch_state touch;
	struct hid_touch_description touch_description;
	unsigned touch_present;
	size_t capability_count;
	size_t absolute_axis_count;
	size_t report_count;
	uint64_t report_milliseconds;
	unsigned error_markers;
	unsigned malformed_logged;
};

/* The fields of the old struct i2c_hid_device that its report handling used. */
struct old_i2c {
	struct hid_report_layout *layout;
	struct hid_report_touch_info touch;
	struct hid_touch_description description;
	struct hid_touch_state state;
	struct hid_report_input decoded;
	struct hid_touch_output output;
	struct input_device *input;
	char name[64];
};

int old_hid_prepare(struct old_hid *hid, const uint8_t *descriptor, size_t descriptor_length);
void old_hid_fallback_name(const struct old_hid *hid, char *name, size_t size);
int old_hid_publish(struct old_hid *hid, const char *name, const char *physical_path, const char *unique_id,
	const char *touch_name, const char *touch_physical_path, const struct input_id *id);
void old_hid_report(struct old_hid *hid, const uint8_t *buffer, size_t length, uint64_t milliseconds);
void old_hid_unpublish(struct old_hid *hid);
void old_hid_destroy(struct old_hid *hid);
int old_hid_enodev(void);
int old_i2c_prepare(struct old_i2c *device, const uint8_t *descriptor, size_t length);
int old_i2c_publish(struct old_i2c *device, const char *path, uint16_t vendor, uint16_t product, uint16_t version);
void old_i2c_report(struct old_i2c *device, const uint8_t *report, size_t length, uint64_t now);
void old_i2c_unpublish(struct old_i2c *device);
void old_i2c_destroy(struct old_i2c *device);

#endif
