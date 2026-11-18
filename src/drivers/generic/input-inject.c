/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/input-inject: a test-only pen, touch screen, touch pad, mouse or
 * keyboard for machines without one (QEMU), or driven by an agent over SSH
 * on a real one (AAT, ws173).
 *
 * Only root may open the node, and at most INPUT_INJECT_OPENS_MAX opens at
 * a time, each its own device.  The first write declares the device
 * (include/uapi/input-inject.h): one of the fixed shapes, so no other
 * device type can be made.  A mouse and a keyboard take checked events like
 * a pen, and reach readers like a USB mouse and keyboard.
 * For a pen, later writes pass checked events to the ordinary input layer.
 * For a touch screen, later writes are frames of fingers, which are split
 * into the reports a USB touch screen sends and run through the USB touch
 * screen's state machine (src/drivers/usb/hid-touch.c).  A touch screen may
 * be declared with a Scan Time, which its frames then carry.  Either way the
 * device reaches readers as /dev/input/eventN like a USB one.  Closing
 * removes it.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <uapi/input-inject.h>
#include <uapi/input.h>

#include <drivers/generic/input-inject.h>
#include <drivers/generic/hid-touch.h>

#include "kern/cdev.h"
#include "kern/clock.h"
#include "kern/cred.h"
#include "kern/file.h"
#include "kern/input-device.h"
#include "kern/kmem.h"
#include "kern/lock.h"

/* The device number of /dev/input-inject. */
#define INJECT_DEVICE_NUMBER	0x000e0000U

/* The tilt resolution: units per radian for units of one degree. */
#define INJECT_TILT_RESOLUTION	57

/* The most absolute axes a device declares (the pen's five). */
#define INJECT_AXIS_COUNT	5U

/* The keyboard's keys: KEY_ESC to KEY_COMPOSE, KEY_F13 to KEY_F24, and the two brightness keys. */
#define INJECT_KEY_FIRST	1U
#define INJECT_KEY_LAST		127U
#define INJECT_KEY_F13		183U
#define INJECT_KEY_F24		194U
#define INJECT_KEY_BRIGHTNESS_DOWN	224U
#define INJECT_KEY_BRIGHTNESS_UP	225U

/* Room for the capabilities a mouse or the keyboard declares (the keyboard's: SYN and 139 keys). */
#define INJECT_CAPABILITY_MAX	160U

/*
 * One open of the node: the device it declared, if any.
 *
 * It lives from open to close.  The lock serializes the writes; kind is 0
 * until the first write declares the device.  A touch screen keeps its
 * state machine and the report and events of one write here, not on the
 * stack.
 */
struct inject_open {
	struct mutex lock;
	struct input_device *device;
	uint32_t kind;
	struct input_abs_axis axes[INJECT_AXIS_COUNT];
	/* The axes declared (the pen's five, an absolute mouse's two, or none). */
	unsigned axis_count;
	/* A mouse or the keyboard: the capabilities it declared, and whether the mouse is absolute. */
	struct input_capability capabilities[INJECT_CAPABILITY_MAX];
	unsigned capability_count;
	int absolute;
	uint32_t report_contacts;
	int32_t touch_x_max;
	int32_t touch_y_max;
	/* The touch screen's frames carry a Scan Time (the setup said so). */
	int touch_scan_time;
	struct hid_touch_description touch_description;
	struct hid_touch_state touch;
	struct hid_report_input report;
	struct hid_touch_output output;
};

static int inject_open(struct file *file);
static int inject_close(struct file *file);
static void inject_release_node(void);
static ssize_t inject_write(struct file *file, const void *buffer, size_t size);
static ssize_t inject_write_pen(struct inject_open *state, const void *buffer, size_t size);
static int inject_declare(struct inject_open *state, const void *buffer, size_t size);
static int inject_setup_valid(const struct input_inject_setup *setup);
static int inject_declare_pen(struct inject_open *state, const struct input_inject_setup *setup);
static int inject_declare_touch(struct inject_open *state, const struct input_inject_setup *setup);
static int inject_declare_mouse(struct inject_open *state, const struct input_inject_setup *setup);
static int inject_declare_keyboard(struct inject_open *state, const struct input_inject_setup *setup);
static void inject_capability(struct inject_open *state, uint16_t type, uint16_t code);
static int inject_mouse_event_valid(const struct inject_open *state, const struct input_event *event);
static int inject_keyboard_key(uint16_t code);
static int inject_axis_value_valid(const struct inject_open *state, const struct input_event *event);
static ssize_t inject_write_touch(struct inject_open *state, const void *buffer, size_t size);
static int inject_frame_valid(const struct inject_open *state, const struct input_inject_touch_frame *frame);
static int inject_contact_valid(const struct inject_open *state, const struct input_inject_contact *contact);
static int inject_touch_report(struct inject_open *state, const struct input_inject_touch_frame *frame, uint32_t first, uint32_t count, uint64_t milliseconds);
static void inject_report_value(struct hid_report_input *report, uint16_t code, int32_t value);
static int inject_event_valid(const struct inject_open *state, const struct input_event *event);
static int inject_key_valid(uint16_t code);
static void inject_axis(struct input_abs_axis *axis, uint16_t code, int32_t minimum, int32_t maximum, int32_t resolution);

/* The file operations of the node. */
static const struct cdev_ops inject_ops = {
	.open = inject_open,
	.close = inject_close,
	.write = inject_write,
};

/* The pen's fixed capabilities: the report boundary, the buttons and the axes. */
static const struct input_capability inject_capabilities[] = {
	{ EV_SYN, SYN_REPORT },
	{ EV_KEY, BTN_TOOL_PEN },
	{ EV_KEY, BTN_TOOL_RUBBER },
	{ EV_KEY, BTN_TOUCH },
	{ EV_KEY, BTN_STYLUS },
	{ EV_KEY, BTN_STYLUS2 },
	{ EV_ABS, ABS_X },
	{ EV_ABS, ABS_Y },
	{ EV_ABS, ABS_PRESSURE },
	{ EV_ABS, ABS_TILT_X },
	{ EV_ABS, ABS_TILT_Y },
};

/*
 * The lock of inject_busy.
 *
 * It is initialized when the node is registered and lives as long as the
 * kernel.
 */
static struct mutex inject_lock;

/*
 * How many opens hold the node: counted up by an open that admits itself
 * (at most INPUT_INJECT_OPENS_MAX), down when it closes (or fails to finish
 * opening).  Zero means the node is free.  inject_lock protects it.
 */
static unsigned inject_busy;

/*
 * Publishes /dev/input-inject.
 */
int
drv_input_inject_register(void)
{
	int error;

	/* Prepares the single-open lock. */
	error = mutex_init(&inject_lock, LOCK_RANK_DEVICE, "input inject");
	if (error != 0)
		return error;

	/* Registers the character device. */
	error = cdev_register("input-inject", (dev_t)INJECT_DEVICE_NUMBER,
			      &inject_ops, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the node exists. */
	return 0;
}

/* Admits one root open with no device declared yet. */
static int
inject_open(
	struct file *file)
{
	struct inject_open *state;
	const struct ucred *credentials;
	int superuser;
	int busy;
	int error;

	/* Refuses anyone but root, whatever the node's mode says. */
	credentials = cred_current();
	superuser = cred_is_superuser(credentials);
	if (!superuser)
		return EPERM;

	/* Admits up to INPUT_INJECT_OPENS_MAX opens, each its own device. */
	mutex_lock(&inject_lock);
	busy = inject_busy >= INPUT_INJECT_OPENS_MAX;
	if (!busy)
		inject_busy++;
	mutex_unlock(&inject_lock);

	/* Refuses an open while all of them are held. */
	if (busy)
		return EBUSY;

	/* Allocates the open's state; without it the node is given back. */
	state = kern_malloc(sizeof(*state));
	if (state == NULL) {
		inject_release_node();
		return ENOMEM;
	}

	/* The state starts with no device and its own write lock. */
	kern_memset(state, 0, sizeof(*state));
	error = mutex_init(&state->lock, LOCK_RANK_DEVICE, "input inject open");
	if (error != 0) {
		kern_free(state);
		inject_release_node();
		return error;
	}

	/* Succeeded: the file holds the state until it closes. */
	file->f_data = state;
	return 0;
}

/* Removes the declared device and gives the node back. */
static int
inject_close(
	struct file *file)
{
	struct inject_open *state;

	/* Removes the device; the input layer releases its held buttons. */
	state = file->f_data;
	if (state != NULL) {
		if (state->device != NULL)
			drv_input_device_unregister(state->device);
		kern_free(state);
	}

	/* The file holds nothing any more. */
	file->f_data = NULL;

	/* Gives the node back. */
	inject_release_node();

	/* Succeeded: the node is free. */
	return 0;
}

/* Gives one open's place back. */
static void
inject_release_node(void)
{
	/* One open fewer: the next open may take its place. */
	mutex_lock(&inject_lock);
	if (inject_busy > 0U)
		inject_busy--;
	mutex_unlock(&inject_lock);
}

/* Declares the device on the first write and takes events or frames after it. */
static ssize_t
inject_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	struct inject_open *state;
	ssize_t written;
	int error;

	/* The writes of one open are taken one at a time. */
	state = file->f_data;
	mutex_lock(&state->lock);

	/* The first write declares the device; the whole record is taken or refused. */
	if (state->device == NULL) {
		error = inject_declare(state, buffer, size);
		mutex_unlock(&state->lock);
		if (error != 0)
			return -error;
		return (ssize_t)size;
	}

	/* A touch screen and a touch pad take frames of fingers; a pen, a mouse and a keyboard events. */
	if (state->kind == INPUT_INJECT_KIND_TOUCH || state->kind == INPUT_INJECT_KIND_TOUCHPAD) {
		written = inject_write_touch(state, buffer, size);
	} else {
		written = inject_write_pen(state, buffer, size);
	}

	/* Lets the open's next write in. */
	mutex_unlock(&state->lock);

	/* Reports a refused write. */
	if (written < 0)
		return written;

	/* Succeeded: the whole write was taken. */
	return written;
}

/*
 * Takes a pen's events: checks every one, then emits them all.  Returns the
 * size taken or a negative error.
 */
static ssize_t
inject_write_pen(
	struct inject_open *state,
	const void *buffer,
	size_t size)
{
	struct input_event event;
	const uint8_t *bytes;
	size_t count;
	size_t index;
	int valid;

	/* Refuses a write that is not a whole, bounded array of events. */
	count = size / sizeof(event);
	if (size == 0)
		return -EINVAL;
	if (size % sizeof(event) != 0)
		return -EINVAL;
	if (count > INPUT_INJECT_EVENTS_MAX)
		return -EINVAL;

	/* Checks every event before any is emitted. */
	bytes = buffer;
	for (index = 0; index < count; index++) {
		kern_memcpy(&event, bytes + index * sizeof(event), sizeof(event));
		valid = inject_event_valid(state, &event);
		if (!valid)
			return -EINVAL;
	}

	/* Emits the events through the input layer. */
	for (index = 0; index < count; index++) {
		kern_memcpy(&event, bytes + index * sizeof(event), sizeof(event));
		drv_input_device_emit(state->device, event.type, event.code,
				      event.value);
	}

	/* Succeeded: the whole write was taken. */
	return (ssize_t)size;
}

/* Checks the setup record and registers the device it declares. */
static int
inject_declare(
	struct inject_open *state,
	const void *buffer,
	size_t size)
{
	struct input_inject_setup setup;
	int valid;
	int error;

	/* Refuses anything but one setup record with sane ranges. */
	if (size != sizeof(setup))
		return EINVAL;
	kern_memcpy(&setup, buffer, sizeof(setup));
	valid = inject_setup_valid(&setup);
	if (!valid)
		return EINVAL;

	/* A mouse and a keyboard (ws173-p001). */
	if (setup.kind == INPUT_INJECT_KIND_MOUSE) {
		error = inject_declare_mouse(state, &setup);
		return error;
	}

	/* The keyboard. */
	if (setup.kind == INPUT_INJECT_KIND_KEYBOARD) {
		error = inject_declare_keyboard(state, &setup);
		return error;
	}

	/* A touch screen, and a touch pad (ws159-p003), are declared apart. */
	if (setup.kind == INPUT_INJECT_KIND_TOUCH || setup.kind == INPUT_INJECT_KIND_TOUCHPAD) {
		error = inject_declare_touch(state, &setup);
		if (error != 0)
			return error;

		/* Succeeded: the touch screen is registered. */
		return 0;
	}

	/* Anything else must be a pen. */
	error = inject_declare_pen(state, &setup);
	if (error != 0)
		return error;

	/* Succeeded: the pen is registered. */
	return 0;
}

/* Tells whether a setup record has the magic, sane ranges and only known bits. */
static int
inject_setup_valid(
	const struct input_inject_setup *setup)
{
	int minimum;

	/* The record names itself. */
	if (setup->magic != INPUT_INJECT_MAGIC)
		return 0;

	/* A mouse or a keyboard may declare no area (0); every other device declares one. */
	minimum = 1;
	if (setup->kind == INPUT_INJECT_KIND_MOUSE || setup->kind == INPUT_INJECT_KIND_KEYBOARD)
		minimum = 0;

	/* The area's width lies in the axis range. */
	if (setup->x_max < minimum)
		return 0;
	if (setup->x_max > INPUT_INJECT_AXIS_MAX)
		return 0;

	/* So does its height. */
	if (setup->y_max < minimum)
		return 0;
	if (setup->y_max > INPUT_INJECT_AXIS_MAX)
		return 0;

	/* The reserved word has no bit but the Scan Time's. */
	if ((setup->reserved & ~INPUT_INJECT_TOUCH_SCAN_TIME) != 0U)
		return 0;

	/* The record is sane. */
	return 1;
}

/* Registers the pen a setup record declares: position, pressure, tilt and buttons. */
static int
inject_declare_pen(
	struct inject_open *state,
	const struct input_inject_setup *setup)
{
	struct input_device_info info;
	int error;

	/* A pen is a pen kind (on a display, or on a tablet beside it), reports no fingers and has no Scan Time. */
	if (setup->kind != INPUT_INJECT_KIND_PEN && setup->kind != INPUT_INJECT_KIND_PEN_POINTER)
		return EINVAL;
	if (setup->report_contacts != 0U)
		return EINVAL;
	if (setup->reserved != 0U)
		return EINVAL;
	state->kind = INPUT_INJECT_KIND_PEN;

	/* Describes the axes: position, 4096 pressure levels and tilt. */
	state->axis_count = INJECT_AXIS_COUNT;
	inject_axis(&state->axes[0], ABS_X, 0, setup->x_max, 0);
	inject_axis(&state->axes[1], ABS_Y, 0, setup->y_max, 0);
	inject_axis(&state->axes[2], ABS_PRESSURE, 0,
		    INPUT_INJECT_PRESSURE_MAX, 0);
	inject_axis(&state->axes[3], ABS_TILT_X, -INPUT_INJECT_TILT_MAX,
		    INPUT_INJECT_TILT_MAX, INJECT_TILT_RESOLUTION);
	inject_axis(&state->axes[4], ABS_TILT_Y, -INPUT_INJECT_TILT_MAX,
		    INPUT_INJECT_TILT_MAX, INJECT_TILT_RESOLUTION);

	/*
	 * Registers the pen as an ordinary input device: one on a display
	 * points at it, one on a tablet does not (WS217); their names tell them
	 * apart.  Both are written to as the pen kind.
	 */
	kern_memset(&info, 0, sizeof(info));
	info.name = "Test pen (input-inject)";
	info.properties = 1U << INPUT_PROP_DIRECT;
	if (setup->kind == INPUT_INJECT_KIND_PEN_POINTER) {
		info.name = "Test pen tablet (input-inject)";
		info.properties = 1U << INPUT_PROP_POINTER;
	}

	/* The rest of what it declares. */
	info.physical_path = "input-inject";
	info.id.bustype = BUS_VIRTUAL;
	info.capabilities = inject_capabilities;
	info.capability_count =
		sizeof(inject_capabilities) / sizeof(inject_capabilities[0]);
	info.absolute_axes = state->axes;
	info.absolute_axis_count = INJECT_AXIS_COUNT;
	error = drv_input_device_register(&info, &state->device);
	if (error != 0)
		return error;

	/* Succeeded: later writes are the pen's events. */
	return 0;
}

/* Tests whether an event is one the declared pen has, within its range. */
static int
inject_event_valid(
	const struct inject_open *state,
	const struct input_event *event)
{
	int button;
	int valid;

	/* A mouse's and the keyboard's events are checked against what they declared. */
	if (state->kind == INPUT_INJECT_KIND_MOUSE || state->kind == INPUT_INJECT_KIND_KEYBOARD) {
		valid = inject_mouse_event_valid(state, event);
		return valid;
	}

	/* Of the synchronization events only the report boundary, with 0. */
	if (event->type == EV_SYN) {
		if (event->code != SYN_REPORT)
			return 0;
		if (event->value != 0)
			return 0;
		return 1;
	}

	/* The pen's buttons, pressed or released. */
	if (event->type == EV_KEY) {
		button = inject_key_valid(event->code);
		if (!button)
			return 0;
		if (event->value != 0 && event->value != 1)
			return 0;
		return 1;
	}

	/* The pen's axes, within their declared ranges. */
	if (event->type == EV_ABS) {
		valid = inject_axis_value_valid(state, event);
		return valid;
	}

	/* Refuses every other event. */
	return 0;
}

/* Tests whether an absolute event names a declared axis, with a value in its range. */
static int
inject_axis_value_valid(
	const struct inject_open *state,
	const struct input_event *event)
{
	const struct input_absinfo *range;
	size_t index;

	/* The axis the event names. */
	for (index = 0; index < state->axis_count; index++) {
		if (state->axes[index].code != event->code)
			continue;

		/* Its value lies in the axis's range. */
		range = &state->axes[index].info;
		if (event->value < range->minimum)
			return 0;
		if (event->value > range->maximum)
			return 0;
		return 1;
	}

	/* An axis the device does not have. */
	return 0;
}

/* Tests whether a key code is one of the pen's buttons. */
static int
inject_key_valid(
	uint16_t code)
{
	/* Admits exactly the declared buttons. */
	switch (code) {
	case BTN_TOOL_PEN:
	case BTN_TOOL_RUBBER:
	case BTN_TOUCH:
	case BTN_STYLUS:
	case BTN_STYLUS2:
		return 1;
	default:
		return 0;
	}
}

/* Fills one absolute axis description. */
static void
inject_axis(
	struct input_abs_axis *axis,
	uint16_t code,
	int32_t minimum,
	int32_t maximum,
	int32_t resolution)
{
	/* The range, with no fuzz or flat zone. */
	kern_memset(axis, 0, sizeof(*axis));
	axis->code = code;
	axis->info.minimum = minimum;
	axis->info.maximum = maximum;
	axis->info.resolution = resolution;

	/* The axis rests at its minimum, or at 0 when the range goes below it (tilt). */
	axis->info.value = minimum;
	if (minimum < 0)
		axis->info.value = 0;
}

/*
 * Registers the touch screen or touch pad a setup record declares: ten slots, fingers in
 * 0..x_max and 0..y_max, reports of report_contacts fingers each, and a
 * Scan Time when the setup asks for one.
 */
static int
inject_declare_touch(
	struct inject_open *state,
	const struct input_inject_setup *setup)
{
	struct hid_report_touch_info touch;
	struct input_device_info info;
	int error;

	/* Refuses reports of no finger, or of more than a frame holds. */
	if (setup->report_contacts < 1U)
		return EINVAL;
	if (setup->report_contacts > INPUT_INJECT_TOUCH_CONTACTS)
		return EINVAL;

	/*
	 * Describes the screen as a USB one that counts its fingers, with the
	 * fingers' position over the declared area.
	 */
	kern_memset(&touch, 0, sizeof(touch));
	touch.contacts = setup->report_contacts;
	touch.count_present = 1;
	touch.x.minimum = 0;
	touch.x.maximum = setup->x_max;
	touch.y.minimum = 0;
	touch.y.maximum = setup->y_max;

	/* A touch pad: one button, the pad itself (ws159-p003), and a size in millimetres (its resolution). */
	if (setup->kind == INPUT_INJECT_KIND_TOUCHPAD) {
		touch.pad = 1;
		touch.buttons = 1U;
		touch.x.resolution = INPUT_INJECT_PAD_RESOLUTION;
		touch.y.resolution = INPUT_INJECT_PAD_RESOLUTION;
	}

	/* A Scan Time, when the setup asks for one, as a Windows touch screen has it. */
	if ((setup->reserved & INPUT_INJECT_TOUCH_SCAN_TIME) != 0U) {
		touch.scan_time_present = 1;
		touch.scan_time_maximum = (int32_t)INPUT_INJECT_SCAN_TIME_MAX;
		touch.scan_time_unit_ns = HID_TOUCH_SCAN_TIME_UNIT_NS;
		state->touch_scan_time = 1;
	}

	/* The device's capabilities and axes. */
	error = drv_hid_touch_describe(&touch, &state->touch_description);
	if (error != 0)
		return error;

	/* No finger touches yet; the state machine counts the Scan Time, if any. */
	drv_hid_touch_reset(&state->touch, state->touch_description.slots);
	drv_hid_touch_set_scan_time(&state->touch, &touch);
	drv_hid_touch_set_pad(&state->touch, &touch);
	state->report_contacts = setup->report_contacts;
	state->touch_x_max = setup->x_max;
	state->touch_y_max = setup->y_max;

	/* Registers the touch screen as an ordinary input device. */
	kern_memset(&info, 0, sizeof(info));
	info.name = "Test touchscreen (input-inject)";
	if (touch.pad)
		info.name = "Test touchpad (input-inject)";
	info.physical_path = "input-inject";
	info.id.bustype = BUS_VIRTUAL;
	info.capabilities = state->touch_description.capabilities;
	info.capability_count = state->touch_description.capability_count;
	info.absolute_axes = state->touch_description.axes;
	info.absolute_axis_count = state->touch_description.axis_count;
	info.properties = state->touch_description.properties;
	error = drv_input_device_register(&info, &state->device);
	if (error != 0)
		return error;

	/* Succeeded: later writes are frames of fingers. */
	state->kind = setup->kind;
	return 0;
}

/*
 * Takes one frame of fingers: checks it whole, then splits it into reports
 * of report_contacts fingers (the first with the frame's Contact Count, the
 * later ones with 0), runs each through the touch state machine and emits
 * what it gives, every event with the time of the write.  Returns the size
 * taken or a negative error.
 */
static ssize_t
inject_write_touch(
	struct inject_open *state,
	const void *buffer,
	size_t size)
{
	struct input_inject_touch_frame frame;
	const struct hid_touch_event *event;
	uint64_t milliseconds;
	uint32_t first;
	uint32_t count;
	size_t index;
	int valid;
	int error;

	/* Refuses anything but one whole, sane frame. */
	if (size != sizeof(frame))
		return -EINVAL;
	kern_memcpy(&frame, buffer, sizeof(frame));
	valid = inject_frame_valid(state, &frame);
	if (!valid)
		return -EINVAL;

	/* Every report and event of the frame has the time it was written. */
	milliseconds = clock_milliseconds(NULL);

	/* Sends the frame's fingers a report at a time; a frame of no finger is one report. */
	first = 0;
	do {
		/* The fingers of this report. */
		count = frame.count - first;
		if (count > state->report_contacts)
			count = state->report_contacts;

		/* Runs the report through the touch state machine. */
		error = inject_touch_report(state, &frame, first, count, milliseconds);
		if (error != 0)
			return -error;

		/* Emits what the report gave, in order. */
		for (index = 0; index < state->output.event_count; index++) {
			event = &state->output.events[index];
			drv_input_device_emit_at(state->device, event->type,
						 event->code, event->value,
						 milliseconds);
		}

		/* The next report starts after these fingers. */
		first += count;
	} while (first < frame.count);

	/* Succeeded: the whole frame was taken. */
	return (ssize_t)size;
}

/* Tells whether a frame is one the touch screen can take. */
static int
inject_frame_valid(
	const struct inject_open *state,
	const struct input_inject_touch_frame *frame)
{
	uint32_t reserved;
	uint32_t index;
	int valid;

	/* At most a frame of fingers. */
	if (frame->count > INPUT_INJECT_TOUCH_CONTACTS)
		return 0;

	/* A touch pad's buttons sit above the Scan Time and are set aside before it is checked. */
	reserved = frame->reserved;
	if (state->kind == INPUT_INJECT_KIND_TOUCHPAD)
		reserved &= ~(INPUT_INJECT_PAD_BUTTONS_MASK << INPUT_INJECT_PAD_BUTTONS_SHIFT);

	/* The rest of the reserved word is the Scan Time on a screen with one, and 0 on any other. */
	if (state->touch_scan_time) {
		if (reserved > INPUT_INJECT_SCAN_TIME_MAX)
			return 0;
	} else {
		if (reserved != 0U)
			return 0;
	}

	/* Every finger of the frame is one the screen can take. */
	for (index = 0; index < frame->count; index++) {
		valid = inject_contact_valid(state, &frame->contacts[index]);
		if (!valid)
			return 0;
	}

	/* The frame is sane. */
	return 1;
}

/* Tells whether a finger names itself, touches or lifts, and lies on the screen. */
static int
inject_contact_valid(
	const struct inject_open *state,
	const struct input_inject_contact *contact)
{
	/* Its identifier is one a USB screen could send. */
	if (contact->contact_id < 0)
		return 0;
	if (contact->contact_id > INPUT_INJECT_CONTACT_ID_MAX)
		return 0;

	/* It touches or lifts. */
	if (contact->tip != 0 && contact->tip != 1)
		return 0;

	/* It lies on the screen. */
	if (contact->x < 0 || contact->x > state->touch_x_max)
		return 0;
	if (contact->y < 0 || contact->y > state->touch_y_max)
		return 0;

	/* The finger is sane. */
	return 1;
}

/*
 * Builds the report of count fingers from first on, as a USB touch screen
 * that counts its fingers sends it, and runs it through the touch state
 * machine into state->output.
 */
static int
inject_touch_report(
	struct inject_open *state,
	const struct input_inject_touch_frame *frame,
	uint32_t first,
	uint32_t count,
	uint64_t milliseconds)
{
	const struct input_inject_contact *contact;
	struct hid_report_input *report;
	uint32_t finger;
	uint32_t scan_time;
	uint32_t buttons;
	int32_t contact_count;
	int error;

	/* The first report of the frame counts its fingers; the later ones count 0. */
	report = &state->report;
	kern_memset(report, 0, sizeof(*report));
	contact_count = 0;
	if (first == 0U)
		contact_count = (int32_t)frame->count;
	inject_report_value(report, HID_TOUCH_CONTACT_COUNT_CODE, contact_count);

	/* A touch pad's frame keeps its buttons above its Scan Time. */
	scan_time = frame->reserved;
	buttons = 0U;
	if (state->kind == INPUT_INJECT_KIND_TOUCHPAD) {
		buttons = (frame->reserved >> INPUT_INJECT_PAD_BUTTONS_SHIFT) & INPUT_INJECT_PAD_BUTTONS_MASK;
		scan_time = frame->reserved & INPUT_INJECT_SCAN_TIME_MAX;
	}

	/* Every report of the frame carries the frame's Scan Time, on a screen with one. */
	if (state->touch_scan_time)
		inject_report_value(report, HID_TOUCH_SCAN_TIME_CODE, (int32_t)scan_time);

	/* A touch pad's first report of the frame carries its left button. */
	if (state->kind == INPUT_INJECT_KIND_TOUCHPAD && first == 0U)
		inject_report_value(report, HID_TOUCH_BUTTON_CODE(0), (int32_t)(buttons & 1U));

	/* Each finger in its place in the report, confident. */
	for (finger = 0; finger < count; finger++) {
		contact = &frame->contacts[first + finger];
		inject_report_value(report, HID_TOUCH_CODE(finger, HID_TOUCH_ITEM_TIP), contact->tip);
		inject_report_value(report, HID_TOUCH_CODE(finger, HID_TOUCH_ITEM_CONFIDENCE), 1);
		inject_report_value(report, HID_TOUCH_CODE(finger, HID_TOUCH_ITEM_CONTACT_ID), contact->contact_id);
		inject_report_value(report, HID_TOUCH_CODE(finger, HID_TOUCH_ITEM_X), contact->x);
		inject_report_value(report, HID_TOUCH_CODE(finger, HID_TOUCH_ITEM_Y), contact->y);
	}

	/* Turns the report into protocol B events, at the time the frame was written. */
	error = drv_hid_touch_translate_at(&state->touch, report, milliseconds, &state->output);
	if (error != 0)
		return error;

	/* Succeeded: the events are in state->output. */
	return 0;
}

/* Appends one touch value to a report (a report holds far more than a frame needs). */
static void
inject_report_value(
	struct hid_report_input *report,
	uint16_t code,
	int32_t value)
{
	struct hid_report_value *entry;

	/* Stores the value after the ones already in the report. */
	entry = &report->values[report->value_count];
	entry->type = HID_REPORT_TYPE_TOUCH;
	entry->code = code;
	entry->value = value;
	report->value_count++;
}

/*
 * Registers the mouse a setup record declares: three buttons and both
 * wheels, with relative motion (x_max and y_max 0) or an absolute position
 * (both above 0, like QEMU's USB tablet).
 */
static int
inject_declare_mouse(
	struct inject_open *state,
	const struct input_inject_setup *setup)
{
	struct input_device_info info;
	int error;

	/* No fingers and no Scan Time; an area in both directions or in neither. */
	if (setup->report_contacts != 0U || setup->reserved != 0U)
		return EINVAL;
	if ((setup->x_max == 0) != (setup->y_max == 0))
		return EINVAL;
	state->absolute = setup->x_max > 0;

	/* The report boundary, the buttons and the wheels. */
	state->capability_count = 0U;
	inject_capability(state, EV_SYN, SYN_REPORT);
	inject_capability(state, EV_KEY, BTN_LEFT);
	inject_capability(state, EV_KEY, BTN_RIGHT);
	inject_capability(state, EV_KEY, BTN_MIDDLE);
	inject_capability(state, EV_REL, REL_HWHEEL);
	inject_capability(state, EV_REL, REL_WHEEL);

	/* The motion: relative, or two absolute axes over the area. */
	state->axis_count = 0U;
	if (state->absolute) {
		inject_capability(state, EV_ABS, ABS_X);
		inject_capability(state, EV_ABS, ABS_Y);
		inject_axis(&state->axes[0], ABS_X, 0, setup->x_max, 0);
		inject_axis(&state->axes[1], ABS_Y, 0, setup->y_max, 0);
		state->axis_count = 2U;
	} else {
		inject_capability(state, EV_REL, REL_X);
		inject_capability(state, EV_REL, REL_Y);
	}

	/* Registers it as an ordinary input device. */
	kern_memset(&info, 0, sizeof(info));
	info.name = "Test mouse (input-inject)";
	if (state->absolute)
		info.name = "Test absolute pointer (input-inject)";
	info.physical_path = "input-inject";
	info.id.bustype = BUS_VIRTUAL;
	info.capabilities = state->capabilities;
	info.capability_count = state->capability_count;
	info.absolute_axes = state->axes;
	info.absolute_axis_count = state->axis_count;
	error = drv_input_device_register(&info, &state->device);
	if (error != 0)
		return error;

	/* Succeeded: later writes are the mouse's events. */
	state->kind = INPUT_INJECT_KIND_MOUSE;
	return 0;
}

/* Registers the keyboard a setup record declares: KEY_ESC to KEY_COMPOSE, KEY_F13 to KEY_F24, brightness. */
static int
inject_declare_keyboard(
	struct inject_open *state,
	const struct input_inject_setup *setup)
{
	struct input_device_info info;
	uint16_t code;
	int error;

	/* No area, no fingers, no Scan Time. */
	if (setup->x_max != 0 || setup->y_max != 0 || setup->report_contacts != 0U || setup->reserved != 0U)
		return EINVAL;

	/* The report boundary and every key. */
	state->capability_count = 0U;
	inject_capability(state, EV_SYN, SYN_REPORT);
	for (code = INJECT_KEY_FIRST; code <= INJECT_KEY_LAST; code++)
		inject_capability(state, EV_KEY, code);
	for (code = INJECT_KEY_F13; code <= INJECT_KEY_F24; code++)
		inject_capability(state, EV_KEY, code);
	inject_capability(state, EV_KEY, INJECT_KEY_BRIGHTNESS_DOWN);
	inject_capability(state, EV_KEY, INJECT_KEY_BRIGHTNESS_UP);

	/* Registers it as an ordinary input device (the compositor repeats a held key). */
	kern_memset(&info, 0, sizeof(info));
	info.name = "Test keyboard (input-inject)";
	info.physical_path = "input-inject";
	info.id.bustype = BUS_VIRTUAL;
	info.capabilities = state->capabilities;
	info.capability_count = state->capability_count;
	error = drv_input_device_register(&info, &state->device);
	if (error != 0)
		return error;

	/* Succeeded: later writes are the keyboard's events. */
	state->kind = INPUT_INJECT_KIND_KEYBOARD;
	return 0;
}

/* Adds one capability to a mouse's or the keyboard's list (the list has room for every one declared). */
static void
inject_capability(
	struct inject_open *state,
	uint16_t type,
	uint16_t code)
{
	/* After the ones before it. */
	if (state->capability_count >= INJECT_CAPABILITY_MAX)
		return;
	state->capabilities[state->capability_count].type = type;
	state->capabilities[state->capability_count].code = code;
	state->capability_count++;
}

/* Tests whether an event is one the mouse or the keyboard declared, with a value it takes. */
static int
inject_mouse_event_valid(
	const struct inject_open *state,
	const struct input_event *event)
{
	int valid;

	/* The report boundary, with 0. */
	if (event->type == EV_SYN) {
		if (event->code != SYN_REPORT || event->value != 0)
			return 0;
		return 1;
	}

	/* A button or a key, pressed or released (never repeated). */
	if (event->type == EV_KEY) {
		if (event->value != 0 && event->value != 1)
			return 0;
		if (state->kind == INPUT_INJECT_KIND_KEYBOARD) {
			valid = inject_keyboard_key(event->code);
			return valid;
		}

		/* The mouse's three buttons. */
		if (event->code == BTN_LEFT || event->code == BTN_RIGHT || event->code == BTN_MIDDLE)
			return 1;
		return 0;
	}

	/* The keyboard has nothing else. */
	if (state->kind == INPUT_INJECT_KIND_KEYBOARD)
		return 0;

	/* A movement or a wheel's turn, within its bounds (an absolute mouse has no relative motion). */
	if (event->type == EV_REL) {
		if (event->value < -INPUT_INJECT_REL_MAX || event->value > INPUT_INJECT_REL_MAX)
			return 0;
		if (event->code == REL_WHEEL || event->code == REL_HWHEEL)
			return 1;
		if (!state->absolute && (event->code == REL_X || event->code == REL_Y))
			return 1;
		return 0;
	}

	/* An absolute mouse's position, within its area. */
	if (event->type == EV_ABS && state->absolute) {
		valid = inject_axis_value_valid(state, event);
		return valid;
	}

	/* Refuses every other event. */
	return 0;
}

/* Tests whether a key code is one of the keyboard's. */
static int
inject_keyboard_key(
	uint16_t code)
{
	/* KEY_ESC to KEY_COMPOSE. */
	if (code >= INJECT_KEY_FIRST && code <= INJECT_KEY_LAST)
		return 1;

	/* KEY_F13 to KEY_F24. */
	if (code >= INJECT_KEY_F13 && code <= INJECT_KEY_F24)
		return 1;

	/* The brightness keys. */
	if (code == INJECT_KEY_BRIGHTNESS_DOWN || code == INJECT_KEY_BRIGHTNESS_UP)
		return 1;

	/* Not one of them. */
	return 0;
}
