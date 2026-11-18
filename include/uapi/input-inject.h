/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/input-inject: the test-only pen, touch screen, mouse and keyboard
 * injector (CONFIG_INPUT_TEST_INJECT; test images only, root only).
 *
 * Every open declares one device, and up to INPUT_INJECT_OPENS_MAX opens
 * may be held at once (a mouse and a keyboard together, ws173).  The first
 * write on an open is one struct input_inject_setup.  Its kind chooses the
 * device:
 *
 * INPUT_INJECT_KIND_PEN declares a virtual pen: ABS_X 0..x_max, ABS_Y
 * 0..y_max, ABS_PRESSURE 0..4095, ABS_TILT_X/Y -60..60 degrees,
 * BTN_TOOL_PEN, BTN_TOOL_RUBBER, BTN_TOUCH, BTN_STYLUS and BTN_STYLUS2
 * (report_contacts and reserved are 0).  Every later write is an array of
 * struct input_event; each event must be one of the declared codes with a
 * value in its range, or the whole write is refused.  The pen is on a
 * display (INPUT_PROP_DIRECT).
 *
 * INPUT_INJECT_KIND_PEN_POINTER declares the same pen on a tablet beside
 * the display (INPUT_PROP_POINTER), named apart (WS217).
 *
 * INPUT_INJECT_KIND_TOUCH declares a virtual touch screen of ten slots
 * whose fingers lie in 0..x_max and 0..y_max (evdev multitouch protocol B:
 * ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X/Y, BTN_TOUCH, ABS_X and
 * ABS_Y).  Every later write is one struct input_inject_touch_frame: the
 * fingers of one frame, as a USB touch screen reports them (a finger left
 * out, or given with tip 0, lifts).  The kernel splits the frame into
 * reports of report_contacts fingers (1..INPUT_INJECT_TOUCH_CONTACTS; fewer
 * than the frame's fingers makes the reports a USB "hybrid" screen sends)
 * and runs them through the USB touch screen's state machine, so readers
 * see what a USB touch screen would give them.
 *
 * A touch setup whose reserved word has INPUT_INJECT_TOUCH_SCAN_TIME set
 * declares a screen with a Scan Time (its own clock, 100 us a unit,
 * wrapping at 65536): each frame's reserved word is then its Scan Time
 * (0..65535), and readers get MSC_TIMESTAMP before each SYN_REPORT, as from
 * a USB touch screen with one.  Without it a frame's reserved word is 0.
 * Every other bit of the setup's reserved word is 0.
 *
 * INPUT_INJECT_KIND_TOUCHPAD (ws159-p003) declares a virtual touch pad: the
 * same frames and fingers as the touch screen, run through the touch state
 * machine in touch pad mode (a Windows Precision Touchpad's), with one
 * button, the pad itself (a click pad).  Readers get the fingers in
 * protocol B, BTN_TOOL_FINGER to BTN_TOOL_QUINTTAP for how many touch,
 * BTN_LEFT, and the properties INPUT_PROP_POINTER and
 * INPUT_PROP_BUTTONPAD.  Its position axes have INPUT_INJECT_PAD_RESOLUTION
 * units a millimetre (a Latitude 5330's pad's).  A frame's reserved word holds its Scan Time in
 * its low 16 bits (on a pad declared with one, else 0) and the buttons
 * held in INPUT_INJECT_PAD_BUTTONS (bit 0 the left button).
 *
 * INPUT_INJECT_KIND_MOUSE (ws173-p001) declares a mouse with BTN_LEFT,
 * BTN_RIGHT, BTN_MIDDLE, REL_WHEEL and REL_HWHEEL, as a USB mouse reports
 * them.  With x_max and y_max both 0 it is a relative mouse (REL_X, REL_Y);
 * with both above 0 it is an absolute pointer (ABS_X 0..x_max, ABS_Y
 * 0..y_max, like QEMU's USB tablet), which the compositor maps onto the
 * output, so x_max and y_max one below the output's size in pixels make
 * the axes pixels.  report_contacts and reserved are 0.
 *
 * INPUT_INJECT_KIND_KEYBOARD (ws173-p001) declares a keyboard with every key
 * from KEY_ESC (1) to KEY_COMPOSE (127), KEY_F13 to KEY_F24, and the
 * brightness keys (224, 225) (x_max, y_max,
 * report_contacts and reserved are 0).  A key is pressed (1) or released
 * (0); repeat (2) is refused, since the compositor repeats a held key.
 *
 * Later writes on a mouse or a keyboard are arrays of struct input_event,
 * as on a pen: each event must be one the device declared (a REL_* value in
 * -INPUT_INJECT_REL_MAX..INPUT_INJECT_REL_MAX, an ABS_* value in its
 * range, a button or key 0 or 1, SYN_REPORT 0), or the whole write is
 * refused.  A report ends with SYN_REPORT, as a USB device's does.
 *
 * The device appears as /dev/input/eventN.  Closing the file removes it,
 * and the input layer releases what it held.
 */

#ifndef KERN_UAPI_INPUT_INJECT_H
#define KERN_UAPI_INPUT_INJECT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define INPUT_INJECT_MAGIC		0x6e706e69U
#define INPUT_INJECT_KIND_PEN		1U
#define INPUT_INJECT_KIND_TOUCH		2U
#define INPUT_INJECT_KIND_TOUCHPAD	3U
#define INPUT_INJECT_KIND_MOUSE		4U
#define INPUT_INJECT_KIND_KEYBOARD	5U
#define INPUT_INJECT_KIND_PEN_POINTER	6U

/* The most opens (devices) at once, and the largest relative movement of one event. */
#define INPUT_INJECT_OPENS_MAX		4U
#define INPUT_INJECT_REL_MAX		32767
#define INPUT_INJECT_PAD_RESOLUTION	12
#define INPUT_INJECT_AXIS_MAX		65535
#define INPUT_INJECT_PRESSURE_MAX	4095
#define INPUT_INJECT_TILT_MAX		60
#define INPUT_INJECT_EVENTS_MAX		64U

/* The most fingers one touch frame carries, and the largest contact identifier. */
#define INPUT_INJECT_TOUCH_CONTACTS	10U
#define INPUT_INJECT_CONTACT_ID_MAX	255

/* A touch setup's reserved word: the frames carry a Scan Time (100 us units, 0..INPUT_INJECT_SCAN_TIME_MAX). */
#define INPUT_INJECT_TOUCH_SCAN_TIME	1U
#define INPUT_INJECT_SCAN_TIME_MAX	65535U

/* A touch pad frame's reserved word: the buttons held, bit 0 the left one, above the Scan Time. */
#define INPUT_INJECT_PAD_BUTTONS_SHIFT	16U
#define INPUT_INJECT_PAD_BUTTONS_MASK	0x1U

/* The first write on an open: the device it declares. */
struct input_inject_setup {
	uint32_t magic;
	uint32_t kind;
	int32_t x_max;
	int32_t y_max;
	/* Touch: fingers per report (1..INPUT_INJECT_TOUCH_CONTACTS); pen: 0. */
	uint32_t report_contacts;
	/* Touch: 0 or INPUT_INJECT_TOUCH_SCAN_TIME; pen: 0. */
	uint32_t reserved;
};

/* One finger of a touch frame. */
struct input_inject_contact {
	/* The finger's contact identifier, 0..INPUT_INJECT_CONTACT_ID_MAX. */
	int32_t contact_id;
	/* 1 while the finger touches, 0 in the frame it lifts. */
	int32_t tip;
	int32_t x;
	int32_t y;
};

/* One frame of a touch screen: count fingers (0 lifts every finger). */
struct input_inject_touch_frame {
	uint32_t count;
	/* The frame's Scan Time on a screen declared with one; otherwise 0. */
	uint32_t reserved;
	struct input_inject_contact contacts[INPUT_INJECT_TOUCH_CONTACTS];
};

#ifdef __cplusplus
}
#endif

#endif
