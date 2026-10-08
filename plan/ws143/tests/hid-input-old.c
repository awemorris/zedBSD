/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The oracle of plan/ws143/tests/hid-input-host-test.c (ws143-p005 i01a):
 * the report handling of src/drivers/usb/usb-hid.c as it was before the
 * HID input glue (git a6988363c), copied for the test only.  The bodies of
 * usb_hid_fetch_layout (after the descriptor is read; no raw branch, no
 * endpoint), usb_hid_has_capability, the naming of usb_hid_identity,
 * usb_hid_report_state, usb_hid_publish_report (no raw branch, no lock),
 * usb_hid_publish_pen_report, usb_hid_publish_touch_report, the register
 * part of usb_hid_activate and the input part of usb_hid_unpublish are kept
 * as they were, statement for statement, with the USB fields taken out.
 * This file is deliberately left in the old code's form (it is not to be
 * restyled: it is the reference the new code is compared with), and it is
 * not part of any build but the test's.  It is compiled freestanding like
 * the kernel's files, so its error numbers are the kernel's.
 */

#include "hid-input-old.h"

#include <drivers/generic/hid-digitizer.h>
#include <drivers/generic/hid-touch.h>
#include <drivers/generic/hid-report.h>
#include <kern/input-device.h>

#include <kern/kcrt.h>

#include <uapi/errno.h>

#define USB_HID_ERROR_MARKERS		16U

static int usb_hid_has_capability(const struct old_hid *hid, uint16_t type, uint16_t code);
static struct old_hid_report_state * usb_hid_report_state(struct old_hid *hid, uint8_t report_id);
static void usb_hid_publish_pen_report(struct old_hid *hid, const struct hid_report_input *decoded);
static void usb_hid_publish_touch_report(struct old_hid *hid, const struct hid_report_input *decoded);

int
old_hid_prepare(struct old_hid *hid, const uint8_t *descriptor, size_t descriptor_length)
{
	struct hid_report_report_info report;
	struct hid_report_layout_info info;
	struct hid_report_touch_info touch_info;
	size_t index;
	size_t maximum_report = 0;
	int error;

	kern_memset(hid, 0, sizeof(*hid));
	error = drv_hid_report_layout_parse(
		descriptor, descriptor_length, &hid->layout);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = drv_hid_report_layout_get_info(hid->layout, &info);
	if (error != 0 || info.report_count == 0U ||
	    info.report_count > HID_REPORT_ID_COUNT_MAX ||
	    info.capability_count > HID_REPORT_FIELD_COUNT_MAX + 1U ||
	    info.absolute_axis_count > ABS_MAX + 1U) {
		/* Returns the computed result. */
		return error != 0 ? error : EINVAL;
	}
	hid->report_count = info.report_count;
	hid->capability_count = info.capability_count;
	hid->absolute_axis_count = info.absolute_axis_count;

	/* A pen device starts with no tool in range. */
	hid->pen = info.pen;
	drv_hid_digitizer_reset(&hid->digitizer);

	/* A touch screen is described for a device of its own, with no finger down. */
	error = drv_hid_report_layout_get_touch(hid->layout, &touch_info);
	if (error == 0) {
		error = drv_hid_touch_describe(&touch_info, &hid->touch_description);
		if (error == 0) {
			hid->touch_present = 1U;
			drv_hid_touch_reset(&hid->touch, hid->touch_description.slots);
			drv_hid_touch_set_scan_time(&hid->touch, &touch_info);
			drv_hid_touch_set_pad(&hid->touch, &touch_info);
		}
	}

	/* Process each remaining element. */
	for (index = 0; index < info.report_count; index++) {
		/* Checks the operation status. */
		error = drv_hid_report_layout_get_report(hid->layout, index,
							 &report);
		if (error != 0 || report.minimum_size == 0U ||
		    report.minimum_size > HID_REPORT_BITS_MAX / 8U + 1U) {
			/* Returns the computed result. */
			return error != 0 ? error : EINVAL;
		}
		hid->reports[index].id = report.report_id;

		/* Handles the report condition. */
		if (report.minimum_size > maximum_report)
			maximum_report = report.minimum_size;
	}

	/* Process each remaining element. */
	for (index = 0; index < info.capability_count; index++) {
		/* Checks the operation status. */
		error = drv_hid_report_layout_get_capability(
			hid->layout, index, &hid->capabilities[index]);
		if (error != 0)
			return error;
	}

	/* Process each remaining element. */
	for (index = 0; index < info.absolute_axis_count; index++) {
		/* Checks the operation status. */
		error = drv_hid_report_layout_get_absolute_axis(
			hid->layout, index, &hid->absolute_axes[index]);
		if (error != 0)
			return error;
	}

	hid->buffer_size = maximum_report;

	/* Succeeded. */
	return 0;
}

static int
usb_hid_has_capability(
	const struct old_hid *hid,
	uint16_t type,
	uint16_t code)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < hid->capability_count; index++) {
		/* Handles the hid condition. */
		if (hid->capabilities[index].type == type &&
		    hid->capabilities[index].code == code) {
			/* Reports operation failure. */
			return 1;
		}
	}

	/* Succeeded. */
	return 0;
}

void
old_hid_fallback_name(const struct old_hid *hid, char *name, size_t size)
{
	if (hid->pen != HID_REPORT_PEN_NONE)
		(void)kern_snprintf(name, size, "USB HID pen");
	else if (usb_hid_has_capability(hid, EV_ABS, ABS_X))
		(void)kern_snprintf(name, size, "USB HID tablet");
	else if (usb_hid_has_capability(hid, EV_REL, REL_X))
		(void)kern_snprintf(name, size, "USB HID mouse");
	else
		(void)kern_snprintf(name, size,
			       "USB HID keyboard");
}

static struct old_hid_report_state *
usb_hid_report_state(
	struct old_hid *hid,
	uint8_t report_id)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < hid->report_count; index++) {
		/* Handles the hid condition. */
		if (hid->reports[index].id == report_id)
			return &hid->reports[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

void
old_hid_report(
	struct old_hid *hid,
	const uint8_t *buffer,
	size_t length,
	uint64_t milliseconds)
{
	const struct hid_report_value *value_local;
	const struct hid_report_value *value_local1;
	size_t word;
	unsigned long bit;
	int old_value;
	int new_value;
	struct hid_report_input decoded;
	struct old_hid_report_state *state;
	unsigned long current[INPUT_BIT_WORDS(KEY_MAX)];
	unsigned long aggregate[INPUT_BIT_WORDS(KEY_MAX)];
	size_t index;
	unsigned code;
	int is_pen;
	int is_touch;
	int error, emitted = 0;

	hid->report_milliseconds = milliseconds;

	/* Checks the operation status. */
	error = drv_hid_report_decode(hid->layout, buffer, length, &decoded);
	if (error != 0) {
		/* Checks the operation status. */
		if (hid->error_markers++ < USB_HID_ERROR_MARKERS)
			hid->malformed_logged++;

		/* Returns the computed result. */
		return;
	}

	/* A touch report goes through the touch state machine to the touch screen's device. */
	is_touch = drv_hid_touch_report_is_touch(&decoded);
	if (is_touch) {
		usb_hid_publish_touch_report(hid, &decoded);
		return;
	}

	/* A pen report goes through the pen state machine instead. */
	is_pen = drv_hid_digitizer_report_is_pen(&decoded);
	if (is_pen) {
		usb_hid_publish_pen_report(hid, &decoded);
		return;
	}

	/* Handles the state availability. */
	state = usb_hid_report_state(hid, decoded.report_id);
	if (state == NULL)
		return;
	kern_memset(current, 0, sizeof(current));
	/* Process each remaining element. */
	for (index = 0; index < decoded.value_count; index++) {
		/* Handles the value local condition. */
		value_local = &decoded.values[index];
		if (value_local->type == EV_KEY &&
		    value_local->code <= KEY_MAX) {
			current[value_local->code / INPUT_BITS_PER_WORD] |=
				1UL
				<< (value_local->code % INPUT_BITS_PER_WORD);
		}
	}

	/* Checks the operation status. */
	if (!decoded.keyboard_error) {
		kern_memcpy(state->held, current, sizeof(state->held));
		kern_memset(aggregate, 0, sizeof(aggregate));
		/* Process each remaining element. */
		for (index = 0; index < hid->report_count; index++) {
			/* Process each element required by the operation. */
			for (word = 0; word < INPUT_BIT_WORDS(KEY_MAX);
			     word++) {
				aggregate[word] |=
					hid->reports[index].held[word];
			}
		}

		/* Process each element required by the operation. */
		for (code = 0; code <= KEY_MAX; code++) {
			bit = 1UL << (code % INPUT_BITS_PER_WORD);
			old_value = (hid->held[code / INPUT_BITS_PER_WORD] &
				     bit) != 0;

			/* Handles the old value condition. */
			new_value = (aggregate[code / INPUT_BITS_PER_WORD] &
				     bit) != 0;
			if (old_value == new_value)
				continue;
			drv_input_device_emit_at(hid->input, EV_KEY,
						 (uint16_t)code, new_value,
						 hid->report_milliseconds);
			emitted = 1;
		}

		kern_memcpy(hid->held, aggregate, sizeof(hid->held));
	}

	/* Process each remaining element. */
	for (index = 0; index < decoded.value_count; index++) {
		/* Handles the value local1 condition. */
		value_local1 = &decoded.values[index];
		if (value_local1->type == EV_KEY ||
		    (value_local1->type == EV_REL && value_local1->value == 0))
			continue;
		drv_input_device_emit_at(hid->input, value_local1->type,
					 value_local1->code, value_local1->value,
					 hid->report_milliseconds);
		emitted = 1;
	}

	/* Handles the emitted condition. */
	if (emitted)
		drv_input_device_emit_at(hid->input, EV_SYN, SYN_REPORT, 0, hid->report_milliseconds);
}

void
old_hid_unpublish(
	struct old_hid *hid)
{
	struct input_device *input;
	struct input_device *touch_input;

	input = hid->input;
	touch_input = hid->touch_input;
	hid->input = NULL;
	hid->touch_input = NULL;

	/* Handles the input availability. */
	if (input != NULL)
		drv_input_device_unregister(input);

	/* The touch screen's device goes with it. */
	if (touch_input != NULL)
		drv_input_device_unregister(touch_input);
}

int
old_hid_publish(struct old_hid *hid, const char *name, const char *physical_path, const char *unique_id,
	const char *touch_name, const char *touch_physical_path, const struct input_id *id)
{
	struct input_device_info info;
	int error;

	kern_memset(&info, 0, sizeof(info));
	info.name = name;
	info.physical_path = physical_path;
	info.unique_id = unique_id;
	info.id.bustype = id->bustype;
	info.id.vendor = id->vendor;
	info.id.product = id->product;
	info.id.version = id->version;
	info.capabilities = hid->capabilities;
	info.capability_count = hid->capability_count;
	info.absolute_axes = hid->absolute_axes;
	info.absolute_axis_count = hid->absolute_axis_count;

	/*
	 * The interface's own device, unless the interface is a touch screen
	 * and nothing else (its own capabilities are then EV_SYN alone); a
	 * raw interface's node in place of both.
	 */
	error = 0;
	if (hid->capability_count > 1U)
		error = drv_input_device_register(&info, &hid->input);

	/* The touch screen's device beside it, under the same identity. */
	if (error == 0 && hid->touch_present) {
		info.name = touch_name;
		info.physical_path = touch_physical_path;
		info.capabilities = hid->touch_description.capabilities;
		info.capability_count = hid->touch_description.capability_count;
		info.absolute_axes = hid->touch_description.axes;
		info.absolute_axis_count = hid->touch_description.axis_count;
		info.properties = hid->touch_description.properties;
		error = drv_input_device_register(&info, &hid->touch_input);
	}

	/* An interface with no device has nothing to publish. */
	if (error == 0 && hid->input == NULL && hid->touch_input == NULL)
		error = ENODEV;

	/* A failed publication takes back what was published and stops the worker. */
	if (error != 0)
		old_hid_unpublish(hid);
	return error;
}

void
old_hid_destroy(struct old_hid *hid)
{
	if (hid->layout != NULL)
		drv_hid_report_layout_destroy(hid->layout);
	hid->layout = NULL;
}

/* Publishes one pen report through the pen state machine. */
static void
usb_hid_publish_pen_report(
	struct old_hid *hid,
	const struct hid_report_input *decoded)
{
	struct hid_digitizer_output output;
	const struct hid_digitizer_event *event;
	size_t index;
	int error;

	/* Turns the switches and axes into ordered tool and contact frames. */
	error = drv_hid_digitizer_translate(&hid->digitizer, decoded, &output);
	if (error != 0) {
		/* Leaves a marker for the first reports that did not fit. */
		if (hid->error_markers < USB_HID_ERROR_MARKERS) {
			hid->error_markers++;
		}

		/* The report is dropped; the next one continues the frames. */
		return;
	}

	/* Hands every event of the frames to the input layer in order. */
	for (index = 0; index < output.event_count; index++) {
		event = &output.events[index];
		drv_input_device_emit_at(hid->input, event->type, event->code,
					 event->value, hid->report_milliseconds);
	}
}

/* Publishes one touch report through the touch state machine. */
static void
usb_hid_publish_touch_report(
	struct old_hid *hid,
	const struct hid_report_input *decoded)
{
	struct hid_touch_output output;
	const struct hid_touch_event *event;
	size_t index;
	int error;

	/* Turns the fingers into protocol B frames, at the time the report arrived. */
	error = drv_hid_touch_translate_at(&hid->touch, decoded, hid->report_milliseconds, &output);
	if (error != 0) {
		/* Leaves a marker for the first reports that did not fit. */
		if (hid->error_markers < USB_HID_ERROR_MARKERS) {
			hid->error_markers++;
		}

		/* The report is dropped; the next frame starts again. */
		return;
	}

	/* Hands every event of the frames to the touch screen's device in order. */
	for (index = 0; index < output.event_count; index++) {
		event = &output.events[index];
		drv_input_device_emit_at(hid->touch_input, event->type, event->code,
					 event->value, hid->report_milliseconds);
	}
}

int
old_hid_enodev(void)
{
	return ENODEV;
}
