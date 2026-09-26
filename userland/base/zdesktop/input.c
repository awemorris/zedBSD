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
 * may be both.  Nodes are read without blocking.  Events are gathered until
 * SYN_REPORT and applied as one group: motion first, then buttons and keys in
 * the order they arrived, then wheel scrolling, then a pointer frame.
 */

#include "zwl.h"
#include <sys/ioctl.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The directory whose eventN nodes are the evdev devices. */
#define INPUT_DIRECTORY		"/dev/input"

/* Bound the reads one device gets per event-loop pass, so others still run. */
#define INPUT_READS_PER_PASS	16U

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

/*
 * The capability bitmaps of one evdev node, as EVIOCGBIT reports them.
 *
 * One instance lives on the stack while a node is classified.
 */
struct input_capabilities {
	unsigned long event[EV_MAX / (8U * sizeof(unsigned long)) + 1U];
	unsigned long key[KEY_MAX / (8U * sizeof(unsigned long)) + 1U];
	unsigned long relative[REL_MAX / (8U * sizeof(unsigned long)) + 1U];
	unsigned long absolute[ABS_MAX / (8U * sizeof(unsigned long)) + 1U];
};

static int event_node_name(const char *name);
static int device_open(struct zwl_server *server, const char *path);
static void probe_device(struct zwl_server *server, const char *path);
static int read_capabilities(int descriptor, struct input_capabilities *capabilities);
static int read_ranges(int descriptor, struct input_absinfo *x, struct input_absinfo *y);
static int bit_is_set(const unsigned long *bits, unsigned code);
static void consume_event(struct zwl_server *server, struct zwl_input_device *device, const struct input_event *event);
static void apply_frame(struct zwl_server *server, struct zwl_input_device *device, uint32_t time);
static void apply_key(struct zwl_server *server, uint32_t time, uint32_t key, int32_t value);
static int32_t scale_absolute(int32_t value, int32_t minimum, int32_t maximum, uint32_t size);
static int32_t clamp_position(int64_t position, uint32_t size);
static uint32_t event_time(const struct input_event *event);
static void update_capabilities(struct zwl_server *server);

/*
 * Opens every evdev pointer and keyboard not already open.
 *
 * A missing directory leaves the seat without devices; it is not an error.
 * The event loop calls this periodically so late devices are picked up.
 */
void
zwl_input_scan(
	struct zwl_server *server)
{
	DIR *directory;
	struct dirent *entry;
	char path[ZWL_INPUT_PATH_MAX];
	int length;
	int valid;
	int open_already;

	/* The next rescan is due one period from now. */
	server->input_scan_time = zwl_milliseconds();

	/* Without the directory there is nothing to read. */
	directory = opendir(INPUT_DIRECTORY);
	if (directory == NULL)
		return;

	/* Every eventN entry is a candidate device. */
	while (1) {
		/* The end of the directory ends the scan. */
		entry = readdir(directory);
		if (entry == NULL)
			break;

		/* Only eventN nodes speak evdev. */
		valid = event_node_name(entry->d_name);
		if (!valid)
			continue;

		/* An overlong name cannot be an ordinary device node. */
		length = snprintf(path, sizeof(path), "%s/%s", INPUT_DIRECTORY, entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(path))
			continue;

		/* A node already being read is left alone. */
		open_already = device_open(server, path);
		if (open_already)
			continue;

		/* Classify the node and keep it if it is a pointer or a keyboard. */
		probe_device(server, path);
	}

	/* The directory stream is no longer needed. */
	closedir(directory);

	/* Succeeded: every present pointer and keyboard is open. */
	return;
}

/*
 * Adopts an open evdev descriptor as a pointer, a keyboard or both.
 *
 * The device takes ownership of the descriptor, closing it when no slot is
 * free.  An absolute pointer supplies both axis ranges; a relative one passes
 * NULL for both.
 */
int
zwl_input_attach(
	struct zwl_server *server,
	int descriptor,
	const char *path,
	unsigned pointer,
	unsigned keyboard,
	const struct input_absinfo *x,
	const struct input_absinfo *y)
{
	struct zwl_input_device *device;
	unsigned index;

	/* Find a free slot in the fixed device table. */
	device = NULL;
	for (index = 0; index < ZWL_INPUT_MAX; index++) {
		/* A slot not in use can hold the new device. */
		if (!server->inputs[index].live) {
			device = &server->inputs[index];
			break;
		}
	}

	/* A full table cannot take another device. */
	if (device == NULL) {
		close(descriptor);
		return ENOSPC;
	}

	/* The slot now describes this node. */
	memset(device, 0, sizeof(*device));
	device->fd = descriptor;
	device->pointer = pointer;
	device->keyboard = keyboard;
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
		printf("ZWL INPUT device=%s kind=pointer abs=%u\n", device->path, device->absolute);
	if (keyboard)
		printf("ZWL INPUT device=%s kind=keyboard abs=0\n", device->path);

	/* Bound seats learn about a new class of device. */
	update_capabilities(server);

	/* Succeeded: the event loop now reads this device. */
	return 0;
}

/*
 * Drains the events a device has ready and applies each completed report.
 *
 * A read error other than "nothing ready" means the device went away, and it
 * is closed.
 */
void
zwl_input_read(
	struct zwl_server *server,
	struct zwl_input_device *device)
{
	struct input_event events[32];
	ssize_t bytes;
	size_t count;
	size_t index;
	unsigned reads;
	int error;

	/* A bounded number of reads keeps one busy device from starving the loop. */
	for (reads = 0; reads < INPUT_READS_PER_PASS; reads++) {
		/* A closed slot has nothing more to read. */
		if (!device->live)
			return;

		/* Read as many whole events as the buffer holds. */
		bytes = read(device->fd, events, sizeof(events));
		if (bytes < 0) {
			error = errno;

			/* Nothing more is ready; the next poll will say when there is. */
			if (error == EAGAIN || error == EWOULDBLOCK)
				return;

			/* An interrupted read is simply retried. */
			if (error == EINTR)
				continue;

			/* Any other failure means the device is gone. */
			printf("ZWL INPUT_CLOSED device=%s errno=%d\n", device->path, error);
			zwl_input_close(server, device);
			return;
		}

		/* End of file or a torn event means the node no longer speaks evdev. */
		if (bytes == 0 || ((size_t)bytes % sizeof(events[0])) != 0) {
			printf("ZWL INPUT_CLOSED device=%s errno=%d\n", device->path, EIO);
			zwl_input_close(server, device);
			return;
		}

		/* Apply the events in the order the device produced them. */
		count = (size_t)bytes / sizeof(events[0]);
		for (index = 0; index < count; index++)
			consume_event(server, device, &events[index]);
	}

	/* Succeeded: this pass's share of the device's events has been applied. */
	return;
}

/*
 * Closes one device and tells bound seats when a device class disappears.
 */
void
zwl_input_close(
	struct zwl_server *server,
	struct zwl_input_device *device)
{
	/* A slot that is not in use has no descriptor. */
	if (!device->live)
		return;

	/* The slot is free once its descriptor is closed. */
	close(device->fd);
	device->fd = -1;
	device->live = 0;

	/* Bound seats learn that a class of device may be gone. */
	update_capabilities(server);

	/* Succeeded: the device is no longer read. */
	return;
}

/*
 * Closes every device during service shutdown without notifying clients.
 */
void
zwl_input_cleanup(
	struct zwl_server *server)
{
	unsigned index;

	/* Every slot in use owns one descriptor. */
	for (index = 0; index < ZWL_INPUT_MAX; index++) {
		/* A free slot owns nothing. */
		if (!server->inputs[index].live)
			continue;

		/* Close the descriptor and free the slot. */
		close(server->inputs[index].fd);
		server->inputs[index].fd = -1;
		server->inputs[index].live = 0;
	}

	/* No client remains to hear about it, so the capabilities are simply cleared. */
	server->capabilities = 0;

	/* Succeeded: the seat holds no device descriptors. */
	return;
}

/* Reports whether a directory entry is named eventN. */
static int
event_node_name(
	const char *name)
{
	const char *cursor;

	/* The name starts with "event" and has at least one more character. */
	if (strncmp(name, "event", 5) != 0 || name[5] == '\0')
		return 0;

	/* Everything after the prefix is a decimal digit. */
	for (cursor = name + 5; *cursor != '\0'; cursor++) {
		/* Any other character makes it some other kind of node. */
		if (*cursor < '0' || *cursor > '9')
			return 0;
	}

	/* Succeeded: the entry is an evdev node. */
	return 1;
}

/* Reports whether a device node is already open in the table. */
static int
device_open(
	struct zwl_server *server,
	const char *path)
{
	unsigned index;
	int same;

	/* Compare the path with every slot in use. */
	for (index = 0; index < ZWL_INPUT_MAX; index++) {
		/* A free slot names no device. */
		if (!server->inputs[index].live)
			continue;

		/* The same path means the same node. */
		same = strcmp(server->inputs[index].path, path);
		if (same == 0)
			return 1;
	}

	/* The node is not open. */
	return 0;
}

/* Opens one node, classifies it and keeps it when it is a pointer or keyboard. */
static void
probe_device(
	struct zwl_server *server,
	const char *path)
{
	struct input_capabilities capabilities;
	struct input_absinfo x;
	struct input_absinfo y;
	unsigned pointer;
	unsigned keyboard;
	unsigned absolute;
	int has_type;
	int has_first;
	int has_second;
	int descriptor;
	int error;

	/* The seat only reads, never blocks and does not pass the node to children. */
	descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (descriptor < 0)
		return;

	/* The capability bitmaps decide what kind of device this is. */
	error = read_capabilities(descriptor, &capabilities);
	if (error != 0) {
		close(descriptor);
		return;
	}

	/* A keyboard reports key events including the letter keys A and Z. */
	keyboard = 0;
	has_type = bit_is_set(capabilities.event, EV_KEY);
	has_first = bit_is_set(capabilities.key, KEY_A);
	has_second = bit_is_set(capabilities.key, KEY_Z);
	if (has_type && has_first && has_second)
		keyboard = 1;

	/* An absolute pointer reports ABS_X and ABS_Y. */
	absolute = 0;
	has_type = bit_is_set(capabilities.event, EV_ABS);
	has_first = bit_is_set(capabilities.absolute, ABS_X);
	has_second = bit_is_set(capabilities.absolute, ABS_Y);
	if (has_type && has_first && has_second) {
		/* Both axis ranges must be readable and nonempty to be mapped. */
		error = read_ranges(descriptor, &x, &y);
		if (error == 0)
			absolute = 1;
	}

	/* A relative pointer reports REL_X and REL_Y. */
	pointer = absolute;
	has_type = bit_is_set(capabilities.event, EV_REL);
	has_first = bit_is_set(capabilities.relative, REL_X);
	has_second = bit_is_set(capabilities.relative, REL_Y);
	if (has_type && has_first && has_second)
		pointer = 1;

	/* A node that is neither is not the seat's business. */
	if (!pointer && !keyboard) {
		close(descriptor);
		return;
	}

	/* Keep the node; an absolute pointer brings its ranges along. */
	if (absolute) {
		(void)zwl_input_attach(server, descriptor, path, pointer, keyboard, &x, &y);
	} else {
		(void)zwl_input_attach(server, descriptor, path, pointer, keyboard, NULL, NULL);
	}

	/* Succeeded: the node has been classified. */
	return;
}

/* Reads the event, key, relative and absolute capability bitmaps of one node. */
static int
read_capabilities(
	int descriptor,
	struct input_capabilities *capabilities)
{
	int error;

	/* Absent bitmaps read as empty. */
	memset(capabilities, 0, sizeof(*capabilities));

	/* The event types the node can produce. */
	error = ioctl(descriptor, EVIOCGBIT(0, sizeof(capabilities->event)), capabilities->event);
	if (error < 0)
		return errno;

	/* The key and button codes the node can produce. */
	error = ioctl(descriptor, EVIOCGBIT(EV_KEY, sizeof(capabilities->key)), capabilities->key);
	if (error < 0)
		return errno;

	/* The relative axes the node can produce. */
	error = ioctl(descriptor, EVIOCGBIT(EV_REL, sizeof(capabilities->relative)), capabilities->relative);
	if (error < 0)
		return errno;

	/* The absolute axes the node can produce. */
	error = ioctl(descriptor, EVIOCGBIT(EV_ABS, sizeof(capabilities->absolute)), capabilities->absolute);
	if (error < 0)
		return errno;

	/* Succeeded: the four bitmaps describe the node. */
	return 0;
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
	error = ioctl(descriptor, EVIOCGABS(ABS_X), x);
	if (error < 0)
		return errno;

	/* The vertical range maps onto the surface height. */
	memset(y, 0, sizeof(*y));
	error = ioctl(descriptor, EVIOCGABS(ABS_Y), y);
	if (error < 0)
		return errno;

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
	struct zwl_server *server,
	struct zwl_input_device *device,
	const struct input_event *event)
{
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

		/* An intact report is applied as one group. */
		apply_frame(server, device, event_time(event));
		device->frame_count = 0;
		return;
	}

	/* Other synchronization events and a damaged report carry nothing to keep. */
	if (event->type == EV_SYN || device->discarding)
		return;

	/* The exit summary counts every evdev event that was not a synchronization. */
	server->input_events++;

	/* A report too large to hold is thrown away like a dropped one. */
	if (device->frame_count == ZWL_INPUT_FRAME_MAX) {
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
	struct zwl_server *server,
	struct zwl_input_device *device,
	uint32_t time)
{
	const struct input_event *event;
	int64_t delta_x;
	int64_t delta_y;
	int32_t wheel;
	int32_t horizontal_wheel;
	int32_t x;
	int32_t y;
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

	/* An absolute report places the pointer; relative movement is added and clamped. */
	x = server->pointer_x;
	y = server->pointer_y;
	if (absolute_seen) {
		x = scale_absolute(device->abs_x, device->abs_x_minimum, device->abs_x_maximum, server->width);
		y = scale_absolute(device->abs_y, device->abs_y_minimum, device->abs_y_maximum, server->height);
	}

	x = clamp_position((int64_t)x + delta_x, server->width);
	y = clamp_position((int64_t)y + delta_y, server->height);

	/* A changed position is reported as motion before any button of the same report. */
	pointer_activity = 0;
	if (x != server->pointer_x || y != server->pointer_y) {
		server->pointer_x = x;
		server->pointer_y = y;
		zwl_seat_motion(server, time);

		/* Window mode draws the cursor at its new place. */
		server->dirty = 1;
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
			zwl_seat_button(server, time, event->code, (uint32_t)event->value);
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

	/* Wheel notches scroll; evdev counts up as positive, Wayland counts down as positive. */
	if (wheel != 0 || horizontal_wheel != 0) {
		zwl_seat_axis(server, time, -wheel, horizontal_wheel);
		pointer_activity = 1;
	}

	/* Version 5 pointers are told where this report's events end. */
	if (pointer_activity)
		zwl_seat_frame(server);

	/* Succeeded: the report has been delivered. */
	return;
}

/* Tracks the modifier keys and delivers one key with any modifier change. */
static void
apply_key(
	struct zwl_server *server,
	uint32_t time,
	uint32_t key,
	int32_t value)
{
	unsigned held;
	uint32_t modifiers;

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

	/* The key itself is reported first. */
	zwl_seat_key(server, time, key, (uint32_t)value);

	/* A changed mask follows the key that changed it. */
	if (modifiers != server->modifiers) {
		server->modifiers = modifiers;
		zwl_seat_modifiers(server);
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
	struct zwl_server *server)
{
	unsigned capabilities;
	unsigned index;

	/* The seat offers each class some open device provides. */
	capabilities = 0;
	for (index = 0; index < ZWL_INPUT_MAX; index++) {
		/* A free slot provides nothing. */
		if (!server->inputs[index].live)
			continue;

		/* wl_seat's pointer and keyboard capability bits. */
		if (server->inputs[index].pointer)
			capabilities |= 1U;
		if (server->inputs[index].keyboard)
			capabilities |= 2U;
	}

	/* Unchanged capabilities need no event. */
	if (capabilities == server->capabilities)
		return;

	/* Bound seats learn the new set. */
	server->capabilities = capabilities;
	zwl_seat_capabilities(server);

	/* Succeeded: every seat binding agrees with the open devices. */
	return;
}
