/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Evdev pointers and keyboards read by the seat.
 *
 * Every /dev/input/eventN node that reports REL_X and REL_Y or ABS_X and
 * ABS_Y is a pointer; one that reports the letter keys is a keyboard; a node
 * may be both.  A node that reports BTN_TOOL_PEN and ABS_PRESSURE is a pen
 * tablet instead, whose reports tablet.c applies, and one that speaks
 * multitouch protocol B (ABS_MT_SLOT, ABS_MT_TRACKING_ID and both
 * ABS_MT_POSITION axes) is a touch screen, whose reports touch.c applies,
 * unless it says it is a pointer (INPUT_PROP_POINTER): then it is a touch
 * pad, whose reports touchpad.c turns into the pointer's motion, buttons
 * and scrolling (ws159-p004).
 * Nodes are read without blocking.  Events are gathered until
 * SYN_REPORT and applied as one group: motion first, then buttons and keys in
 * the order they arrived, then wheel scrolling, then a pointer frame.
 */

#include "kwl.h"
#include "tablet.h"
#include "touch.h"

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Bound the reads one device gets per event-loop pass, so others still run. */
#define INPUT_READS_PER_PASS	16U

/* The events one read of a device takes at most. */
#define INPUT_READ_EVENTS	32U

/*
 * One ready device's events while the devices are read together: those read
 * and not yet applied, and how many reads it has had this pass.
 */
struct input_source {
	struct kwl_input_device *device;
	struct input_event events[INPUT_READ_EVENTS];
	size_t count;
	size_t next;
	unsigned reads;
	unsigned done;
};

/* The mouse buttons, BTN_LEFT through BTN_TASK, delivered as wl_pointer buttons. */
#define INPUT_BUTTON_FIRST	0x110U
#define INPUT_BUTTON_LAST	0x117U

/* Key codes from BTN_MISC up to the end of the button block are not keyboard keys. */
#define INPUT_BUTTON_BLOCK_FIRST	0x100U
#define INPUT_BUTTON_BLOCK_END		0x160U

/* The uapi header does not name the meta keys; these are their evdev codes. */
#define INPUT_KEY_LEFTMETA	125U
#define INPUT_KEY_RIGHTMETA	126U

/* The xkb default modifier mask bits reported in wl_keyboard.modifiers. */
#define MODIFIER_SHIFT		0x01U
#define MODIFIER_CONTROL	0x04U
#define MODIFIER_ALT		0x08U
#define MODIFIER_META		0x40U

/* The locked modifier bits, and the evdev codes of the keys that toggle them (ws035-p078). */
#define MODIFIER_CAPS_LOCK	0x02U
#define MODIFIER_NUM_LOCK	0x10U
#define INPUT_KEY_CAPSLOCK	58U
#define INPUT_KEY_NUMLOCK	69U

/* Bits of server->modifier_keys, one per held modifier key. */
#define HELD_LEFTSHIFT		0x01U
#define HELD_RIGHTSHIFT		0x02U
#define HELD_LEFTCTRL		0x04U
#define HELD_RIGHTCTRL		0x08U
#define HELD_LEFTALT		0x10U
#define HELD_RIGHTALT		0x20U
#define HELD_LEFTMETA		0x40U
#define HELD_RIGHTMETA		0x80U

/* The number of bits in one word of an evdev capability bitmap. */
#define BITMAP_WORD_BITS	(8U * sizeof(unsigned long))

static int read_ranges(int descriptor, struct input_absinfo *x, struct input_absinfo *y);
static int bit_is_set(const unsigned long *bits, unsigned code);
static void consume_event(struct kwl_server *server, struct kwl_input_device *device, const struct input_event *event);
static int source_fill(struct kwl_server *server, struct input_source *source);
static int event_earlier(const struct input_event *event, const struct input_event *other);
static void apply_frame(struct kwl_server *server, struct kwl_input_device *device, uint32_t time);
static void apply_key(struct kwl_server *server, uint32_t time, uint32_t key, int32_t value);
static int32_t scale_absolute(int32_t value, int32_t minimum, int32_t maximum, uint32_t size);
static int32_t clamp_position(int64_t position, uint32_t size);
static uint32_t event_time(const struct input_event *event);
static void update_capabilities(struct kwl_server *server);
static int attach_tablet(struct kwl_server *server, int descriptor, const char *path);
static int attach_touch(struct kwl_server *server, int descriptor, const char *path);
static int multitouch(const struct kl_backend_input_caps *capabilities);
static int touchpad_node(const struct kl_backend_input_caps *capabilities);
static int attach_touchpad(struct kwl_server *server, int descriptor, const char *path);
static void apply_touchpad(struct kwl_server *server, struct kwl_input_device *device, uint32_t time);
static void apply_touchpad_actions(struct kwl_server *server, const struct kwl_touchpad *pad, const struct kwl_touchpad_actions *actions, uint32_t time);
static void lock_pad_gesture(struct kwl_server *server, const struct kwl_touchpad_action *action);
static int pointer_move(struct kwl_server *server, int64_t delta_x, int64_t delta_y, uint32_t time);
static ssize_t input_read(struct kwl_server *server, struct kwl_input_device *device, struct input_event *events, size_t capacity);

/*
 * Opens the input devices not read yet (libkeiland-backend's scan, which
 * offers each through kwl_input_probe) and sets the time of the next scan.
 */
void
kwl_input_scan(
	struct kwl_server *server)
{
	/* The next rescan is due one period from now. */
	server->input_scan_time = kwl_milliseconds();

	/* The backend finds, opens and offers the devices. */
	kl_backend_input_scan(server->backend);
}

/*
 * Classifies an open device and attaches it to the seat.
 *
 * Returns 1 when the seat keeps the descriptor, 0 when it is unsupported or
 * the table is full (the backend then closes it).
 */
int
kwl_input_probe(
	struct kwl_server *server,
	int descriptor,
	const char *path,
	const struct kl_backend_input_caps *capabilities)
{
	struct input_absinfo x;
	struct input_absinfo y;
	unsigned pointer;
	unsigned keyboard;
	unsigned absolute;
	int touch_screen;
	int pad;
	int has_type;
	int has_first;
	int has_second;
	int error;

	/* A pen tablet reports BTN_TOOL_PEN, ABS_PRESSURE and both position axes (tablet.c, WS079 p003). */
	has_type = bit_is_set(capabilities->event, EV_KEY);
	has_first = bit_is_set(capabilities->key, BTN_TOOL_PEN);
	has_second = bit_is_set(capabilities->absolute, ABS_PRESSURE);
	if (has_type &&
	    has_first &&
	    has_second) {
		/* Both position ranges must be readable and nonempty to be mapped. */
		error = read_ranges(descriptor, &x, &y);
		if (error == 0) {
			error = attach_tablet(server, descriptor, path);
			return error == 0;
		}
	}

	/* A touch pad speaks multitouch protocol B and is a pointer (touchpad.c, ws159-p004). */
	pad = touchpad_node(capabilities);
	if (pad) {
		error = attach_touchpad(server, descriptor, path);
		return error == 0;
	}

	/* A touch screen speaks multitouch protocol B (touch.c, WS079 p013); its ABS_X/Y are not a pointer. */
	touch_screen = multitouch(capabilities);
	if (touch_screen) {
		error = attach_touch(server, descriptor, path);
		return error == 0;
	}

	/* A keyboard reports key events including the letter keys A and Z. */
	keyboard = 0;
	has_type = bit_is_set(capabilities->event, EV_KEY);
	has_first = bit_is_set(capabilities->key, KEY_A);
	has_second = bit_is_set(capabilities->key, KEY_Z);
	if (has_type &&
	    has_first &&
	    has_second)
		keyboard = 1;

	/* An absolute pointer reports ABS_X and ABS_Y. */
	absolute = 0;
	has_type = bit_is_set(capabilities->event, EV_ABS);
	has_first = bit_is_set(capabilities->absolute, ABS_X);
	has_second = bit_is_set(capabilities->absolute, ABS_Y);
	if (has_type &&
	    has_first &&
	    has_second) {
		/* Both axis ranges must be readable and nonempty to be mapped. */
		error = read_ranges(descriptor, &x, &y);
		if (error == 0)
			absolute = 1;
	}

	/* A relative pointer reports REL_X and REL_Y. */
	pointer = absolute;
	has_type = bit_is_set(capabilities->event, EV_REL);
	has_first = bit_is_set(capabilities->relative, REL_X);
	has_second = bit_is_set(capabilities->relative, REL_Y);
	if (has_type &&
	    has_first &&
	    has_second)
		pointer = 1;

	/* A node that is neither is not the seat's business (the backend closes it). */
	if (!pointer && !keyboard)
		return 0;

	/* Keep the node; an absolute pointer brings its ranges along. */
	if (absolute) {
		error = kwl_input_attach(server, descriptor, path, pointer, keyboard, &x, &y);
	} else {
		error = kwl_input_attach(server, descriptor, path, pointer, keyboard, NULL, NULL);
	}

	/* Succeeded: the node has been classified; 1 when the seat keeps it. */
	return error == 0;
}

/*
 * Adopts an open evdev descriptor as a pointer, a keyboard or both.
 *
 * The device takes ownership of the descriptor when it returns 0; otherwise
 * (no slot free) the caller still owns it.  An absolute pointer supplies both axis ranges; a relative one passes
 * NULL for both.
 */
int
kwl_input_attach(
	struct kwl_server *server,
	int descriptor,
	const char *path,
	unsigned pointer,
	unsigned keyboard,
	const struct input_absinfo *x,
	const struct input_absinfo *y)
{
	struct kwl_input_device *device;
	unsigned index;

	/* Find a free slot in the fixed device table. */
	device = NULL;
	for (index = 0; index < KWL_INPUT_MAX; index++) {
		/* A slot not in use can hold the new device. */
		if (!server->inputs[index].live) {
			device = &server->inputs[index];
			break;
		}
	}

	/* A full table cannot take another device (the caller's backend closes it). */
	if (device == NULL)
		return ENOSPC;

	/* The slot now describes this node. */
	memset(device, 0, sizeof(*device));
	device->fd = descriptor;
	device->pointer = pointer;
	device->keyboard = keyboard;
	kwl_pointer_accel_init(&device->accel);
	snprintf(device->path, sizeof(device->path), "%s", path);

	/* An absolute pointer maps its axis ranges onto the surface. */
	if (pointer && x != NULL && y != NULL) {
		device->absolute = 1;
		device->abs_x_minimum = x->minimum;
		device->abs_x_maximum = x->maximum;
		device->abs_y_minimum = y->minimum;
		device->abs_y_maximum = y->maximum;
		device->abs_x = x->value;
		device->abs_y = y->value;
	}

	/* The slot is published only when it is completely filled in. */
	device->live = 1;

	/* One line per role lets a test see which devices the seat uses. */
	if (pointer)
		printf("KWL INPUT device=%s kind=pointer abs=%u\n", device->path, device->absolute);
	if (keyboard)
		printf("KWL INPUT device=%s kind=keyboard abs=0\n", device->path);

	/* Bound seats learn about a new class of device. */
	update_capabilities(server);

	/* Succeeded: the event loop now reads this device. */
	return 0;
}

/*
 * Drains the events the ready devices have and applies them in the order
 * they were made (BUG-142).
 *
 * Each device's reports are gathered in the device until its SYN_REPORT, so
 * the order between devices is the order of their events' times: a key
 * pressed before a click is applied before it even when both waited while
 * the compositor was busy.  Events of one time keep the devices' order, and
 * one device's events keep their own.  A read error other than "nothing
 * ready" means the device went away, and it is closed.
 */
void
kwl_input_read_devices(
	struct kwl_server *server,
	struct kwl_input_device **devices,
	size_t count)
{
	static struct input_source sources[KWL_INPUT_MAX];
	struct input_source *earliest;
	size_t used;
	size_t index;
	int ready;
	int before;

	/* One source for each device given, up to the seat's devices. */
	used = count;
	if (used > KWL_INPUT_MAX)
		used = KWL_INPUT_MAX;
	for (index = 0; index < used; index++) {
		sources[index].device = devices[index];
		sources[index].count = 0;
		sources[index].next = 0;
		sources[index].reads = 0;
		sources[index].done = 0;
	}

	/* Applies the earliest waiting event until no device has one this pass. */
	for (;;) {
		earliest = NULL;
		for (index = 0; index < used; index++) {
			/* A source that ran out reads again, within its reads for the pass. */
			ready = source_fill(server, &sources[index]);
			if (!ready)
				continue;

			/* The first source with the earliest event wins a tie. */
			if (earliest == NULL) {
				earliest = &sources[index];
				continue;
			}

			/* A later source wins only with an earlier event. */
			before = event_earlier(&sources[index].events[sources[index].next],
					       &earliest->events[earliest->next]);
			if (before)
				earliest = &sources[index];
		}

		/* Nothing is left to apply. */
		if (earliest == NULL)
			return;

		/* Applies the event and moves its source on. */
		consume_event(server, earliest->device, &earliest->events[earliest->next]);
		earliest->next++;
	}
}

/*
 * Makes sure a source has an event to apply, reading its device when the
 * events read before are used up.  Reports 1 when it has one, 0 when the
 * device has nothing more this pass (nothing ready, its reads used, or gone).
 */
static int
source_fill(
	struct kwl_server *server,
	struct input_source *source)
{
	ssize_t count;
	int error;

	/* Events read before are still waiting. */
	if (source->next < source->count)
		return 1;

	/* Reads until the device gives events, has none, or the pass's reads are used. */
	while (!source->done) {
		/* A closed slot, or one that had its reads, has nothing more this pass. */
		if (!source->device->live || source->reads >= INPUT_READS_PER_PASS) {
			source->done = 1;
			break;
		}

		/* Reads as many whole events as the buffer holds. */
		source->reads++;
		count = input_read(server, source->device, source->events, INPUT_READ_EVENTS);
		if (count > 0) {
			source->count = (size_t)count;
			source->next = 0;
			return 1;
		}

		/* End of file means the node is gone. */
		if (count == 0) {
			printf("KWL INPUT_CLOSED device=%s errno=%d\n", source->device->path, EIO);
			kwl_input_close(server, source->device);
			source->done = 1;
			break;
		}

		/* Nothing more is ready; the next poll will say when there is. */
		error = errno;
		if (error == EAGAIN || error == EWOULDBLOCK) {
			source->done = 1;
			break;
		}

		/* Any other failure but an interruption means the device is gone. */
		if (error != EINTR) {
			printf("KWL INPUT_CLOSED device=%s errno=%d\n", source->device->path, error);
			kwl_input_close(server, source->device);
			source->done = 1;
		}
	}

	/* Nothing to apply from this device this pass. */
	return 0;
}

/* Reports whether an event was made before another (by its evdev time). */
static int
event_earlier(
	const struct input_event *event,
	const struct input_event *other)
{
	/* The seconds decide first. */
	if (event->time.tv_sec < other->time.tv_sec)
		return 1;
	if (event->time.tv_sec > other->time.tv_sec)
		return 0;

	/* Then the microseconds. */
	if (event->time.tv_usec < other->time.tv_usec)
		return 1;

	/* Made at the same time or later. */
	return 0;
}

/*
 * Closes one device and tells bound seats when a device class disappears.
 */
void
kwl_input_close(
	struct kwl_server *server,
	struct kwl_input_device *device)
{
	struct kwl_touchpad_actions actions;

	/* A slot that is not in use has no descriptor. */
	if (!device->live)
		return;

	/* A pen tablet's clients hear that it and its tools are gone (tablet.c). */
	if (device->tablet)
		kwl_tablet_remove(server, device, 1);

	/* A touch screen's fingers end (touch.c). */
	if (device->touch)
		kwl_touch_remove(server, device, 1);

	/* A touch pad lets go of every button it holds. */
	if (device->touchpad) {
		kwl_touchpad_release_all(&device->pad, &actions);
		apply_touchpad_actions(server, &device->pad, &actions, (uint32_t)kwl_milliseconds());
	}

	/* The slot is free once its descriptor is closed. */
	kl_backend_input_close(server->backend, device->fd);
	device->fd = -1;
	device->live = 0;

	/* Bound seats learn that a class of device may be gone. */
	update_capabilities(server);

	/* Succeeded: the device is no longer read. */
	return;
}

/*
 * Forgets one device whose descriptor the seat has already closed
 * (libkeiland-backend's input_gone, ws131-p006): the same as
 * kwl_input_close without returning the descriptor.
 */
void
kwl_input_forget(
	struct kwl_server *server,
	struct kwl_input_device *device)
{
	struct kwl_touchpad_actions actions;

	/* A slot that is not in use has nothing to forget. */
	if (!device->live)
		return;

	/* A pen tablet's clients and a touch screen's fingers hear that it is gone. */
	if (device->tablet)
		kwl_tablet_remove(server, device, 1);
	if (device->touch)
		kwl_touch_remove(server, device, 1);

	/* A touch pad lets go of every button it holds. */
	if (device->touchpad) {
		kwl_touchpad_release_all(&device->pad, &actions);
		apply_touchpad_actions(server, &device->pad, &actions, (uint32_t)kwl_milliseconds());
	}

	/* The slot is free; its descriptor was the seat's. */
	device->fd = -1;
	device->live = 0;

	/* Bound seats learn that a class of device may be gone. */
	update_capabilities(server);
}

/*
 * Closes every device during service shutdown without notifying clients.
 */
void
kwl_input_cleanup(
	struct kwl_server *server)
{
	unsigned index;

	/* Every slot in use owns one descriptor. */
	for (index = 0; index < KWL_INPUT_MAX; index++) {
		/* A free slot owns nothing. */
		if (!server->inputs[index].live)
			continue;

		/* A pen tablet's state is forgotten without telling anyone (tablet.c). */
		if (server->inputs[index].tablet)
			kwl_tablet_remove(server, &server->inputs[index], 0);

		/* A touch screen's fingers are forgotten the same way (touch.c). */
		if (server->inputs[index].touch)
			kwl_touch_remove(server, &server->inputs[index], 0);

		/* Close the descriptor and free the slot. */
		kl_backend_input_close(server->backend, server->inputs[index].fd);
		server->inputs[index].fd = -1;
		server->inputs[index].live = 0;
	}

	/* No client remains to hear about it, so the capabilities are simply cleared. */
	server->capabilities = 0;

	/* Succeeded: the seat holds no device descriptors. */
	return;
}

/* Reads both absolute axis ranges and refuses a range that cannot be mapped. */
static int
read_ranges(
	int descriptor,
	struct input_absinfo *x,
	struct input_absinfo *y)
{
	int error;

	/* The horizontal range maps onto the surface width. */
	memset(x, 0, sizeof(*x));
	error = kl_backend_input_absinfo(descriptor, ABS_X, x);
	if (error != 0)
		return error;

	/* The vertical range maps onto the surface height. */
	memset(y, 0, sizeof(*y));
	error = kl_backend_input_absinfo(descriptor, ABS_Y, y);
	if (error != 0)
		return error;

	/* An empty or inverted range has no pixel to map to. */
	if (x->maximum <= x->minimum || y->maximum <= y->minimum)
		return EINVAL;

	/* Succeeded: both axes can be scaled onto the surface. */
	return 0;
}

/* Reports whether one code is set in an evdev bitmap of unsigned long words. */
static int
bit_is_set(
	const unsigned long *bits,
	unsigned code)
{
	unsigned long word;

	/* The word holding the code, and the code's bit within it. */
	word = bits[code / BITMAP_WORD_BITS];
	if ((word & (1UL << (code % BITMAP_WORD_BITS))) != 0)
		return 1;

	/* The code is not reported. */
	return 0;
}

/* Adds one event to the device's pending report, or completes the report. */
static void
consume_event(
	struct kwl_server *server,
	struct kwl_input_device *device,
	const struct input_event *event)
{
	uint32_t time;

	/* The kernel dropped events: forget the partial report and skip to the next one. */
	if (event->type == EV_SYN && event->code == SYN_DROPPED) {
		device->frame_count = 0;
		device->discarding = 1;
		return;
	}

	/* SYN_REPORT completes a report, unless the report was damaged by a drop. */
	if (event->type == EV_SYN && event->code == SYN_REPORT) {
		/* A damaged report is thrown away; the next one is applied again. */
		if (device->discarding) {
			device->discarding = 0;
			device->frame_count = 0;
			return;
		}

		/* An intact report is applied as one group: a pen tablet's by the tablet (tablet.c), a touch screen's by touch.c. */
		time = event_time(event);
		device->frame_time_us = (uint64_t)event->time.tv_sec * 1000000U + (uint64_t)event->time.tv_usec;
		if (device->tablet) {
			kwl_tablet_frame(server, device, time);
		} else if (device->touch) {
			kwl_touch_frame(server, device, time);
		} else if (device->touchpad) {
			apply_touchpad(server, device, time);
		} else {
			apply_frame(server, device, time);
		}

		/* The next report starts empty. */
		device->frame_count = 0;
		return;
	}

	/* Other synchronization events and a damaged report carry nothing to keep. */
	if (event->type == EV_SYN || device->discarding)
		return;

	/* The exit summary counts every evdev event that was not a synchronization. */
	server->input_events++;

	/* A report too large to hold is thrown away like a dropped one. */
	if (device->frame_count == KWL_INPUT_FRAME_MAX) {
		device->frame_count = 0;
		device->discarding = 1;
		return;
	}

	/* Keep the event until its report completes. */
	device->frame[device->frame_count] = *event;
	device->frame_count++;

	/* Succeeded: the event waits for SYN_REPORT. */
	return;
}

/* Applies one completed report: motion, buttons and keys, scrolling, frame. */
static void
apply_frame(
	struct kwl_server *server,
	struct kwl_input_device *device,
	uint32_t time)
{
	const struct input_event *event;
	int64_t delta_x;
	int64_t delta_y;
	int64_t moved_x;
	int64_t moved_y;
	int32_t wheel;
	int32_t horizontal_wheel;
	int32_t x;
	int32_t y;
	int32_t old_x;
	int32_t old_y;
	unsigned absolute_seen;
	unsigned pointer_activity;
	unsigned index;

	/* Collect relative movement, wheel steps and the latest absolute position. */
	delta_x = 0;
	delta_y = 0;
	wheel = 0;
	horizontal_wheel = 0;
	absolute_seen = 0;
	for (index = 0; index < device->frame_count; index++) {
		event = &device->frame[index];

		/* Only a pointer's axes move the pointer. */
		if (!device->pointer)
			break;

		/* Relative axes add up over the report. */
		if (event->type == EV_REL) {
			/* Each relative code feeds its own sum. */
			if (event->code == REL_X) {
				delta_x += event->value;
			} else if (event->code == REL_Y) {
				delta_y += event->value;
			} else if (event->code == REL_WHEEL) {
				wheel += event->value;
			} else if (event->code == REL_HWHEEL) {
				horizontal_wheel += event->value;
			}
		}

		/* Absolute axes replace the device's remembered position. */
		if (event->type == EV_ABS && device->absolute) {
			/* Each axis is remembered separately, as a report may carry only one. */
			if (event->code == ABS_X) {
				device->abs_x = event->value;
				absolute_seen = 1;
			} else if (event->code == ABS_Y) {
				device->abs_y = event->value;
				absolute_seen = 1;
			}
		}
	}

	/*
	 * The device placed or moved the pointer, even onto the place it
	 * already had: its arrow is shown from now on, drawn where it is
	 * (ws035-p116).
	 */
	if (server->pointer_unmoved &&
	    (absolute_seen ||
	     delta_x != 0 ||
	     delta_y != 0)) {
		server->pointer_unmoved = 0U;
		kwl_damage_pointer(server, server->pointer_x, server->pointer_y);
		printf("KWL CURSOR shown by=pointer device=%s\n", device->path);
	}

	/* An absolute report places the pointer; relative movement is added and clamped. */
	x = server->pointer_x;
	y = server->pointer_y;
	if (absolute_seen) {
		x = scale_absolute(device->abs_x, device->abs_x_minimum, device->abs_x_maximum, server->width);
		y = scale_absolute(device->abs_y, device->abs_y_minimum, device->abs_y_maximum, server->height);
	}

	/*
	 * The relative movement at the mouse's speed and acceleration
	 * (ws089-p024), the fractions of a pixel carried to the next report so
	 * that a slow pointer still moves.
	 */
	if (delta_x != 0 || delta_y != 0) {
		kwl_pointer_accel_move(&device->accel, delta_x, delta_y, device->frame_time_us, server->mouse_speed, server->mouse_acceleration, &moved_x, &moved_y);
		delta_x = moved_x;
		delta_y = moved_y;
	}

	/*
	 * An absolute device maps onto the anchor; relative movement goes over
	 * the outputs shown, across their shared edges (ws113-p007).
	 */
	if (absolute_seen) {
		kwl_pointer_absolute(server);
		x = clamp_position((int64_t)x + delta_x, server->width);
		y = clamp_position((int64_t)y + delta_y, server->height);
	} else {
		kwl_pointer_relative(server, delta_x, delta_y, &x, &y);
	}

	/* A changed position is reported as motion before any button of the same report. */
	pointer_activity = 0;
	if (x != server->pointer_x || y != server->pointer_y) {
		old_x = server->pointer_x;
		old_y = server->pointer_y;
		server->pointer_x = x;
		server->pointer_y = y;
		kwl_seat_motion(server, time);

		/* Window mode draws the cursor at its new place (and where it was, damage.c). */
		kwl_damage_pointer(server, old_x, old_y);
		pointer_activity = 1;
	}

	/* Buttons and keys follow in the order the device reported them. */
	for (index = 0; index < device->frame_count; index++) {
		event = &device->frame[index];

		/* Only key-type events are buttons or keys. */
		if (event->type != EV_KEY)
			continue;

		/* Autorepeat (value 2) is not forwarded; the client is told not to repeat either. */
		if (event->value != 0 && event->value != 1)
			continue;

		/* A pointer's mouse buttons become wl_pointer buttons with their BTN_ code. */
		if (device->pointer &&
		    event->code >= INPUT_BUTTON_FIRST &&
		    event->code <= INPUT_BUTTON_LAST) {
			kwl_seat_button(server, time, event->code, (uint32_t)event->value);
			pointer_activity = 1;
			continue;
		}

		/* The rest of the button block is neither a mouse button nor a key. */
		if (event->code >= INPUT_BUTTON_BLOCK_FIRST && event->code < INPUT_BUTTON_BLOCK_END)
			continue;

		/* A keyboard's keys become wl_keyboard keys with their evdev code. */
		if (device->keyboard)
			apply_key(server, time, event->code, event->value);
	}

	/* The user's natural scrolling for a mouse (ws089-p007, p024) turns the wheel round. */
	if (server->mouse_natural != 0) {
		wheel = -wheel;
		horizontal_wheel = -horizontal_wheel;
	}

	/* Wheel notches scroll; evdev counts up as positive, Wayland counts down as positive. */
	if (wheel != 0 || horizontal_wheel != 0) {
		kwl_seat_axis(server, time, -wheel, horizontal_wheel);
		pointer_activity = 1;
	}

	/* Version 5 pointers are told where this report's events end. */
	if (pointer_activity)
		kwl_seat_frame(server);

	/* Succeeded: the report has been delivered. */
	return;
}

/*
 * Applies one report of a touch pad: its events go to the touch pad layer,
 * and the layer's actions move the pointer, press its buttons and scroll
 * (ws159-p004).
 */
static void
apply_touchpad(
	struct kwl_server *server,
	struct kwl_input_device *device,
	uint32_t time)
{
	struct kwl_touchpad_actions actions;
	const struct input_event *event;
	unsigned index;

	/* The report's events, in order. */
	for (index = 0; index < device->frame_count; index++) {
		event = &device->frame[index];
		kwl_touchpad_event(&device->pad, event->type, event->code, event->value);
	}

	/* The report's end: what the fingers did. */
	kwl_touchpad_frame(&device->pad, kwl_milliseconds(), &actions);
	apply_touchpad_actions(server, &device->pad, &actions, time);

	/* Succeeded: the report has been applied. */
	return;
}

/*
 * Passes the end of a touch pad's touch on the lock screen to it (ws187-p002):
 * with the travel of a gesture up that ended (two fingers from the bottom
 * edge, three fingers up), or 0 for any other end.
 */
static void
lock_pad_gesture(
	struct kwl_server *server,
	const struct kwl_touchpad_action *action)
{
	int64_t gesture_up_um;

	/* A beginning or a step is no end. */
	if (action->phase == KWL_TOUCHPAD_PHASE_BEGIN)
		return;
	if (action->phase == KWL_TOUCHPAD_PHASE_UPDATE)
		return;

	/* The travel of a gesture up that ended rather than was given up. */
	gesture_up_um = 0;
	if (action->phase == KWL_TOUCHPAD_PHASE_END) {
		if (action->gesture == KWL_TOUCHPAD_GESTURE_BOTTOM2)
			gesture_up_um = action->travel_um;
		if (action->gesture == KWL_TOUCHPAD_GESTURE_UP3)
			gesture_up_um = action->travel_um;
	}

	/* The lock screen counts it, and the next touch starts afresh. */
	kwl_greeter_pad_end(server, gesture_up_um);
}

/* Carries out the actions of the touch pad layer on the seat, as a mouse's report would. */
static void
apply_touchpad_actions(
	struct kwl_server *server,
	const struct kwl_touchpad *pad,
	const struct kwl_touchpad_actions *actions,
	uint32_t time)
{
	const struct kwl_touchpad_action *action;
	int64_t down_um;
	unsigned activity;
	unsigned index;
	int moved;
	int taken;

	/* Each action in order. */
	activity = 0;
	for (index = 0; index < actions->count; index++) {
		action = &actions->actions[index];

		/* Its kind. */
		switch (action->kind) {
		case KWL_TOUCHPAD_MOTION:
			/* The pointer moves, at the user's speed. */
			moved = pointer_move(server, action->dx, action->dy, time);
			if (moved)
				activity = 1;
			break;
		case KWL_TOUCHPAD_BUTTON:
			/* A button's press or release. */
			kwl_seat_button(server, time, action->button, action->pressed);
			activity = 1;
			break;
		case KWL_TOUCHPAD_SCROLL:
			/* On the lock screen two fingers up may open it: their own way, the scrolling's direction turned back (ws187-p002). */
			if (server->locked) {
				down_um = (int64_t)action->vertical * KWL_TOUCHPAD_NOTCH_UM;
				if (pad->natural_scroll)
					down_um = -down_um;
				kwl_greeter_pad_scroll(server, down_um);
				break;
			}

			/* The switcher or Wiseview, while it shows, takes the two fingers' swipe (shell.c, ws142-p009). */
			taken = kwl_glass_pad_scroll(server, action->vertical, action->horizontal, pad->natural_scroll);
			if (taken)
				break;

			/* Otherwise the fingers' scrolling, in the wheel's units (vertical positive down, as the seat takes them). */
			kwl_seat_axis_finger(server, time, action->vertical, action->horizontal, action->vertical_units, action->horizontal_units);
			activity = 1;
			break;
		case KWL_TOUCHPAD_GESTURE:
			/* On the lock screen a touch's end may be a gesture up far enough to open it (ws187-p002). */
			if (server->locked)
				lock_pad_gesture(server, action);

			/* A gesture is the shell's (ws142-p003); the end of two fingers' scroll also ends the client's scrolling (BUG-211). */
			kwl_glass_gesture(server, action->gesture, action->phase, action->travel_um, action->speed);
			if (action->gesture == KWL_TOUCHPAD_GESTURE_SWIPE2 && action->phase == KWL_TOUCHPAD_PHASE_END)
				kwl_seat_axis_stop(server, time);
			break;
		default:
			break;
		}
	}

	/* Version 5 pointers are told where the actions end. */
	if (activity)
		kwl_seat_frame(server);

	/* Succeeded: the actions are carried out. */
	return;
}

/*
 * Gives every touch pad attached the touch pads' acceleration and
 * scrolling direction again, after the settings changed them (ws089-p024).
 */
void
kwl_input_touchpads_changed(
	struct kwl_server *server)
{
	unsigned index;

	/* Every touch pad in use. */
	for (index = 0; index < KWL_INPUT_MAX; index++) {
		/* A free slot, or a device that is no touch pad. */
		if (!server->inputs[index].live || !server->inputs[index].touchpad)
			continue;

		/* The new feel, from the next report on. */
		kwl_touchpad_set_feel(&server->inputs[index].pad, server->touchpad_acceleration, server->touchpad_natural);
	}
}

/*
 * Lets time pass for the touch pads: a tap whose drag did not come
 * completes its click (touchpad.c).
 */
void
kwl_input_tick(
	struct kwl_server *server,
	uint64_t now)
{
	struct kwl_touchpad_actions actions;
	unsigned index;

	/* Every touch pad in use. */
	for (index = 0; index < KWL_INPUT_MAX; index++) {
		/* A free slot, or a device that is no touch pad, has no time to keep. */
		if (!server->inputs[index].live || !server->inputs[index].touchpad)
			continue;

		/* Its layer's timers, and what they give. */
		kwl_touchpad_tick(&server->inputs[index].pad, now, &actions);
		apply_touchpad_actions(server, &server->inputs[index].pad, &actions, (uint32_t)now);
	}

	/* Succeeded: the timers have run. */
	return;
}

/*
 * Moves the pointer by a relative motion at the user's speed, kept on the
 * output, as a mouse's motion does (apply_frame).  Reports whether it moved.
 */
static int
pointer_move(
	struct kwl_server *server,
	int64_t delta_x,
	int64_t delta_y,
	uint32_t time)
{
	int32_t x;
	int32_t y;
	int32_t old_x;
	int32_t old_y;

	/* The device moved the pointer: its arrow is shown from now on (ws035-p116). */
	if (server->pointer_unmoved && (delta_x != 0 || delta_y != 0)) {
		server->pointer_unmoved = 0U;
		kwl_damage_pointer(server, server->pointer_x, server->pointer_y);
		printf("KWL CURSOR shown by=touchpad\n");
	}

	/* The motion at the touch pads' speed (ws089-p007, p024, a percentage), the hundredths carried. */
	if (server->touchpad_speed != 100 && (delta_x != 0 || delta_y != 0)) {
		delta_x = delta_x * server->touchpad_speed + server->pointer_remainder_x;
		delta_y = delta_y * server->touchpad_speed + server->pointer_remainder_y;
		server->pointer_remainder_x = delta_x % 100;
		server->pointer_remainder_y = delta_y % 100;
		delta_x /= 100;
		delta_y /= 100;
	}

	/* The new place, over the outputs shown (ws113-p007). */
	kwl_pointer_relative(server, delta_x, delta_y, &x, &y);
	if (x == server->pointer_x && y == server->pointer_y)
		return 0;

	/* The pointer moves; the seat hears it, and the cursor is drawn at its new place. */
	old_x = server->pointer_x;
	old_y = server->pointer_y;
	server->pointer_x = x;
	server->pointer_y = y;
	kwl_seat_motion(server, time);
	kwl_damage_pointer(server, old_x, old_y);

	/* Succeeded: the pointer moved. */
	return 1;
}

/* Tells whether a node is a touch pad: multitouch protocol B, and a pointer by its properties. */
static int
touchpad_node(
	const struct kl_backend_input_caps *capabilities)
{
	int present;

	/* Multitouch protocol B. */
	present = multitouch(capabilities);
	if (!present)
		return 0;

	/* A pointer, not a screen (a backend that reads no properties offers no touch pad). */
	present = bit_is_set(capabilities->properties, INPUT_PROP_POINTER);
	if (!present)
		return 0;

	/* Succeeded: the node is a touch pad. */
	return 1;
}

/*
 * Takes a touch pad into the seat: its fingers' resolution starts its
 * layer, and it is the seat's pointer.
 */
static int
attach_touchpad(
	struct kwl_server *server,
	int descriptor,
	const char *path)
{
	struct kwl_input_device *device;
	struct input_absinfo x;
	struct input_absinfo y;
	unsigned index;
	int error;

	/* The fingers' axes; their resolution says how far a unit is. */
	memset(&x, 0, sizeof(x));
	memset(&y, 0, sizeof(y));
	error = kl_backend_input_absinfo(descriptor, ABS_MT_POSITION_X, &x);
	if (error != 0)
		return error;
	error = kl_backend_input_absinfo(descriptor, ABS_MT_POSITION_Y, &y);
	if (error != 0)
		return error;

	/* Find a free slot in the fixed device table. */
	device = NULL;
	for (index = 0; index < KWL_INPUT_MAX; index++) {
		/* A slot not in use can hold the new device. */
		if (!server->inputs[index].live) {
			device = &server->inputs[index];
			break;
		}
	}

	/* A full table cannot take another device (the caller's backend closes it). */
	if (device == NULL)
		return ENOSPC;

	/* The slot describes this node: a pointer moved by its touch pad layer. */
	memset(device, 0, sizeof(*device));
	device->fd = descriptor;
	device->pointer = 1;
	device->touchpad = 1;
	snprintf(device->path, sizeof(device->path), "%s", path);
	kwl_touchpad_init(&device->pad, x.resolution, y.resolution);
	kwl_touchpad_set_range(&device->pad, x.minimum, x.maximum, y.minimum, y.maximum);
	kwl_touchpad_set_feel(&device->pad, server->touchpad_acceleration, server->touchpad_natural);

	/* The slot is published only when it is completely filled in. */
	device->live = 1;

	/* One line lets a test see the touch pad and its resolution. */
	printf("KWL INPUT device=%s kind=touchpad abs=0 resolution=%d,%d size=%d,%d\n", device->path, x.resolution, y.resolution, x.maximum, y.maximum);

	/* Bound seats learn that a pointer is there. */
	update_capabilities(server);

	/* Succeeded: the event loop now reads this device. */
	return 0;
}

/* Tracks the modifier keys and delivers one key with any modifier change. */
static void
apply_key(
	struct kwl_server *server,
	uint32_t time,
	uint32_t key,
	int32_t value)
{
	unsigned held;
	uint32_t modifiers;
	uint32_t locked;

	/* Each modifier key has its own held bit, so left and right are independent. */
	held = 0;
	switch (key) {
	case KEY_LEFTSHIFT:
		held = HELD_LEFTSHIFT;
		break;
	case KEY_RIGHTSHIFT:
		held = HELD_RIGHTSHIFT;
		break;
	case KEY_LEFTCTRL:
		held = HELD_LEFTCTRL;
		break;
	case KEY_RIGHTCTRL:
		held = HELD_RIGHTCTRL;
		break;
	case KEY_LEFTALT:
		held = HELD_LEFTALT;
		break;
	case KEY_RIGHTALT:
		held = HELD_RIGHTALT;
		break;
	case INPUT_KEY_LEFTMETA:
		held = HELD_LEFTMETA;
		break;
	case INPUT_KEY_RIGHTMETA:
		held = HELD_RIGHTMETA;
		break;
	default:
		break;
	}

	/*
	 * modifier_keys records which modifier keys are down right now; the
	 * depressed mask sent to clients is derived from it.
	 */
	if (value != 0) {
		server->modifier_keys |= held;
	} else {
		server->modifier_keys &= ~held;
	}

	/* Fold the held keys into the xkb default modifier bits. */
	modifiers = 0;
	if ((server->modifier_keys & (HELD_LEFTSHIFT | HELD_RIGHTSHIFT)) != 0)
		modifiers |= MODIFIER_SHIFT;
	if ((server->modifier_keys & (HELD_LEFTCTRL | HELD_RIGHTCTRL)) != 0)
		modifiers |= MODIFIER_CONTROL;
	if ((server->modifier_keys & (HELD_LEFTALT | HELD_RIGHTALT)) != 0)
		modifiers |= MODIFIER_ALT;
	if ((server->modifier_keys & (HELD_LEFTMETA | HELD_RIGHTMETA)) != 0)
		modifiers |= MODIFIER_META;

	/* A press of Caps Lock or Num Lock toggles its lock (a kernel repeat, value 2, does not). */
	locked = server->locked_modifiers;
	if (value == 1 && key == INPUT_KEY_CAPSLOCK)
		locked ^= MODIFIER_CAPS_LOCK;
	if (value == 1 && key == INPUT_KEY_NUMLOCK)
		locked ^= MODIFIER_NUM_LOCK;

	/* The key itself is reported first. */
	kwl_seat_key(server, time, key, (uint32_t)value);

	/* A changed mask follows the key that changed it. */
	if (modifiers != server->modifiers || locked != server->locked_modifiers) {
		server->modifiers = modifiers;
		server->locked_modifiers = locked;
		kwl_seat_modifiers(server);
	}

	/* Succeeded: the key and the modifier state are delivered. */
	return;
}

/* Maps one absolute axis value onto 0 .. size - 1 surface pixels. */
static int32_t
scale_absolute(
	int32_t value,
	int32_t minimum,
	int32_t maximum,
	uint32_t size)
{
	int64_t offset;
	int64_t range;

	/* The low end of the range, and anything below it, is the first pixel. */
	if (size <= 1U || value <= minimum)
		return 0;

	/* The high end of the range, and anything above it, is the last pixel. */
	if (value >= maximum)
		return (int32_t)size - 1;

	/* Everything between scales linearly, rounding down. */
	offset = (int64_t)value - minimum;
	range = (int64_t)maximum - minimum;

	/* Succeeded: the pixel the value falls on. */
	return (int32_t)((offset * ((int64_t)size - 1)) / range);
}

/* Keeps a pointer coordinate inside 0 .. size - 1. */
static int32_t
clamp_position(
	int64_t position,
	uint32_t size)
{
	/* Nothing lies left of or above the surface. */
	if (position < 0)
		return 0;

	/* Nothing lies right of or below the surface. */
	if (position >= (int64_t)size)
		return (int32_t)size - 1;

	/* Succeeded: the position is already on the surface. */
	return (int32_t)position;
}

/* Converts an evdev timestamp to the wrapping millisecond count Wayland uses. */
static uint32_t
event_time(
	const struct input_event *event)
{
	uint64_t milliseconds;

	/* Seconds and microseconds become milliseconds, keeping the low 32 bits. */
	milliseconds = (uint64_t)event->time.tv_sec * 1000U;
	milliseconds += (uint64_t)event->time.tv_usec / 1000U;

	/* Succeeded: the event time with an undefined base, as the protocol allows. */
	return (uint32_t)milliseconds;
}

/* Recomputes the seat's capability bits and tells bound seats when they change. */
static void
update_capabilities(
	struct kwl_server *server)
{
	unsigned capabilities;
	unsigned index;

	/* The seat offers each class some open device provides. */
	capabilities = 0;
	for (index = 0; index < KWL_INPUT_MAX; index++) {
		/* A free slot provides nothing. */
		if (!server->inputs[index].live)
			continue;

		/*
		 * wl_seat's pointer, keyboard and touch capability bits; a pen
		 * tablet and a touch screen are also a pointer (their fallback for
		 * a client without the tablet or wl_touch).
		 */
		if (server->inputs[index].pointer)
			capabilities |= 1U;
		if (server->inputs[index].tablet)
			capabilities |= 1U;
		if (server->inputs[index].keyboard)
			capabilities |= 2U;
		if (server->inputs[index].touch)
			capabilities |= 1U | 4U;
	}

	/* Unchanged capabilities need no event. */
	if (capabilities == server->capabilities)
		return;

	/* Bound seats learn the new set. */
	server->capabilities = capabilities;
	kwl_seat_capabilities(server);

	/* Succeeded: every seat binding agrees with the open devices. */
	return;
}

/*
 * Adopts an open evdev descriptor as a pen tablet (tablet.c, WS079 p003).
 *
 * The device takes ownership of the descriptor when it returns 0; otherwise
 * (no slot free, or the tablet cannot take another device.
 */
static int
attach_tablet(
	struct kwl_server *server,
	int descriptor,
	const char *path)
{
	struct kwl_input_device *device;
	unsigned index;
	int error;

	/* Find a free slot in the fixed device table. */
	device = NULL;
	for (index = 0; index < KWL_INPUT_MAX; index++) {
		/* A slot not in use can hold the new device. */
		if (!server->inputs[index].live) {
			device = &server->inputs[index];
			break;
		}
	}

	/* A full table cannot take another device (the caller's backend closes it). */
	if (device == NULL)
		return ENOSPC;

	/* The slot now describes this node; it is neither a pointer nor a keyboard of its own. */
	memset(device, 0, sizeof(*device));
	device->fd = descriptor;
	device->tablet = 1;
	snprintf(device->path, sizeof(device->path), "%s", path);

	/* The tablet reads the axes and tells the bound tablet seats (tablet.c). */
	error = kwl_tablet_add(server, device);
	if (error != 0) {
		device->fd = -1;
		device->tablet = 0;
		return error;
	}

	/* The slot is published only when it is completely filled in. */
	device->live = 1;

	/* One line lets a test see which devices the seat uses. */
	printf("KWL INPUT device=%s kind=tablet abs=1\n", device->path);

	/* Bound seats learn that a pointer (the pen's fallback) is there. */
	update_capabilities(server);

	/* Succeeded: the event loop now reads this device. */
	return 0;
}

/*
 * Adopts an open evdev descriptor as a touch screen (touch.c, WS079 p013).
 *
 * The device takes ownership of the descriptor when it returns 0; otherwise
 * (no slot free, or the touch screens cannot take another device) the
 * caller still owns it.
 */
static int
attach_touch(
	struct kwl_server *server,
	int descriptor,
	const char *path)
{
	struct kwl_input_device *device;
	unsigned index;
	int error;

	/* Find a free slot in the fixed device table. */
	device = NULL;
	for (index = 0; index < KWL_INPUT_MAX; index++) {
		/* A slot not in use can hold the new device. */
		if (!server->inputs[index].live) {
			device = &server->inputs[index];
			break;
		}
	}

	/* A full table cannot take another device (the caller's backend closes it). */
	if (device == NULL)
		return ENOSPC;

	/* The slot now describes this node; it is neither a pointer nor a keyboard of its own. */
	memset(device, 0, sizeof(*device));
	device->fd = descriptor;
	device->touch = 1;
	snprintf(device->path, sizeof(device->path), "%s", path);

	/* The touch screen reads its range (touch.c). */
	error = kwl_touch_add(device);
	if (error != 0) {
		device->fd = -1;
		device->touch = 0;
		return error;
	}

	/* The slot is published only when it is completely filled in. */
	device->live = 1;

	/* One line lets a test see which devices the seat uses. */
	printf("KWL INPUT device=%s kind=touch abs=1\n", device->path);

	/* Bound seats learn that a touch screen (and the pointer of its fallback) is there. */
	update_capabilities(server);

	/* Succeeded: the event loop now reads this device. */
	return 0;
}

/*
 * Reads whole events from a device through the backend.  A device the seat
 * revoked (ENODEV) that logind keeps for its later resume is set aside (its
 * record's descriptor -1) and reads as nothing ready (EAGAIN): the seat's
 * input_resumed or input_gone comes later (ws105-p009, ws131-p007).
 */
static ssize_t
input_read(
	struct kwl_server *server,
	struct kwl_input_device *device,
	struct input_event *events,
	size_t capacity)
{
	ssize_t count;
	int retained;

	/* The events, or the reason there are none. */
	count = kl_backend_input_read(device->fd, events, capacity);
	if (count >= 0 || errno != ENODEV)
		return count;

	/* A revoked device the seat keeps waits for the seat; another closes as usual. */
	retained = kl_backend_seat_device_revoked(server->backend, device->fd);
	if (retained == 0) {
		errno = ENODEV;
		return -1;
	}

	/* Retained: the descriptor is forgotten until the seat gives the device back. */
	printf("KWL SEAT input_revoked path=%s lease=retained\n", device->path);
	device->fd = -1;
	errno = EAGAIN;
	return -1;
}

/* Reports whether a node speaks multitouch protocol B: slots, tracking numbers and both places. */
static int
multitouch(
	const struct kl_backend_input_caps *capabilities)
{
	int present;

	/* Absolute axes at all. */
	present = bit_is_set(capabilities->event, EV_ABS);
	if (!present)
		return 0;

	/* The slot the finger events address, and the finger's number. */
	present = bit_is_set(capabilities->absolute, ABS_MT_SLOT);
	if (!present)
		return 0;
	present = bit_is_set(capabilities->absolute, ABS_MT_TRACKING_ID);
	if (!present)
		return 0;

	/* The finger's place across and down. */
	present = bit_is_set(capabilities->absolute, ABS_MT_POSITION_X);
	if (!present)
		return 0;
	present = bit_is_set(capabilities->absolute, ABS_MT_POSITION_Y);
	if (!present)
		return 0;

	/* Succeeded: the node is a touch screen. */
	return 1;
}

/*
 * Tells whether an Alt key is held now (the network's icon opens its
 * details on an Alt+click, network.c, ws099-p032).
 */
int
kwl_input_alt_held(
	const struct kwl_server *server)
{
	/* Either Alt. */
	if ((server->modifier_keys & (HELD_LEFTALT | HELD_RIGHTALT)) != 0U)
		return 1;

	/* Neither. */
	return 0;
}
