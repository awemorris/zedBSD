/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch state machine of a USB HID touch screen (evdev multitouch
 * protocol B).
 */

#ifndef KERN_DRIVERS_HID_TOUCH_H
#define KERN_DRIVERS_HID_TOUCH_H

#include <drivers/generic/hid-report.h>
#include <kern/input-capability.h>

#include <stddef.h>
#include <stdint.h>

/*
 * The items of one finger that the report decoder hands on.  A value of type
 * HID_REPORT_TYPE_TOUCH carries HID_TOUCH_CODE(finger, item) as its code,
 * where finger is the Finger collection's place in its report (0 first).
 */
#define HID_TOUCH_ITEM_TIP		1U
#define HID_TOUCH_ITEM_CONFIDENCE	2U
#define HID_TOUCH_ITEM_CONTACT_ID	3U
#define HID_TOUCH_ITEM_X		4U
#define HID_TOUCH_ITEM_Y		5U

/* How many low bits of a touch code name the item; the finger is above them. */
#define HID_TOUCH_ITEM_BITS		4U

/* The code of one finger's item. */
#define HID_TOUCH_CODE(finger, item)	((uint16_t)(((finger) << HID_TOUCH_ITEM_BITS) | (item)))

/* The code of the report's Contact Count, which belongs to no finger. */
#define HID_TOUCH_CONTACT_COUNT_CODE	0x1000U

/* The code of the report's Scan Time, which belongs to no finger either. */
#define HID_TOUCH_SCAN_TIME_CODE	0x1001U

/*
 * The code of a touch pad's button n (0 the left, 1 the right, 2 the
 * middle), which belongs to no finger (ws159-p003).
 */
#define HID_TOUCH_BUTTON_CODE(n)	((uint16_t)(0x1002U + (n)))
#define HID_TOUCH_BUTTONS_MAX		3U

/* The unit of a Scan Time whose descriptor gives none of time (100 us, as Windows requires). */
#define HID_TOUCH_SCAN_TIME_UNIT_NS	100000U

/* After this long without a report (milliseconds) the Scan Time is counted from zero again. */
#define HID_TOUCH_SCAN_TIME_RESTART_MS	1000U

/* The time given to drv_hid_touch_translate_at() when the caller has none. */
#define HID_TOUCH_TIME_UNKNOWN		UINT64_MAX

/* The most Finger collections one report may carry; later ones are ignored. */
#define HID_TOUCH_CONTACTS_MAX		16U

/*
 * The slots a touch screen gets: at least ten fingers (a device that splits
 * a frame over reports may track more fingers than one report carries), and
 * never more than HID_TOUCH_SLOTS_MAX.
 */
#define HID_TOUCH_SLOTS_MIN		10U
#define HID_TOUCH_SLOTS_MAX		16U

/* The tracking identifiers run 0..HID_TOUCH_TRACKING_MAX and then wrap. */
#define HID_TOUCH_TRACKING_MAX		65535

/*
 * The most events one report can turn into: an unfinished frame closed early
 * and a whole frame, each at most four events a slot, BTN_TOUCH, ABS_X,
 * ABS_Y, MSC_TIMESTAMP and SYN_REPORT.
 */
#define HID_TOUCH_EVENT_MAX		(2U * (4U * HID_TOUCH_SLOTS_MAX + 5U + 2U + HID_TOUCH_BUTTONS_MAX))

/*
 * The capabilities (MSC_TIMESTAMP only with a Scan Time) and axes a touch
 * screen declares; a touch pad adds its five finger counts and its buttons.
 */
#define HID_TOUCH_CAPABILITY_COUNT	(9U + 5U + HID_TOUCH_BUTTONS_MAX)
#define HID_TOUCH_AXIS_COUNT		6U

/*
 * One slot: a finger on the screen, as its readers know it.
 *
 * A slot is active from the frame its finger touches until the frame it
 * lifts or is no longer reported.  The flags record what the frame being
 * built changed; they are cleared when the frame's events are written.
 */
struct hid_touch_slot {
	int32_t contact_id;
	int32_t tracking_id;
	int32_t x;
	int32_t y;
	/* Which finger touched first: a smaller age is an older finger. */
	uint32_t age;
	uint8_t active;
	/* Reported touching in the frame being built. */
	uint8_t seen;
	/* Touched in the frame being built: readers have not heard of it yet. */
	uint8_t opened;
	/* Lifted in the frame being built: readers still think it touches. */
	uint8_t closing;
	uint8_t moved_x;
	uint8_t moved_y;
	uint8_t reserved[2];
};

/*
 * What one touch screen has already told its readers, and the frame being
 * built from its reports.
 *
 * One instance lives with each touch screen from attach to detach.  The
 * state that drv_hid_touch_reset() leaves means that no finger touches.
 */
struct hid_touch_state {
	struct hid_touch_slot slots[HID_TOUCH_SLOTS_MAX];
	unsigned slot_count;
	/* The slot the readers last selected with ABS_MT_SLOT (-1: none yet). */
	int32_t current_slot;
	/* The tracking identifier the next finger gets. */
	int32_t next_tracking_id;
	/* The age the next finger gets. */
	uint32_t next_age;
	/* A frame split over reports: how many fingers it has, how many came. */
	uint32_t expected;
	uint32_t received;
	uint8_t in_frame;
	/* BTN_TOUCH as the readers know it. */
	uint8_t touching;
	/* Whether ABS_X and ABS_Y have been written, and their values. */
	uint8_t pointer_known;
	uint8_t reserved;
	int32_t pointer_x;
	int32_t pointer_y;
	/*
	 * The Scan Time, when drv_hid_touch_set_scan_time() said the screen
	 * has one: its wrap (logical maximum + 1) and unit, the last report's
	 * raw value and host time, the time elapsed on the screen's clock since
	 * it was last counted from zero, and the MSC_TIMESTAMP of the report
	 * being taken and of the frame being built (the frame's first report).
	 */
	uint8_t scan_time;
	uint8_t scan_seen;
	uint8_t scan_reserved[2];
	uint32_t scan_modulus;
	uint32_t scan_unit_ns;
	uint32_t scan_last;
	uint64_t scan_last_ms;
	uint64_t scan_elapsed_ns;
	uint32_t report_timestamp;
	uint32_t frame_timestamp;
	/*
	 * A touch pad (drv_hid_touch_set_pad()): the BTN_TOOL_* of the number
	 * of fingers the readers know (0: none), the buttons they know held
	 * (bit n for button n), and the buttons the reports say are held, which
	 * the next frame writes.
	 */
	uint8_t pad;
	uint8_t buttons;
	uint8_t pending_buttons;
	uint8_t pad_reserved;
	uint16_t tool;
};

/*
 * One event the touch state machine asks the input layer to emit.
 */
struct hid_touch_event {
	uint16_t type;
	uint16_t code;
	int32_t value;
};

/*
 * The ordered events one touch report produced.
 *
 * The caller owns the storage; the translation fills it from the start.
 */
struct hid_touch_output {
	size_t event_count;
	struct hid_touch_event events[HID_TOUCH_EVENT_MAX];
};

/*
 * What a touch screen device declares: its capabilities and axes, and the
 * number of slots its state machine keeps.
 *
 * The caller owns the storage; drv_hid_touch_describe() fills it.
 */
struct hid_touch_description {
	struct input_capability capabilities[HID_TOUCH_CAPABILITY_COUNT];
	size_t capability_count;
	struct input_abs_axis axes[HID_TOUCH_AXIS_COUNT];
	size_t axis_count;
	unsigned slots;
	/* The INPUT_PROP_* bits of the device (bit n for property n). */
	uint32_t properties;
};

int drv_hid_touch_describe(const struct hid_report_touch_info *, struct hid_touch_description *);
void drv_hid_touch_reset(struct hid_touch_state *, unsigned);
int drv_hid_touch_report_is_touch(const struct hid_report_input *);
int drv_hid_touch_translate(struct hid_touch_state *, const struct hid_report_input *, struct hid_touch_output *);
int drv_hid_touch_translate_at(struct hid_touch_state *, const struct hid_report_input *, uint64_t,
	struct hid_touch_output *);
void drv_hid_touch_set_scan_time(struct hid_touch_state *, const struct hid_report_touch_info *);
void drv_hid_touch_set_pad(struct hid_touch_state *, const struct hid_report_touch_info *);

#endif
