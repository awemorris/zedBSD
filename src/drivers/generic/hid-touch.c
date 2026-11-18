/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch state machine of a USB HID touch screen.
 *
 * A touch screen reports each finger in a Finger collection of its own:
 * Tip Switch, often Confidence, a Contact Identifier and X and Y, and the
 * report says in Contact Count how many fingers the frame has.  A device
 * with fewer Finger collections than fingers splits one frame over several
 * reports ("hybrid" reporting): the first report carries the count, the
 * later ones a count of zero.  Readers of an evdev touch screen expect
 * multitouch protocol B instead: ABS_MT_SLOT selects a slot, a new
 * ABS_MT_TRACKING_ID opens a finger in it, ABS_MT_POSITION_X/Y move it, a
 * tracking identifier of -1 lifts it, BTN_TOUCH says whether any finger
 * touches, and ABS_X/ABS_Y follow the oldest finger for readers that know
 * only one.  This file turns decoded reports into that ordered event list.
 * A finger the frame no longer reports is lifted.
 *
 * A screen with a Scan Time (its own clock, in the report beside the
 * Contact Count) also gets MSC_TIMESTAMP before each SYN_REPORT: the time
 * on the screen's clock in microseconds, counted from zero after a second
 * without reports, so that a reader can tell when the screen scanned each
 * frame (plan/ws081/design.md section 2).  While a finger touches, such a
 * screen writes every frame, changed or not, so that the reports' pace
 * reaches the reader.  The file touches no hardware, so the host tests run
 * it unchanged.
 *
 * A touch pad (ws159-p003, a Windows Precision Touchpad over I2C-HID) is
 * the same fingers with two more things in each frame: the BTN_TOOL_* of
 * how many fingers touch (BTN_TOOL_FINGER for one up to
 * BTN_TOOL_QUINTTAP for five or more), and its buttons as BTN_LEFT,
 * BTN_RIGHT and BTN_MIDDLE.  Its readers move a pointer by the fingers;
 * the state machine makes no gesture of them.
 */

#include <drivers/generic/hid-touch.h>
#include <uapi/errno.h>
#include <uapi/input.h>

#include <stddef.h>
#include <stdint.h>

/*
 * One finger of one report, after the decoder has split it out.
 *
 * One instance lives on the stack, per finger, for the translation of one
 * report.
 */
struct touch_entry {
	uint8_t tip;
	uint8_t confidence_present;
	uint8_t confidence;
	uint8_t contact_id_present;
	uint8_t x_present;
	uint8_t y_present;
	uint8_t reserved[2];
	int32_t contact_id;
	int32_t x;
	int32_t y;
};

/*
 * The fingers, the Contact Count and the Scan Time of one report.
 *
 * One instance lives on the stack for the translation of one report.
 */
struct touch_report {
	struct touch_entry entries[HID_TOUCH_CONTACTS_MAX];
	/* One more than the highest finger the report has a field of. */
	unsigned finger_count;
	int count_present;
	int32_t count;
	int scan_present;
	int32_t scan;
	/* A touch pad's buttons in this report (bit n for button n), when it has any. */
	int buttons_present;
	uint8_t buttons;
};

static void collect_report(const struct hid_report_input *input, struct touch_report *report);
static int write_pad(struct hid_touch_state *state, struct hid_touch_output *output);
static uint16_t pad_tool(unsigned fingers);
static void scan_take(struct hid_touch_state *state, const struct touch_report *report, uint64_t milliseconds);
static void frame_begin(struct hid_touch_state *state);
static void take_entry(struct hid_touch_state *state, const struct touch_entry *entry, unsigned finger);
static struct hid_touch_slot * slot_of_contact(struct hid_touch_state *state, int32_t contact_id);
static struct hid_touch_slot * free_slot(struct hid_touch_state *state);
static int frame_write(struct hid_touch_state *state, int lift_unseen, struct hid_touch_output *output);
static int write_slot(struct hid_touch_state *state, unsigned index, struct hid_touch_output *output);
static int select_slot(struct hid_touch_state *state, unsigned index, struct hid_touch_output *output);
static int write_pointer(struct hid_touch_state *state, struct hid_touch_output *output);
static int append_event(struct hid_touch_output *output, uint16_t type, uint16_t code, int32_t value);
static void describe_capability(struct hid_touch_description *description, uint16_t type, uint16_t code);
static void describe_axis(struct hid_touch_description *description, uint16_t code, const struct input_absinfo *info);

/*
 * Describes the device a touch screen is published as.
 *
 * The capabilities are the report boundary, BTN_TOUCH, ABS_X and ABS_Y (the
 * oldest finger), and the protocol B axes: ABS_MT_SLOT, ABS_MT_TRACKING_ID
 * and ABS_MT_POSITION_X/Y, whose ranges and resolutions are those of the
 * fingers' X and Y.  A screen without a position range is refused.  A
 * screen is a direct device; a touch pad is a pointer that also declares
 * its finger counts and buttons, and a button pad when its only button is
 * the pad itself (a click pad).
 */
int
drv_hid_touch_describe(
	const struct hid_report_touch_info *info,
	struct hid_touch_description *description)
{
	struct input_absinfo range;
	unsigned slots;

	/* Refuses a screen with no finger or with an empty position range. */
	if (info->contacts == 0U)
		return EINVAL;
	if (info->x.minimum >= info->x.maximum)
		return EINVAL;
	if (info->y.minimum >= info->y.maximum)
		return EINVAL;

	/*
	 * One slot per finger a report carries; a screen that counts its
	 * fingers may split a frame and track more of them than that.
	 */
	slots = (unsigned)info->contacts;
	if (info->count_present && slots < HID_TOUCH_SLOTS_MIN)
		slots = HID_TOUCH_SLOTS_MIN;
	if (slots > HID_TOUCH_SLOTS_MAX)
		slots = HID_TOUCH_SLOTS_MAX;
	description->slots = slots;

	/* Declares the events the state machine writes. */
	description->capability_count = 0;
	describe_capability(description, EV_SYN, SYN_REPORT);
	describe_capability(description, EV_KEY, BTN_TOUCH);
	describe_capability(description, EV_ABS, ABS_X);
	describe_capability(description, EV_ABS, ABS_Y);
	describe_capability(description, EV_ABS, ABS_MT_SLOT);
	describe_capability(description, EV_ABS, ABS_MT_TRACKING_ID);
	describe_capability(description, EV_ABS, ABS_MT_POSITION_X);
	describe_capability(description, EV_ABS, ABS_MT_POSITION_Y);

	/* A screen with a Scan Time also stamps its frames with it. */
	if (info->scan_time_present)
		describe_capability(description, EV_MSC, MSC_TIMESTAMP);

	/* A screen is touched where it shows. */
	description->properties = 1U << INPUT_PROP_DIRECT;

	/* A touch pad: the counts of its fingers, its buttons, and what kind of pointer it is. */
	if (info->pad) {
		describe_capability(description, EV_KEY, BTN_TOOL_FINGER);
		describe_capability(description, EV_KEY, BTN_TOOL_DOUBLETAP);
		describe_capability(description, EV_KEY, BTN_TOOL_TRIPLETAP);
		describe_capability(description, EV_KEY, BTN_TOOL_QUADTAP);
		describe_capability(description, EV_KEY, BTN_TOOL_QUINTTAP);

		/* Its buttons, left first. */
		if (info->buttons >= 1U)
			describe_capability(description, EV_KEY, BTN_LEFT);
		if (info->buttons >= 2U)
			describe_capability(description, EV_KEY, BTN_RIGHT);
		if (info->buttons >= 3U)
			describe_capability(description, EV_KEY, BTN_MIDDLE);

		/* A pointer; one with a single button is pressed as a whole (a click pad). */
		description->properties = 1U << INPUT_PROP_POINTER;
		if (info->buttons == 1U)
			description->properties |= 1U << INPUT_PROP_BUTTONPAD;
	}

	/* The oldest finger's position, over the fingers' own range. */
	description->axis_count = 0;
	describe_axis(description, ABS_X, &info->x);
	describe_axis(description, ABS_Y, &info->y);

	/* The slots, numbered from zero. */
	range.value = 0;
	range.minimum = 0;
	range.maximum = (int32_t)slots - 1;
	range.fuzz = 0;
	range.flat = 0;
	range.resolution = 0;
	describe_axis(description, ABS_MT_SLOT, &range);

	/* The tracking identifiers; -1, which lifts a finger, lies below them. */
	range.maximum = HID_TOUCH_TRACKING_MAX;
	describe_axis(description, ABS_MT_TRACKING_ID, &range);

	/* Each finger's position. */
	describe_axis(description, ABS_MT_POSITION_X, &info->x);
	describe_axis(description, ABS_MT_POSITION_Y, &info->y);

	/* Succeeded: the description is complete. */
	return 0;
}

/*
 * Forgets every finger a touch screen has reported.
 *
 * The next report is then treated as the first one after attach.  The
 * screen keeps slot_count slots (at least one, at most HID_TOUCH_SLOTS_MAX).
 */
void
drv_hid_touch_reset(
	struct hid_touch_state *state,
	unsigned slot_count)
{
	unsigned index;

	/* Empties every slot. */
	for (index = 0; index < HID_TOUCH_SLOTS_MAX; index++) {
		state->slots[index].contact_id = 0;
		state->slots[index].tracking_id = -1;
		state->slots[index].x = 0;
		state->slots[index].y = 0;
		state->slots[index].age = 0;
		state->slots[index].active = 0;
		state->slots[index].seen = 0;
		state->slots[index].opened = 0;
		state->slots[index].closing = 0;
		state->slots[index].moved_x = 0;
		state->slots[index].moved_y = 0;
		state->slots[index].reserved[0] = 0;
		state->slots[index].reserved[1] = 0;
	}

	/* Keeps the number of slots within what the state holds. */
	if (slot_count == 0U)
		slot_count = 1U;
	if (slot_count > HID_TOUCH_SLOTS_MAX)
		slot_count = HID_TOUCH_SLOTS_MAX;
	state->slot_count = slot_count;

	/* No slot is selected, no finger touches, no frame is open. */
	state->current_slot = -1;
	state->next_tracking_id = 0;
	state->next_age = 0;
	state->expected = 0;
	state->received = 0;
	state->in_frame = 0;
	state->touching = 0;
	state->pointer_known = 0;
	state->reserved = 0;
	state->pointer_x = 0;
	state->pointer_y = 0;

	/* No Scan Time until drv_hid_touch_set_scan_time() gives one. */
	state->scan_time = 0;
	state->scan_seen = 0;
	state->scan_reserved[0] = 0;
	state->scan_reserved[1] = 0;
	state->scan_modulus = 0;
	state->scan_unit_ns = 0;
	state->scan_last = 0;
	state->scan_last_ms = 0;
	state->scan_elapsed_ns = 0;
	state->report_timestamp = 0;
	state->frame_timestamp = 0;

	/* A touch screen until drv_hid_touch_set_pad() says otherwise; no button is held. */
	state->pad = 0;
	state->buttons = 0;
	state->pending_buttons = 0;
	state->pad_reserved = 0;
	state->tool = 0;
}

/*
 * Tells the state machine whether the device is a touch pad, from the
 * description of its reports (after drv_hid_touch_reset(), which makes it
 * a touch screen).
 */
void
drv_hid_touch_set_pad(
	struct hid_touch_state *state,
	const struct hid_report_touch_info *info)
{
	/* No finger count and no button is known to the readers yet. */
	state->pad = 0;
	state->buttons = 0;
	state->pending_buttons = 0;
	state->tool = 0;

	/* A touch pad writes its finger count and its buttons with each frame. */
	if (info != NULL && info->pad)
		state->pad = 1;
}

/*
 * Tells the state machine the screen's Scan Time, from the description of
 * its reports (after drv_hid_touch_reset(), which forgets it).
 *
 * A screen without one, or with a Scan Time that cannot count (a logical
 * maximum below one), writes no MSC_TIMESTAMP.
 */
void
drv_hid_touch_set_scan_time(
	struct hid_touch_state *state,
	const struct hid_report_touch_info *info)
{
	/* Nothing is counted until a Scan Time is known. */
	state->scan_time = 0;
	state->scan_seen = 0;
	state->scan_elapsed_ns = 0;
	state->report_timestamp = 0;
	state->frame_timestamp = 0;

	/* A screen without a Scan Time that counts writes none. */
	if (info == NULL || !info->scan_time_present)
		return;
	if (info->scan_time_maximum < 1)
		return;

	/* The Scan Time wraps after its logical maximum, in its unit (100 us when none of time is given). */
	state->scan_modulus = (uint32_t)info->scan_time_maximum + 1U;
	state->scan_unit_ns = info->scan_time_unit_ns;
	if (state->scan_unit_ns == 0U)
		state->scan_unit_ns = HID_TOUCH_SCAN_TIME_UNIT_NS;
	state->scan_time = 1;
}

/*
 * Asks whether a decoded report carries fingers of a touch screen.
 *
 * A device with a pen and a touch screen sends them in different reports;
 * only a report with a touch value takes the touch path.
 */
int
drv_hid_touch_report_is_touch(
	const struct hid_report_input *input)
{
	size_t index;

	/* Looks for any value that only a touch screen produces. */
	for (index = 0; index < input->value_count; index++) {
		/* A touch value marks the whole report as a touch report. */
		if (input->values[index].type == HID_REPORT_TYPE_TOUCH)
			return 1;
	}

	/* No touch value: the report belongs to another collection. */
	return 0;
}

/*
 * Turns one decoded touch report into the ordered evdev events, without a
 * host time (a Scan Time is then never counted from zero again).
 */
int
drv_hid_touch_translate(
	struct hid_touch_state *state,
	const struct hid_report_input *input,
	struct hid_touch_output *output)
{
	int error;

	/* The report, with no time of its own. */
	error = drv_hid_touch_translate_at(state, input, HID_TOUCH_TIME_UNKNOWN, output);
	if (error != 0)
		return error;

	/* Succeeded: the report's events are in output. */
	return 0;
}

/*
 * Turns one decoded touch report into the ordered evdev events.
 *
 * With a Contact Count, a report whose count is not zero starts a frame of
 * that many fingers, and later reports with a count of zero go on with it;
 * the frame is written when its last finger has come.  A new count while a
 * frame is still open (a report was lost) writes the open frame first, as
 * it stands, without lifting the fingers it did not get to.  Without a
 * Contact Count every report is a whole frame of the fingers whose tip
 * touches.  Either way a finger a whole frame does not report touching is
 * lifted, and a frame that changed nothing writes nothing (unless the
 * screen has a Scan Time and a finger touches).  milliseconds is when the
 * report arrived (HID_TOUCH_TIME_UNKNOWN when the caller does not know);
 * a second without reports counts the Scan Time from zero again.
 */
int
drv_hid_touch_translate_at(
	struct hid_touch_state *state,
	const struct hid_report_input *input,
	uint64_t milliseconds,
	struct hid_touch_output *output)
{
	struct touch_report report;
	unsigned finger;
	int error;

	/* Starts an empty event list and reads the report's fingers. */
	output->event_count = 0;
	collect_report(input, &report);

	/* Moves the screen's clock on to this report. */
	scan_take(state, &report, milliseconds);

	/* A touch pad's buttons are written with the frame this report ends or goes on with. */
	if (state->pad && report.buttons_present)
		state->pending_buttons = report.buttons;

	/* Without a Contact Count the report is a whole frame. */
	if (!report.count_present) {
		frame_begin(state);
		for (finger = 0; finger < report.finger_count; finger++)
			take_entry(state, &report.entries[finger], finger);

		/* Reports an event list too small for the frame. */
		error = frame_write(state, 1, output);
		if (error != 0)
			return error;

		/* Succeeded: the frame is written. */
		return 0;
	}

	/*
	 * A count starts a frame, closing one a lost report left open; a count
	 * of zero goes on with the open frame, or is a frame with no finger.
	 */
	if (report.count > 0) {
		/* Writes what the open frame has, lifting nobody. */
		if (state->in_frame) {
			error = frame_write(state, 0, output);
			if (error != 0)
				return error;
		}

		/* The new frame expects the counted fingers. */
		frame_begin(state);
		state->expected = (uint32_t)report.count;
		state->in_frame = 1;
	} else if (!state->in_frame) {
		/* No finger touches any more. */
		frame_begin(state);
		state->expected = 0;
		state->in_frame = 1;
	}

	/* Takes the fingers of this report that the frame still expects. */
	for (finger = 0; finger < report.finger_count; finger++) {
		/* The fingers past the count are unused entries. */
		if (state->received >= state->expected)
			break;
		take_entry(state, &report.entries[finger], finger);
		state->received++;
	}

	/* The frame goes on in the next report. */
	if (state->received < state->expected)
		return 0;

	/* The last finger came: the frame is written. */
	state->in_frame = 0;
	error = frame_write(state, 1, output);
	if (error != 0)
		return error;

	/* Succeeded: the frame is written. */
	return 0;
}

/* Splits the fingers and the Contact Count of one report out of its decoded values. */
static void
collect_report(
	const struct hid_report_input *input,
	struct touch_report *report)
{
	const struct hid_report_value *value;
	struct touch_entry *entry;
	uint16_t button_first;
	uint16_t button_end;
	unsigned finger;
	unsigned item;
	size_t index;

	/* Starts from a report that has no finger and no count. */
	for (finger = 0; finger < HID_TOUCH_CONTACTS_MAX; finger++) {
		entry = &report->entries[finger];
		entry->tip = 0;
		entry->confidence_present = 0;
		entry->confidence = 0;
		entry->contact_id_present = 0;
		entry->x_present = 0;
		entry->y_present = 0;
		entry->reserved[0] = 0;
		entry->reserved[1] = 0;
		entry->contact_id = 0;
		entry->x = 0;
		entry->y = 0;
	}

	/* No finger has a field yet, and no count has come. */
	report->finger_count = 0;
	report->count_present = 0;
	report->count = 0;
	report->scan_present = 0;
	report->scan = 0;
	report->buttons_present = 0;
	report->buttons = 0;

	/* The codes of a touch pad's buttons, from the first to past the last. */
	button_first = HID_TOUCH_BUTTON_CODE(0);
	button_end = HID_TOUCH_BUTTON_CODE(HID_TOUCH_BUTTONS_MAX);

	/* Stores each touch value under its finger and its item. */
	for (index = 0; index < input->value_count; index++) {
		value = &input->values[index];

		/* Only the touch values belong to the fingers. */
		if (value->type != HID_REPORT_TYPE_TOUCH)
			continue;

		/* The Contact Count belongs to the whole report. */
		if (value->code == HID_TOUCH_CONTACT_COUNT_CODE) {
			report->count_present = 1;
			report->count = value->value;
			continue;
		}

		/* So does the Scan Time. */
		if (value->code == HID_TOUCH_SCAN_TIME_CODE) {
			report->scan_present = 1;
			report->scan = value->value;
			continue;
		}

		/* And a touch pad's buttons, one bit each. */
		if (value->code >= button_first && value->code < button_end) {
			report->buttons_present = 1;
			if (value->value != 0)
				report->buttons |= (uint8_t)(1U << (value->code - button_first));
			continue;
		}

		/* A finger past what a report can carry is ignored. */
		finger = (unsigned)value->code >> HID_TOUCH_ITEM_BITS;
		item = (unsigned)value->code & ((1U << HID_TOUCH_ITEM_BITS) - 1U);
		if (finger >= HID_TOUCH_CONTACTS_MAX)
			continue;

		/* The finger count covers every finger with a field. */
		if (finger + 1U > report->finger_count)
			report->finger_count = finger + 1U;

		/* Stores the item under its meaning. */
		entry = &report->entries[finger];
		switch (item) {
		case HID_TOUCH_ITEM_TIP:
			entry->tip = 0;
			if (value->value != 0)
				entry->tip = 1;
			break;
		case HID_TOUCH_ITEM_CONFIDENCE:
			entry->confidence_present = 1;
			entry->confidence = 0;
			if (value->value != 0)
				entry->confidence = 1;
			break;
		case HID_TOUCH_ITEM_CONTACT_ID:
			entry->contact_id_present = 1;
			entry->contact_id = value->value;
			break;
		case HID_TOUCH_ITEM_X:
			entry->x_present = 1;
			entry->x = value->value;
			break;
		case HID_TOUCH_ITEM_Y:
			entry->y_present = 1;
			entry->y = value->value;
			break;
		default:
			break;
		}
	}
}

/*
 * Counts the screen's clock on to one report: the steps of its Scan Time
 * since the last report (across a wrap), in nanoseconds.  The first report,
 * and a report after a second of silence, count from zero again.
 */
static void
scan_take(
	struct hid_touch_state *state,
	const struct touch_report *report,
	uint64_t milliseconds)
{
	uint32_t raw;
	uint32_t step;
	int restart;

	/* Nothing to count without a Scan Time, or in a report without one. */
	if (!state->scan_time)
		return;
	if (!report->scan_present)
		return;

	/* The raw value within the Scan Time's range. */
	raw = (uint32_t)report->scan % state->scan_modulus;

	/* The first report, or one after a second of silence, starts the count again. */
	restart = 0;
	if (!state->scan_seen) {
		restart = 1;
	} else if (milliseconds != HID_TOUCH_TIME_UNKNOWN &&
		   state->scan_last_ms != HID_TOUCH_TIME_UNKNOWN &&
		   milliseconds >= state->scan_last_ms &&
		   milliseconds - state->scan_last_ms >= HID_TOUCH_SCAN_TIME_RESTART_MS) {
		restart = 1;
	}

	/* Counts from zero, or adds the steps since the last report. */
	if (restart) {
		state->scan_elapsed_ns = 0;
	} else {
		step = raw - state->scan_last;
		if (raw < state->scan_last)
			step = raw + (state->scan_modulus - state->scan_last);
		state->scan_elapsed_ns += (uint64_t)step * state->scan_unit_ns;
	}

	/* Remembers this report and its time on the screen's clock, in microseconds (wrapping). */
	state->scan_last = raw;
	state->scan_last_ms = milliseconds;
	state->scan_seen = 1;
	state->report_timestamp = (uint32_t)(state->scan_elapsed_ns / 1000U);
}

/* Starts building a frame: no finger has been reported touching in it yet. */
static void
frame_begin(
	struct hid_touch_state *state)
{
	unsigned index;

	/* Clears what the last frame saw. */
	for (index = 0; index < state->slot_count; index++)
		state->slots[index].seen = 0;

	/* No finger of the frame has come yet; the frame is stamped with this report's time. */
	state->expected = 0;
	state->received = 0;
	state->frame_timestamp = state->report_timestamp;
}

/*
 * Takes one finger of the frame: a touching finger keeps its slot, or opens
 * a free one; a finger that does not touch is left for the end of the frame
 * to lift.
 */
static void
take_entry(
	struct hid_touch_state *state,
	const struct touch_entry *entry,
	unsigned finger)
{
	struct hid_touch_slot *slot;
	int32_t contact_id;

	/* A finger without a position is not a finger the screen can place. */
	if (!entry->x_present)
		return;
	if (!entry->y_present)
		return;

	/* A finger that lifted, or that the screen takes for a palm, does not touch. */
	if (!entry->tip)
		return;
	if (entry->confidence_present && !entry->confidence)
		return;

	/* A screen without Contact Identifiers names a finger by its place. */
	contact_id = (int32_t)finger;
	if (entry->contact_id_present)
		contact_id = entry->contact_id;

	/* A finger that touched before moves in its slot. */
	slot = slot_of_contact(state, contact_id);
	if (slot != NULL) {
		/* A new X is written with the frame. */
		if (slot->x != entry->x) {
			slot->x = entry->x;
			slot->moved_x = 1;
		}

		/* A new Y is written with the frame. */
		if (slot->y != entry->y) {
			slot->y = entry->y;
			slot->moved_y = 1;
		}

		/* The finger touches in this frame, so the frame's end keeps it. */
		slot->seen = 1;
		return;
	}

	/* A new finger takes a free slot; with none free it is not tracked. */
	slot = free_slot(state);
	if (slot == NULL)
		return;

	/*
	 * The new finger's tracking identifier is the next one, and it is the
	 * youngest finger; readers hear of it when the frame is written.
	 */
	slot->contact_id = contact_id;
	slot->tracking_id = state->next_tracking_id;
	slot->x = entry->x;
	slot->y = entry->y;
	slot->age = state->next_age;
	slot->active = 1;
	slot->seen = 1;
	slot->opened = 1;
	slot->moved_x = 0;
	slot->moved_y = 0;
	state->next_age++;

	/* The identifiers wrap after HID_TOUCH_TRACKING_MAX. */
	state->next_tracking_id++;
	if (state->next_tracking_id > HID_TOUCH_TRACKING_MAX)
		state->next_tracking_id = 0;
}

/* Finds the slot of a finger that touches; NULL when it has none. */
static struct hid_touch_slot *
slot_of_contact(
	struct hid_touch_state *state,
	int32_t contact_id)
{
	struct hid_touch_slot *slot;
	unsigned index;

	/* Looks among the touching fingers. */
	for (index = 0; index < state->slot_count; index++) {
		slot = &state->slots[index];

		/* A slot without a finger, or with another one, is not it. */
		if (!slot->active)
			continue;
		if (slot->contact_id != contact_id)
			continue;

		/* Succeeded: the finger's slot. */
		return slot;
	}

	/* The finger did not touch before. */
	return NULL;
}

/*
 * Finds the lowest slot a new finger can take; NULL when every slot is
 * taken.  A slot whose finger lifted in the frame being built stays taken
 * until the frame is written, so readers hear the lift before the new
 * finger.
 */
static struct hid_touch_slot *
free_slot(
	struct hid_touch_state *state)
{
	struct hid_touch_slot *slot;
	unsigned index;

	/* Looks for a slot with no finger and no lift to write. */
	for (index = 0; index < state->slot_count; index++) {
		slot = &state->slots[index];

		/* A slot with a finger, or with its lift still to write, is taken. */
		if (slot->active)
			continue;
		if (slot->closing)
			continue;

		/* Succeeded: the slot is free. */
		return slot;
	}

	/* Every slot is taken. */
	return NULL;
}

/*
 * Writes the frame built so far: the slots that changed in slot order, then
 * BTN_TOUCH, then the oldest finger's ABS_X and ABS_Y, and SYN_REPORT when
 * anything was written.  With lift_unseen, a finger the frame did not report
 * touching is lifted first.
 */
static int
frame_write(
	struct hid_touch_state *state,
	int lift_unseen,
	struct hid_touch_output *output)
{
	struct hid_touch_slot *slot;
	size_t first_event;
	unsigned index;
	uint8_t touching;
	int error;

	/*
	 * A finger the frame did not report touching has lifted: its slot is
	 * given back once readers hear the lift.
	 */
	if (lift_unseen) {
		for (index = 0; index < state->slot_count; index++) {
			slot = &state->slots[index];
			if (!slot->active || slot->seen)
				continue;
			slot->active = 0;
			slot->closing = 1;
		}
	}

	/* Writes each slot the frame changed. */
	first_event = output->event_count;
	for (index = 0; index < state->slot_count; index++) {
		/* Reports an event list too small for the frame. */
		error = write_slot(state, index, output);
		if (error != 0)
			return error;
	}

	/* BTN_TOUCH is set while any finger touches. */
	touching = 0;
	for (index = 0; index < state->slot_count; index++) {
		if (state->slots[index].active)
			touching = 1;
	}

	/* Reports a first finger down or a last finger up. */
	if (touching != state->touching) {
		error = append_event(output, EV_KEY, BTN_TOUCH, touching);
		if (error != 0)
			return error;

		/* Readers now hold this contact state. */
		state->touching = touching;
	}

	/* ABS_X and ABS_Y follow the oldest finger. */
	error = write_pointer(state, output);
	if (error != 0)
		return error;

	/* A touch pad's finger count and buttons. */
	if (state->pad) {
		error = write_pad(state, output);
		if (error != 0)
			return error;
	}

	/*
	 * A screen with a Scan Time stamps a frame that changed something,
	 * and every frame while a finger touches: the pace of its reports is
	 * news to a reader even when nothing moved.
	 */
	if (state->scan_time &&
	    (output->event_count != first_event ||
	     state->touching)) {
		error = append_event(output, EV_MSC, MSC_TIMESTAMP, (int32_t)state->frame_timestamp);
		if (error != 0)
			return error;
	}

	/* A frame that changed nothing writes nothing. */
	if (output->event_count == first_event)
		return 0;

	/* Ends the frame. */
	error = append_event(output, EV_SYN, SYN_REPORT, 0);
	if (error != 0)
		return error;

	/* Succeeded: the frame is written. */
	return 0;
}

/*
 * Writes what the frame changed in one slot: a lift (tracking identifier
 * -1), a new finger (its tracking identifier and position), or a move.
 */
static int
write_slot(
	struct hid_touch_state *state,
	unsigned index,
	struct hid_touch_output *output)
{
	struct hid_touch_slot *slot;
	int error;

	/* A slot the frame did not change writes nothing. */
	slot = &state->slots[index];
	if (!slot->closing &&
	    !slot->opened &&
	    !slot->moved_x &&
	    !slot->moved_y)
		return 0;

	/* Readers select the slot first. */
	error = select_slot(state, index, output);
	if (error != 0)
		return error;

	/* A finger that lifted ends its tracking identifier; the slot is free. */
	if (slot->closing) {
		error = append_event(output, EV_ABS, ABS_MT_TRACKING_ID, -1);
		if (error != 0)
			return error;

		/* The lift is written: the slot may take a new finger. */
		slot->closing = 0;
		slot->tracking_id = -1;
		slot->moved_x = 0;
		slot->moved_y = 0;
		return 0;
	}

	/* A new finger opens its tracking identifier and gives its whole position. */
	if (slot->opened) {
		error = append_event(output, EV_ABS, ABS_MT_TRACKING_ID, slot->tracking_id);
		if (error != 0)
			return error;
		error = append_event(output, EV_ABS, ABS_MT_POSITION_X, slot->x);
		if (error != 0)
			return error;
		error = append_event(output, EV_ABS, ABS_MT_POSITION_Y, slot->y);
		if (error != 0)
			return error;

		/* Readers now know the finger. */
		slot->opened = 0;
		slot->moved_x = 0;
		slot->moved_y = 0;
		return 0;
	}

	/* A finger that moved gives the coordinates that changed. */
	if (slot->moved_x) {
		error = append_event(output, EV_ABS, ABS_MT_POSITION_X, slot->x);
		if (error != 0)
			return error;

		/* Readers now hold this position. */
		slot->moved_x = 0;
	}

	/* Its other coordinate. */
	if (slot->moved_y) {
		error = append_event(output, EV_ABS, ABS_MT_POSITION_Y, slot->y);
		if (error != 0)
			return error;

		/* Readers now hold this position. */
		slot->moved_y = 0;
	}

	/* Succeeded: the slot's changes are written. */
	return 0;
}

/* Selects a slot for the events that follow, unless readers have it selected. */
static int
select_slot(
	struct hid_touch_state *state,
	unsigned index,
	struct hid_touch_output *output)
{
	int error;

	/* The readers' slot is already this one. */
	if (state->current_slot == (int32_t)index)
		return 0;

	/* Reports an event list too small for the selection. */
	error = append_event(output, EV_ABS, ABS_MT_SLOT, (int32_t)index);
	if (error != 0)
		return error;

	/* Readers now have this slot selected. */
	state->current_slot = (int32_t)index;

	/* Succeeded: the slot is selected. */
	return 0;
}

/* Writes ABS_X and ABS_Y for the oldest finger when they changed. */
static int
write_pointer(
	struct hid_touch_state *state,
	struct hid_touch_output *output)
{
	struct hid_touch_slot *oldest;
	struct hid_touch_slot *slot;
	unsigned index;
	int error;

	/* Finds the finger that has touched the longest. */
	oldest = NULL;
	for (index = 0; index < state->slot_count; index++) {
		slot = &state->slots[index];
		if (!slot->active)
			continue;
		if (oldest != NULL && slot->age >= oldest->age)
			continue;
		oldest = slot;
	}

	/* With no finger on the screen the pointer stays where it was. */
	if (oldest == NULL)
		return 0;

	/* Its X, when readers do not hold it. */
	if (!state->pointer_known || state->pointer_x != oldest->x) {
		error = append_event(output, EV_ABS, ABS_X, oldest->x);
		if (error != 0)
			return error;

		/* Readers now hold this X. */
		state->pointer_x = oldest->x;
	}

	/* Its Y, when readers do not hold it. */
	if (!state->pointer_known || state->pointer_y != oldest->y) {
		error = append_event(output, EV_ABS, ABS_Y, oldest->y);
		if (error != 0)
			return error;

		/* Readers now hold this Y. */
		state->pointer_y = oldest->y;
	}

	/* Readers now hold both coordinates. */
	state->pointer_known = 1;

	/* Succeeded: the pointer is written. */
	return 0;
}

/*
 * Writes a touch pad's BTN_TOOL_* when the number of touching fingers
 * changed (the old count's tool released before the new one is pressed),
 * then each button whose state changed.
 */
static int
write_pad(
	struct hid_touch_state *state,
	struct hid_touch_output *output)
{
	unsigned fingers;
	unsigned index;
	unsigned bit;
	uint16_t tool;
	int32_t pressed;
	int error;

	/* Counts the fingers that touch. */
	fingers = 0;
	for (index = 0; index < state->slot_count; index++) {
		if (state->slots[index].active)
			fingers++;
	}

	/* The tool of that count, or none. */
	tool = pad_tool(fingers);
	if (tool != state->tool) {
		/* Releases the count the readers knew. */
		if (state->tool != 0U) {
			error = append_event(output, EV_KEY, state->tool, 0);
			if (error != 0)
				return error;
		}

		/* Presses the new count. */
		if (tool != 0U) {
			error = append_event(output, EV_KEY, tool, 1);
			if (error != 0)
				return error;
		}

		/* Readers now know this count. */
		state->tool = tool;
	}

	/* Writes each button that changed, left first. */
	for (index = 0; index < HID_TOUCH_BUTTONS_MAX; index++) {
		/* A button whose state the readers know already writes nothing. */
		bit = 1U << index;
		if ((state->buttons & bit) == (state->pending_buttons & bit))
			continue;

		/* The button's new state: pressed or released. */
		pressed = 0;
		if ((state->pending_buttons & bit) != 0U)
			pressed = 1;
		error = append_event(output, EV_KEY, (uint16_t)(BTN_LEFT + index), pressed);
		if (error != 0)
			return error;

		/* Readers now know the button's state. */
		state->buttons = (uint8_t)(state->buttons ^ bit);
	}

	/* Succeeded: the readers know the count and the buttons. */
	return 0;
}

/* Gives the BTN_TOOL_* of a number of touching fingers; 0 for none. */
static uint16_t
pad_tool(
	unsigned fingers)
{
	/* One BTN_TOOL_* for each count up to four, and one for five or more. */
	switch (fingers) {
	case 0:
		return 0;
	case 1:
		return BTN_TOOL_FINGER;
	case 2:
		return BTN_TOOL_DOUBLETAP;
	case 3:
		return BTN_TOOL_TRIPLETAP;
	case 4:
		return BTN_TOOL_QUADTAP;
	default:
		return BTN_TOOL_QUINTTAP;
	}
}

/* Appends one event to the list, refusing to overrun it. */
static int
append_event(
	struct hid_touch_output *output,
	uint16_t type,
	uint16_t code,
	int32_t value)
{
	struct hid_touch_event *event;

	/* Refuses an event the list has no room for. */
	if (output->event_count >= HID_TOUCH_EVENT_MAX)
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

/* Adds one capability to a description (the caller keeps within its table). */
static void
describe_capability(
	struct hid_touch_description *description,
	uint16_t type,
	uint16_t code)
{
	struct input_capability *capability;

	/* Stores the capability after the ones already declared. */
	capability = &description->capabilities[description->capability_count];
	capability->type = type;
	capability->code = code;
	description->capability_count++;
}

/* Adds one absolute axis to a description (the caller keeps within its table). */
static void
describe_axis(
	struct hid_touch_description *description,
	uint16_t code,
	const struct input_absinfo *info)
{
	struct input_abs_axis *axis;

	/* Stores the axis after the ones already declared, at rest at its minimum. */
	axis = &description->axes[description->axis_count];
	axis->code = code;
	axis->info = *info;
	axis->info.value = info->minimum;
	description->axis_count++;
}
