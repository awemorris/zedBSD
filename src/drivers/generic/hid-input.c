/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HID input glue (ws143-p005, moved out of usb-hid.c): the devices a
 * HID report descriptor declares, and the events of every input report.
 *
 * A report goes to one of three places.  A touch screen's or pad's report
 * goes through the touch state machine to the touch device of its own; a
 * pen's report goes through the pen state machine to the main device; any
 * other report is a set of held keys and of axis values.  The keys of every
 * report ID are kept apart and joined, so a key one report holds is not
 * released by another report that does not carry it, and only the keys whose
 * joined state changed are told.  Relative axes that did not move are not
 * told, and a report that told anything ends with SYN_REPORT.
 */

#include <drivers/generic/hid-digitizer.h>
#include <drivers/generic/hid-input.h>
#include <drivers/generic/hid-report.h>
#include <drivers/generic/hid-touch.h>
#include <kern/input-device.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>

#include <stdint.h>
#include <uapi/errno.h>
#include "kern/klog.h"

/* How many malformed or dropped reports leave a line in the kernel's log. */
#define HID_INPUT_LOG_MARKERS		16U

/* The longest report the parser can describe: its bits and the ID's byte. */
#define HID_INPUT_REPORT_SIZE_MAX	(HID_REPORT_BITS_MAX / 8U + 1U)

/*
 * One report the descriptor declares: its ID, its length in bytes (the
 * ID's byte included), and the keys it holds now.  Kept from prepare to
 * destroy.
 */
struct hid_input_report_state {
	uint8_t id;
	size_t minimum_size;
	unsigned long held[INPUT_BIT_WORDS(KEY_MAX)];
};

/*
 * One descriptor's devices.
 *
 * The capabilities, the axes and the touch description are what the
 * devices declare when they are published.  The held keys of each report
 * and their join are what readers have been told.  The decoded report and
 * the state machines' outputs are working storage of drv_hid_input_report(),
 * kept here so that a report handed on from a system call does not need
 * several kilobytes of a kernel stack.
 */
struct hid_input {
	struct hid_report_layout *layout;
	struct input_device *input;
	struct input_device *touch_input;
	struct input_capability capabilities[HID_REPORT_FIELD_COUNT_MAX + 1U];
	size_t capability_count;
	struct input_abs_axis absolute_axes[ABS_MAX + 1U];
	size_t absolute_axis_count;
	struct hid_input_report_state reports[HID_REPORT_ID_COUNT_MAX];
	size_t report_count;
	size_t report_max;
	int uses_report_ids;
	unsigned long held[INPUT_BIT_WORDS(KEY_MAX)];
	/* What the pen state machine has already told readers (pen devices only). */
	struct hid_digitizer_state digitizer;
	unsigned pen;
	/* The touch screen or pad: what its state machine has told, and what it declares. */
	struct hid_touch_state touch;
	struct hid_touch_description touch_description;
	unsigned touch_present;
	/*
	 * The reports the layout refused, which a transport may report, and the
	 * lines left in the kernel's log for the refused or dropped ones (at
	 * most HID_INPUT_LOG_MARKERS, so a broken device cannot fill the log).
	 */
	unsigned malformed;
	unsigned log_markers;
	/* The physical path given at publication, for the log's lines. */
	char physical_path[64];
	struct hid_report_input decoded;
	struct hid_digitizer_output pen_output;
	struct hid_touch_output touch_output;
	unsigned long current[INPUT_BIT_WORDS(KEY_MAX)];
	unsigned long aggregate[INPUT_BIT_WORDS(KEY_MAX)];
};

static int hid_input_describe(struct hid_input *input);
static int hid_input_has_capability(const struct hid_input *input, uint16_t type, uint16_t code);
static struct hid_input_report_state *hid_input_report_state(struct hid_input *input, uint8_t report_id);
static void hid_input_keys(struct hid_input *input, struct hid_input_report_state *state, uint64_t milliseconds, int *emitted);
static void hid_input_pen_report(struct hid_input *input, uint64_t milliseconds);
static void hid_input_touch_report(struct hid_input *input, uint64_t milliseconds);
static int hid_input_log_marker(struct hid_input *input);

/*
 * Parses a report descriptor and describes the devices it declares.
 *
 * Nothing is registered.  Fails with the parser's error (EINVAL, E2BIG,
 * ENOMEM, EOPNOTSUPP) or EINVAL for a descriptor whose reports or devices
 * do not fit; *result is then untouched.
 */
int
drv_hid_input_prepare(
	const void *descriptor,
	size_t size,
	struct hid_input **result)
{
	struct hid_input *input;
	int error;

	/* The record, zeroed: no device, no key held. */
	input = kern_malloc(sizeof(*input));
	if (input == NULL)
		return ENOMEM;
	kern_memset(input, 0, sizeof(*input));

	/* The layout of the reports. */
	error = drv_hid_report_layout_parse(descriptor, size, &input->layout);
	if (error != 0) {
		kern_free(input);
		return error;
	}

	/* What the devices declare. */
	error = hid_input_describe(input);
	if (error != 0) {
		drv_hid_report_layout_destroy(input->layout);
		kern_free(input);
		return error;
	}

	/* Succeeded: the caller owns the description. */
	*result = input;
	return 0;
}

/*
 * Reports the longest input report the descriptor declares, in bytes, the
 * report ID's byte included.
 */
size_t
drv_hid_input_report_max(
	const struct hid_input *input)
{
	/* The longest report's size. */
	return input->report_max;
}

/* Reports whether the descriptor numbers its reports (each starts with its ID's byte). */
int
drv_hid_input_report_ids(
	const struct hid_input *input)
{
	/* Nonzero when the reports carry their ID. */
	return input->uses_report_ids;
}

/*
 * Reports whether a report is shorter than its report ID declares (the ID
 * is its first byte when the descriptor numbers its reports).  A report of
 * an ID the descriptor does not declare is not short: the layout refuses
 * it as malformed.
 */
int
drv_hid_input_report_short(
	const struct hid_input *input,
	const uint8_t *report,
	size_t length)
{
	const struct hid_input_report_state *state;
	uint8_t report_id;
	size_t index;

	/* The report's ID: its first byte, or 0 for a descriptor without IDs. */
	report_id = 0;
	if (input->uses_report_ids) {
		/* A numbered report needs at least its ID. */
		if (length == 0U)
			return 1;
		report_id = report[0];
	}

	/* The declared report of that ID. */
	state = NULL;
	for (index = 0; index < input->report_count; index++) {
		/* The report's own record. */
		if (input->reports[index].id == report_id) {
			state = &input->reports[index];
			break;
		}
	}

	/* An undeclared ID is the layout's to refuse. */
	if (state == NULL)
		return 0;

	/* Shorter than declared. */
	if (length < state->minimum_size)
		return 1;

	/* Long enough. */
	return 0;
}

/*
 * Reports what the devices chiefly are (HID_INPUT_KIND_*), for the name of a
 * device that has none of its own.
 */
unsigned
drv_hid_input_kind(
	const struct hid_input *input)
{
	int found;

	/* A device with a pen collection is its pen. */
	if (input->pen != HID_REPORT_PEN_NONE)
		return HID_INPUT_KIND_PEN;

	/* An absolute pointer is a tablet. */
	found = hid_input_has_capability(input, EV_ABS, ABS_X);
	if (found)
		return HID_INPUT_KIND_TABLET;

	/* A relative pointer is a mouse. */
	found = hid_input_has_capability(input, EV_REL, REL_X);
	if (found)
		return HID_INPUT_KIND_MOUSE;

	/* Anything else is taken for a keyboard. */
	return HID_INPUT_KIND_KEYBOARD;
}

/*
 * Reports what the descriptor says about its touch screen or pad (ENOENT
 * for a descriptor without one).
 */
int
drv_hid_input_touch(
	const struct hid_input *input,
	struct hid_report_touch_info *touch)
{
	int error;

	/* The layout's answer. */
	error = drv_hid_report_layout_get_touch(input->layout, touch);
	if (error != 0)
		return error;

	/* Succeeded: the touch device is described. */
	return 0;
}

/*
 * Reports where a field of a feature report is (a Precision Touchpad's
 * mode and switches), or the layout's error for a descriptor without it.
 */
int
drv_hid_input_feature(
	const struct hid_input *input,
	uint32_t usage,
	struct hid_report_feature_info *feature)
{
	int error;

	/* The layout's answer. */
	error = drv_hid_report_layout_get_feature(input->layout, usage, feature);
	if (error != 0)
		return error;

	/* Succeeded: the field is found. */
	return 0;
}

/*
 * Registers the devices a descriptor declares with the input layer.
 *
 * The main device is registered unless it would declare nothing but
 * EV_SYN (a touch screen and nothing else) or the identity asks for the
 * touch device alone (HID_INPUT_TOUCH_ONLY; the main device's reports are
 * then dropped); a touch screen or pad is a device of its own beside it.  Fails with ENODEV when there is no device
 * at all, or with the input layer's error (ENOSPC when it has no room); a
 * failure leaves nothing registered.
 */
int
drv_hid_input_publish(
	struct hid_input *input,
	const struct hid_input_identity *identity)
{
	struct input_device_info info;
	int error;

	/* The path the log's lines name the devices by. */
	(void)kern_snprintf(input->physical_path, sizeof(input->physical_path), "%s", identity->physical_path);

	/* The main device's identity and what it declares. */
	kern_memset(&info, 0, sizeof(info));
	info.name = identity->name;
	info.physical_path = identity->physical_path;
	info.unique_id = identity->unique_id;
	info.id = identity->id;
	info.capabilities = input->capabilities;
	info.capability_count = input->capability_count;
	info.absolute_axes = input->absolute_axes;
	info.absolute_axis_count = input->absolute_axis_count;

	/* The main device, unless it would declare EV_SYN alone or the transport wants the touch device alone. */
	error = 0;
	if (input->capability_count > 1U && (identity->flags & HID_INPUT_TOUCH_ONLY) == 0U)
		error = drv_input_device_register(&info, &input->input);

	/* The touch screen's or pad's device beside it, under the same identity. */
	if (error == 0 && input->touch_present) {
		info.name = identity->touch_name;
		info.physical_path = identity->touch_physical_path;
		info.capabilities = input->touch_description.capabilities;
		info.capability_count = input->touch_description.capability_count;
		info.absolute_axes = input->touch_description.axes;
		info.absolute_axis_count = input->touch_description.axis_count;
		info.properties = input->touch_description.properties;
		error = drv_input_device_register(&info, &input->touch_input);
	}

	/* A descriptor that declares no device has nothing to publish. */
	if (error == 0 && input->input == NULL && input->touch_input == NULL)
		error = ENODEV;

	/* A failed publication takes back what was registered. */
	if (error != 0) {
		drv_hid_input_unpublish(input);
		return error;
	}

	/* Succeeded: the devices are in the input layer. */
	return 0;
}

/*
 * Hands one input report, as the transport received it, to the devices.
 *
 * milliseconds is when the report arrived (CLOCK_MONOTONIC), the time of
 * every event it brings.  A report the layout refuses is counted and
 * dropped; the devices keep their state.
 */
void
drv_hid_input_report(
	struct hid_input *input,
	const uint8_t *report,
	size_t length,
	uint64_t milliseconds)
{
	const struct hid_report_value *value;
	struct hid_input_report_state *state;
	size_t index;
	int is_touch;
	int is_pen;
	int marker;
	int error;
	int emitted;

	/* The values of the report. */
	error = drv_hid_report_decode(input->layout, report, length, &input->decoded);
	if (error != 0) {
		input->malformed++;
		marker = hid_input_log_marker(input);
		if (marker) {
			kern_logf("hid-input: malformed input %s length=%u error=%d\n",
				  input->physical_path,
				  (unsigned)length,
				  error);
		}

		/* The report is dropped. */
		return;
	}

	/* A touch report goes through the touch state machine to the touch device. */
	is_touch = drv_hid_touch_report_is_touch(&input->decoded);
	if (is_touch) {
		hid_input_touch_report(input, milliseconds);
		return;
	}

	/* A pen report goes through the pen state machine instead. */
	is_pen = drv_hid_digitizer_report_is_pen(&input->decoded);
	if (is_pen) {
		hid_input_pen_report(input, milliseconds);
		return;
	}

	/* A report ID the descriptor does not declare has nothing to tell. */
	state = hid_input_report_state(input, input->decoded.report_id);
	if (state == NULL)
		return;

	/* The keys whose joined state changed. */
	emitted = 0;
	hid_input_keys(input, state, milliseconds, &emitted);

	/* Every other value as it is, except a relative axis that did not move. */
	for (index = 0; index < input->decoded.value_count; index++) {
		value = &input->decoded.values[index];

		/* The keys were told above. */
		if (value->type == EV_KEY)
			continue;

		/* A relative axis that did not move tells nothing. */
		if (value->type == EV_REL && value->value == 0)
			continue;

		/* The value, at the report's time. */
		drv_input_device_emit_at(input->input, value->type, value->code, value->value, milliseconds);
		emitted = 1;
	}

	/* A report that told anything ends its frame. */
	if (emitted)
		drv_input_device_emit_at(input->input, EV_SYN, SYN_REPORT, 0, milliseconds);
}

/* Reports how many input reports the layout refused. */
unsigned
drv_hid_input_malformed(
	const struct hid_input *input)
{
	/* The count of refused reports. */
	return input->malformed;
}

/*
 * Reports the numbers of the published devices' nodes (/dev/input/eventN):
 * the main device's and the touch device's, -1 for one not published.
 */
void
drv_hid_input_numbers(
	const struct hid_input *input,
	int *event,
	int *touch_event)
{
	/* The main device's node. */
	*event = -1;
	if (input->input != NULL)
		*event = (int)drv_input_device_number(input->input);

	/* The touch device's node. */
	*touch_event = -1;
	if (input->touch_input != NULL)
		*touch_event = (int)drv_input_device_number(input->touch_input);
}

/*
 * Takes the published devices out of the input layer, which releases every
 * key and button they still hold.  Unpublishing twice does nothing more.
 */
void
drv_hid_input_unpublish(
	struct hid_input *input)
{
	struct input_device *device;
	struct input_device *touch_device;

	/* The devices are forgotten before they go, so a second call finds none. */
	device = input->input;
	touch_device = input->touch_input;
	input->input = NULL;
	input->touch_input = NULL;

	/* The main device goes. */
	if (device != NULL)
		drv_input_device_unregister(device);

	/* The touch device goes with it. */
	if (touch_device != NULL)
		drv_input_device_unregister(touch_device);
}

/* Frees a description after its devices were unpublished. */
void
drv_hid_input_destroy(
	struct hid_input *input)
{
	/* Nothing to free. */
	if (input == NULL)
		return;

	/* The layout, then the record. */
	drv_hid_report_layout_destroy(input->layout);
	kern_free(input);
}

/*
 * Describes the devices of a parsed layout: its reports, capabilities and
 * axes, its pen, and its touch screen or pad.  EINVAL for a layout whose
 * reports or devices do not fit.
 */
static int
hid_input_describe(
	struct hid_input *input)
{
	struct hid_report_layout_info info;
	struct hid_report_report_info report;
	struct hid_report_touch_info touch_info;
	size_t index;
	int error;

	/* The counts of the layout. */
	error = drv_hid_report_layout_get_info(input->layout, &info);
	if (error != 0)
		return error;

	/* A layout without reports, or with more than the records hold, is refused. */
	if (info.report_count == 0U)
		return EINVAL;
	if (info.report_count > HID_REPORT_ID_COUNT_MAX)
		return EINVAL;
	if (info.capability_count > HID_REPORT_FIELD_COUNT_MAX + 1U)
		return EINVAL;
	if (info.absolute_axis_count > ABS_MAX + 1U)
		return EINVAL;
	input->report_count = info.report_count;
	input->capability_count = info.capability_count;
	input->absolute_axis_count = info.absolute_axis_count;
	input->uses_report_ids = info.uses_report_ids;

	/* A pen device starts with no tool in range. */
	input->pen = info.pen;
	drv_hid_digitizer_reset(&input->digitizer);

	/* A touch screen or pad is described for a device of its own, with no finger down. */
	error = drv_hid_report_layout_get_touch(input->layout, &touch_info);
	if (error == 0) {
		error = drv_hid_touch_describe(&touch_info, &input->touch_description);
		if (error == 0) {
			input->touch_present = 1U;
			drv_hid_touch_reset(&input->touch, input->touch_description.slots);
			drv_hid_touch_set_scan_time(&input->touch, &touch_info);
			drv_hid_touch_set_pad(&input->touch, &touch_info);
		}
	}

	/* Each report's ID, and the longest report. */
	for (index = 0; index < info.report_count; index++) {
		error = drv_hid_report_layout_get_report(input->layout, index, &report);
		if (error != 0)
			return error;

		/* An empty report, or one longer than the parser describes, is refused. */
		if (report.minimum_size == 0U)
			return EINVAL;
		if (report.minimum_size > HID_INPUT_REPORT_SIZE_MAX)
			return EINVAL;
		input->reports[index].id = report.report_id;
		input->reports[index].minimum_size = report.minimum_size;

		/* The longest so far. */
		if (report.minimum_size > input->report_max)
			input->report_max = report.minimum_size;
	}

	/* What the main device declares. */
	for (index = 0; index < info.capability_count; index++) {
		error = drv_hid_report_layout_get_capability(input->layout, index, &input->capabilities[index]);
		if (error != 0)
			return error;
	}

	/* The ranges of its absolute axes. */
	for (index = 0; index < info.absolute_axis_count; index++) {
		error = drv_hid_report_layout_get_absolute_axis(input->layout, index, &input->absolute_axes[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the devices are described. */
	return 0;
}

/* Reports whether the main device declares one event. */
static int
hid_input_has_capability(
	const struct hid_input *input,
	uint16_t type,
	uint16_t code)
{
	size_t index;

	/* Looks through what the main device declares. */
	for (index = 0; index < input->capability_count; index++) {
		/* Another event. */
		if (input->capabilities[index].type != type)
			continue;
		if (input->capabilities[index].code != code)
			continue;

		/* Succeeded: the event is declared. */
		return 1;
	}

	/* The event is not declared. */
	return 0;
}

/* Finds the held keys of one report ID, NULL for an ID the descriptor does not declare. */
static struct hid_input_report_state *
hid_input_report_state(
	struct hid_input *input,
	uint8_t report_id)
{
	size_t index;

	/* Looks through the declared reports. */
	for (index = 0; index < input->report_count; index++) {
		/* The report's own record. */
		if (input->reports[index].id == report_id)
			return &input->reports[index];
	}

	/* An undeclared report ID. */
	return NULL;
}

/*
 * Tells the keys whose state changed: the decoded report's keys replace
 * what its report ID held, and every report's keys are joined.  A keyboard
 * report that carried an error (ErrorRollOver and the like) keeps the keys
 * as they were.  Sets *emitted when a key was told.
 */
static void
hid_input_keys(
	struct hid_input *input,
	struct hid_input_report_state *state,
	uint64_t milliseconds,
	int *emitted)
{
	const struct hid_report_value *value;
	unsigned long bit;
	size_t index;
	size_t word;
	size_t words;
	unsigned code;
	int old_value;
	int new_value;

	/* The keys this report holds. */
	kern_memset(input->current, 0, sizeof(input->current));
	for (index = 0; index < input->decoded.value_count; index++) {
		value = &input->decoded.values[index];

		/* Only the keys the event bits can name. */
		if (value->type != EV_KEY)
			continue;
		if (value->code > KEY_MAX)
			continue;
		input->current[value->code / INPUT_BITS_PER_WORD] |= 1UL << (value->code % INPUT_BITS_PER_WORD);
	}

	/* A keyboard that reported an error keeps the keys it held. */
	if (input->decoded.keyboard_error)
		return;

	/* The report's keys replace its own, and every report's are joined. */
	kern_memcpy(state->held, input->current, sizeof(state->held));
	kern_memset(input->aggregate, 0, sizeof(input->aggregate));
	words = INPUT_BIT_WORDS(KEY_MAX);
	for (index = 0; index < input->report_count; index++) {
		/* One report's held keys join the rest. */
		for (word = 0; word < words; word++)
			input->aggregate[word] |= input->reports[index].held[word];
	}

	/* Each key whose joined state changed is told. */
	for (code = 0; code <= KEY_MAX; code++) {
		bit = 1UL << (code % INPUT_BITS_PER_WORD);

		/* What readers were told, and what holds now. */
		old_value = 0;
		if ((input->held[code / INPUT_BITS_PER_WORD] & bit) != 0)
			old_value = 1;
		new_value = 0;
		if ((input->aggregate[code / INPUT_BITS_PER_WORD] & bit) != 0)
			new_value = 1;

		/* An unchanged key tells nothing. */
		if (old_value == new_value)
			continue;

		/* The change, at the report's time. */
		drv_input_device_emit_at(input->input, EV_KEY, (uint16_t)code, new_value, milliseconds);
		*emitted = 1;
	}

	/* What readers have been told now. */
	kern_memcpy(input->held, input->aggregate, sizeof(input->held));
}

/* Hands one pen report through the pen state machine to the main device. */
static void
hid_input_pen_report(
	struct hid_input *input,
	uint64_t milliseconds)
{
	const struct hid_digitizer_event *event;
	size_t index;
	int marker;
	int error;

	/* Turns the switches and axes into ordered tool and contact frames. */
	error = drv_hid_digitizer_translate(&input->digitizer, &input->decoded, &input->pen_output);
	if (error != 0) {
		marker = hid_input_log_marker(input);
		if (marker)
			kern_logf("hid-input: pen report dropped error=%d\n", error);

		/* The report is dropped; the next one continues the frames. */
		return;
	}

	/* Hands every event of the frames to the input layer in order. */
	for (index = 0; index < input->pen_output.event_count; index++) {
		event = &input->pen_output.events[index];
		drv_input_device_emit_at(input->input, event->type, event->code, event->value, milliseconds);
	}
}

/* Hands one touch report through the touch state machine to the touch device. */
static void
hid_input_touch_report(
	struct hid_input *input,
	uint64_t milliseconds)
{
	const struct hid_touch_event *event;
	size_t index;
	int marker;
	int error;

	/* Turns the fingers into protocol B frames, at the time the report arrived. */
	error = drv_hid_touch_translate_at(&input->touch, &input->decoded, milliseconds, &input->touch_output);
	if (error != 0) {
		marker = hid_input_log_marker(input);
		if (marker)
			kern_logf("hid-input: touch report dropped error=%d\n", error);

		/* The report is dropped; the next frame starts again. */
		return;
	}

	/* Hands every event of the frames to the touch device in order. */
	for (index = 0; index < input->touch_output.event_count; index++) {
		event = &input->touch_output.events[index];
		drv_input_device_emit_at(input->touch_input, event->type, event->code, event->value, milliseconds);
	}
}

/*
 * Reports whether one more refused or dropped report may leave a line in
 * the kernel's log, and counts it when it may.
 */
static int
hid_input_log_marker(
	struct hid_input *input)
{
	/* The first HID_INPUT_LOG_MARKERS leave a line; the rest are quiet. */
	if (input->log_markers >= HID_INPUT_LOG_MARKERS)
		return 0;
	input->log_markers++;

	/* Succeeded: this one is logged. */
	return 1;
}
