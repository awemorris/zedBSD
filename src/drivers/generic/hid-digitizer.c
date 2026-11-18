/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pen state machine of a USB HID digitizer.
 *
 * A digitizer reports In Range, Invert, Tip Switch and Eraser as separate
 * switches.  Readers of an evdev pen expect a tool (BTN_TOOL_PEN or
 * BTN_TOOL_RUBBER) and a contact (BTN_TOUCH) instead, in a fixed order: the
 * tool comes before the axes when a pen enters, and the pressure, the contact
 * and the tool go back to zero when it leaves.  This file turns one decoded
 * report into that ordered event list.  It touches no hardware, so the host
 * tests run it unchanged.
 */

#include <drivers/generic/hid-digitizer.h>
#include <uapi/errno.h>
#include <uapi/input.h>

#include <stddef.h>
#include <stdint.h>

/*
 * The switches of one report, after the decoder has split them out.
 *
 * One instance lives on the stack for the translation of one report.
 */
struct hid_digitizer_switches {
	int in_range_present;
	int invert_present;
	int in_range;
	int invert;
	int tip;
	int eraser;
	int stylus;
	int stylus2;
	int pressure_present;
};

static void collect_switches(const struct hid_report_input *input, struct hid_digitizer_switches *switches);
static uint16_t choose_tool(const struct hid_digitizer_state *state, const struct hid_digitizer_switches *switches);
static int append_event(struct hid_digitizer_output *output, uint16_t type, uint16_t code, int32_t value);
static int emit_leave(struct hid_digitizer_state *state, const struct hid_digitizer_switches *switches, struct hid_digitizer_output *output);
static int emit_present(struct hid_digitizer_state *state, uint16_t tool, const struct hid_report_input *input, const struct hid_digitizer_switches *switches, struct hid_digitizer_output *output);

/*
 * Forgets every tool and button a pen device has reported.
 *
 * The next report is then treated as the first one after attach.
 */
void
drv_hid_digitizer_reset(
	struct hid_digitizer_state *state)
{
	/* Clears the tool, the contact and both side buttons. */
	state->tool = 0;
	state->touch = 0;
	state->stylus = 0;
	state->stylus2 = 0;
	state->reserved[0] = 0;
	state->reserved[1] = 0;
	state->reserved[2] = 0;
}

/*
 * Asks whether a decoded report carries the switches of a pen.
 *
 * A composite device may send its pen and its other collections in
 * different reports; only a report with a Digitizer switch takes the pen
 * path.
 */
int
drv_hid_digitizer_report_is_pen(
	const struct hid_report_input *input)
{
	size_t index;

	/* Looks for any value that only a pen collection produces. */
	for (index = 0; index < input->value_count; index++) {
		/* A pen switch marks the whole report as a pen report. */
		if (input->values[index].type == HID_REPORT_TYPE_DIGITIZER)
			return 1;
	}

	/* No pen switch: the report belongs to another collection. */
	return 0;
}

/*
 * Turns one decoded pen report into the ordered evdev events.
 *
 * A tool that leaves, or is replaced by the other end of the pen, is closed
 * in a frame of its own before the new tool opens its frame.  Each frame ends
 * with SYN_REPORT.  A report that arrives with no tool in range and none
 * before it produces no events.
 */
int
drv_hid_digitizer_translate(
	struct hid_digitizer_state *state,
	const struct hid_report_input *input,
	struct hid_digitizer_output *output)
{
	struct hid_digitizer_switches switches;
	uint16_t tool;
	int error;

	/* Starts an empty event list. */
	output->event_count = 0;

	/* Reads the switches and chooses the tool they describe. */
	collect_switches(input, &switches);
	tool = choose_tool(state, &switches);

	/* Closes the tool that left, or that the other end replaced. */
	if (state->tool != 0 && state->tool != tool) {
		/* Reports an event list too small for the frame. */
		error = emit_leave(state, &switches, output);
		if (error != 0)
			return error;
	}

	/* A pen out of range has no position worth reporting. */
	if (tool == 0)
		return 0;

	/* Opens or continues the frame of the tool in range. */
	error = emit_present(state, tool, input, &switches, output);
	if (error != 0)
		return error;

	/* Succeeded: the output holds every frame of the report. */
	return 0;
}

/* Splits the pen switches of one report out of its decoded values. */
static void
collect_switches(
	const struct hid_report_input *input,
	struct hid_digitizer_switches *switches)
{
	const struct hid_report_value *value;
	int on;
	size_t index;

	/* Starts from a report that declares no switch. */
	switches->in_range_present = 0;
	switches->invert_present = 0;
	switches->in_range = 0;
	switches->invert = 0;
	switches->tip = 0;
	switches->eraser = 0;
	switches->stylus = 0;
	switches->stylus2 = 0;
	switches->pressure_present = 0;

	/* Records each switch and whether the report carries a pressure. */
	for (index = 0; index < input->value_count; index++) {
		value = &input->values[index];

		/* The pressure axis decides whether a leave resets it. */
		if (value->type == EV_ABS && value->code == ABS_PRESSURE) {
			switches->pressure_present = 1;
			continue;
		}

		/* Anything else that is not a pen switch is an axis or a key. */
		if (value->type != HID_REPORT_TYPE_DIGITIZER)
			continue;

		/* A switch is on for any value other than zero. */
		on = 0;
		if (value->value != 0)
			on = 1;

		/* Stores the switch under the meaning of its usage. */
		switch (value->code) {
		case HID_DIGITIZER_USAGE_IN_RANGE:
			switches->in_range_present = 1;
			switches->in_range = on;
			break;
		case HID_DIGITIZER_USAGE_INVERT:
			switches->invert_present = 1;
			switches->invert = on;
			break;
		case HID_DIGITIZER_USAGE_TIP_SWITCH:
			switches->tip = on;
			break;
		case HID_DIGITIZER_USAGE_ERASER:
			switches->eraser = on;
			break;
		case HID_DIGITIZER_USAGE_BARREL_SWITCH:
			switches->stylus = on;
			break;
		case HID_DIGITIZER_USAGE_SECONDARY_BARREL:
			switches->stylus2 = on;
			break;
		default:
			break;
		}
	}
}

/* Chooses the tool one report describes, or zero when none is in range. */
static uint16_t
choose_tool(
	const struct hid_digitizer_state *state,
	const struct hid_digitizer_switches *switches)
{
	int present;

	/*
	 * A pen is present while In Range is set.  A device without In Range
	 * only reports a pen that touches the surface.
	 */
	present = 0;
	if (switches->in_range_present) {
		/* In Range alone decides presence. */
		if (switches->in_range)
			present = 1;
	} else if (switches->tip) {
		/* The tip touches the surface. */
		present = 1;
	} else if (switches->eraser) {
		/* The eraser end touches the surface. */
		present = 1;
	}

	/* Reports that no tool is in range. */
	if (!present)
		return 0;

	/* The eraser end is near when Invert is set. */
	if (switches->invert)
		return BTN_TOOL_RUBBER;

	/* An eraser contact is always the eraser end. */
	if (switches->eraser)
		return BTN_TOOL_RUBBER;

	/*
	 * A device without Invert learns of the eraser end only from its
	 * contact, so the eraser stays the tool until the pen leaves.
	 */
	if (!switches->invert_present && state->tool == BTN_TOOL_RUBBER)
		return BTN_TOOL_RUBBER;

	/* The writing end is in range. */
	return BTN_TOOL_PEN;
}

/* Appends one event to the list, refusing to overrun it. */
static int
append_event(
	struct hid_digitizer_output *output,
	uint16_t type,
	uint16_t code,
	int32_t value)
{
	struct hid_digitizer_event *event;

	/* Refuses an event the list has no room for. */
	if (output->event_count >= HID_DIGITIZER_EVENT_MAX)
		return E2BIG;

	/* Stores the event after the ones already listed. */
	event = &output->events[output->event_count];
	event->type = type;
	event->code = code;
	event->value = value;
	output->event_count++;

	/* Succeeded: the event is listed. */
	return 0;
}

/* Closes the frame of the tool that left: pressure, contact, buttons, tool. */
static int
emit_leave(
	struct hid_digitizer_state *state,
	const struct hid_digitizer_switches *switches,
	struct hid_digitizer_output *output)
{
	int error;

	/* Returns the pressure to zero so a reader never keeps a stale one. */
	if (switches->pressure_present) {
		/* Reports an event list too small for the frame. */
		error = append_event(output, EV_ABS, ABS_PRESSURE, 0);
		if (error != 0)
			return error;
	}

	/* Lifts the contact the old tool still held. */
	if (state->touch) {
		/* Reports an event list too small for the frame. */
		error = append_event(output, EV_KEY, BTN_TOUCH, 0);
		if (error != 0)
			return error;

		/* The contact is released: readers saw BTN_TOUCH go to zero. */
		state->touch = 0;
	}

	/* Releases the first side button the old tool still held. */
	if (state->stylus) {
		/* Reports an event list too small for the frame. */
		error = append_event(output, EV_KEY, BTN_STYLUS, 0);
		if (error != 0)
			return error;

		/* The button is released: readers saw BTN_STYLUS go to zero. */
		state->stylus = 0;
	}

	/* Releases the second side button the old tool still held. */
	if (state->stylus2) {
		/* Reports an event list too small for the frame. */
		error = append_event(output, EV_KEY, BTN_STYLUS2, 0);
		if (error != 0)
			return error;

		/* The button is released: readers saw BTN_STYLUS2 go to zero. */
		state->stylus2 = 0;
	}

	/* Takes the old tool out of range. */
	error = append_event(output, EV_KEY, state->tool, 0);
	if (error != 0)
		return error;

	/* No tool is in range until the next frame opens one. */
	state->tool = 0;

	/* Ends the leave frame. */
	error = append_event(output, EV_SYN, SYN_REPORT, 0);
	if (error != 0)
		return error;

	/* Succeeded: the old tool is closed. */
	return 0;
}

/* Writes the frame of a tool in range: tool, axes, contact, buttons. */
static int
emit_present(
	struct hid_digitizer_state *state,
	uint16_t tool,
	const struct hid_report_input *input,
	const struct hid_digitizer_switches *switches,
	struct hid_digitizer_output *output)
{
	const struct hid_report_value *value;
	uint8_t touch;
	size_t index;
	int error;

	/* Brings a tool that was not in range into range first. */
	if (state->tool != tool) {
		/* Reports an event list too small for the frame. */
		error = append_event(output, EV_KEY, tool, 1);
		if (error != 0)
			return error;

		/* The tool is in range: readers saw its BTN_TOOL go to one. */
		state->tool = tool;
	}

	/* Reports every absolute axis of the report: position, pressure, tilt. */
	for (index = 0; index < input->value_count; index++) {
		value = &input->values[index];

		/* Only absolute axes belong to the pen frame. */
		if (value->type != EV_ABS)
			continue;

		/* Reports an event list too small for the frame. */
		error = append_event(output, EV_ABS, value->code, value->value);
		if (error != 0)
			return error;
	}

	/* The pen touches with its tip, or with its eraser end. */
	touch = 0;
	if (switches->tip)
		touch = 1;
	else if (switches->eraser)
		touch = 1;

	/* Reports a contact that began or ended. */
	if (touch != state->touch) {
		/* Reports an event list too small for the frame. */
		error = append_event(output, EV_KEY, BTN_TOUCH, touch);
		if (error != 0)
			return error;

		/* Readers now hold this contact state. */
		state->touch = touch;
	}

	/* Reports a first side button that was pressed or released. */
	if ((uint8_t)switches->stylus != state->stylus) {
		/* Reports an event list too small for the frame. */
		error = append_event(output, EV_KEY, BTN_STYLUS, switches->stylus);
		if (error != 0)
			return error;

		/* Readers now hold this button state. */
		state->stylus = (uint8_t)switches->stylus;
	}

	/* Reports a second side button that was pressed or released. */
	if ((uint8_t)switches->stylus2 != state->stylus2) {
		/* Reports an event list too small for the frame. */
		error = append_event(output, EV_KEY, BTN_STYLUS2, switches->stylus2);
		if (error != 0)
			return error;

		/* Readers now hold this button state. */
		state->stylus2 = (uint8_t)switches->stylus2;
	}

	/* Ends the frame of the tool in range. */
	error = append_event(output, EV_SYN, SYN_REPORT, 0);
	if (error != 0)
		return error;

	/* Succeeded: the frame of the tool is listed. */
	return 0;
}
