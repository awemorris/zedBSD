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
 * Nodes are read without blocking by the input thread (input-thread.c,
 * ws213-p004), which gathers the events until SYN_REPORT, moves the
 * pointer in the cursor's box, and passes each report here in the order
 * the reports were made.  The main thread applies a report as one group:
 * motion first, then buttons and keys in the order they arrived, then
 * wheel scrolling, then a pointer frame.  The main thread opens and closes
 * the devices, attaching each to the input thread and detaching it before
 * its descriptor is closed.
 */

#include "kwl.h"
#include "data.h"
#include "tablet.h"
#include "touch.h"

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
static void apply_event(struct kwl_server *server, const struct kwl_input_event *event);
static void apply_frame(struct kwl_server *server, struct kwl_input_device *device, const struct kwl_input_event *event);
static void apply_report(struct kwl_server *server, struct kwl_input_device *device, const struct kwl_input_event *event);
static void apply_detached(struct kwl_server *server, struct kwl_input_device *device, const struct kwl_input_detached *detached);
static int apply_motion(struct kwl_server *server, const struct kwl_cursor_motion *motion, unsigned absolute, uint32_t time);
static void box_measure(struct kwl_server *server, const struct kwl_cursor_motion *motion);
static void apply_key(struct kwl_server *server, uint32_t time, uint32_t key, int32_t value);
static void update_capabilities(struct kwl_server *server);
static int attach_tablet(struct kwl_server *server, int descriptor, const char *path, unsigned direct);
static int attach_touch(struct kwl_server *server, int descriptor, const char *path);
static int multitouch(const struct kl_backend_input_caps *capabilities);
static int touchpad_node(const struct kl_backend_input_caps *capabilities);
static int attach_touchpad(struct kwl_server *server, int descriptor, const char *path);
static void apply_touchpad_actions(struct kwl_server *server, const struct kwl_input_pad *pad, uint32_t time);
static void lock_pad_gesture(struct kwl_server *server, const struct kwl_touchpad_action *action);
static void input_attach(struct kwl_server *server, struct kwl_input_device *device);
static void input_detach(struct kwl_server *server, struct kwl_input_device *device);
static int input_answer_later(struct kwl_server *server, struct kwl_input_device *device);
static void input_detach_failed(const char *site, const struct kwl_server *server, const struct kwl_input_device *device, unsigned count, uint64_t sequence, uint64_t asked_ms, int error);
static const char *input_kind_name(const struct kwl_input_device *device);
static const char *input_reason_name(enum kwl_input_detach_reason reason);
static void input_retire(struct kwl_server *server, struct kwl_input_device *device);
static void input_settings(struct kwl_server *server, struct kwl_input_settings *settings);
static void pointer_warp_place(const void *context, const struct kwl_plane_rect *outputs, int32_t *x, int32_t *y, unsigned *output);

/*
 * Opens the input devices not read yet and sets the time of the next scan:
 * on the device I/O thread where it runs (io.c, the seat is offered the
 * devices from a later pass), otherwise in libkeiland-backend's scan here,
 * which offers each through kwl_input_probe.
 */
void
kwl_input_scan(
	struct kwl_server *server)
{
	int error;

	/* The next rescan is due one period from now. */
	server->input_scan_time = kwl_milliseconds();

	/* The device I/O thread opens them. */
	error = kwl_io_input_find(server);
	if (error == 0)
		return;

	/*
	 * A finding under way began before what asked for this scan: the scan
	 * stays asked, and the pass that takes that finding (its end wakes the
	 * event loop) asks again.
	 */
	if (error == EBUSY) {
		server->input_scan_time = 0U;
		return;
	}

	/* Without the thread (or memory for its job) the backend finds, opens and offers the devices here. */
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
	unsigned direct;
	int on_display;
	int beside_display;
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
		/*
		 * A pen on the display points at it (INPUT_PROP_DIRECT, WS217); a
		 * pen tablet on the desk, or one whose node says nothing, does not.
		 */
		direct = 0U;
		on_display = bit_is_set(capabilities->properties, INPUT_PROP_DIRECT);
		if (on_display)
			direct = 1U;

		/* Both position ranges must be readable and nonempty to be mapped. */
		error = read_ranges(descriptor, &x, &y);
		if (error == 0) {
			error = attach_tablet(server, descriptor, path, direct);
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

	/*
	 * An absolute pointer points at the display it is on (QEMU's tablet,
	 * WS217), unless its node says it is a pointer beside it (a pen tablet
	 * without pressure, ws217-p003 review M5).
	 */
	direct = 1U;
	beside_display = bit_is_set(capabilities->properties, INPUT_PROP_POINTER);
	if (beside_display)
		direct = 0U;

	/* Keep the node; an absolute pointer brings its ranges along. */
	if (absolute) {
		error = kwl_input_attach(server, descriptor, path, pointer, keyboard, direct, &x, &y);
	} else {
		error = kwl_input_attach(server, descriptor, path, pointer, keyboard, 0U, NULL, NULL);
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
	unsigned direct,
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
	device->seat_fd = descriptor;
	device->pointer = pointer;
	device->keyboard = keyboard;
	snprintf(device->path, sizeof(device->path), "%s", path);

	/*
	 * An absolute pointer maps its axis ranges onto the display, from the
	 * place it reports now; one on the display maps onto its panel (WS217).
	 */
	if (pointer && x != NULL && y != NULL) {
		device->absolute = 1;
		device->direct = direct;
		device->abs_x_minimum = x->minimum;
		device->abs_x_maximum = x->maximum;
		device->abs_y_minimum = y->minimum;
		device->abs_y_maximum = y->maximum;
		device->reading.abs_x = x->value;
		device->reading.abs_y = y->value;
	}

	/* The slot is published only when it is completely filled in, and the input thread reads it. */
	device->live = 1;
	input_attach(server, device);

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
 * Forgets one device whose descriptor the seat closes (libkeiland-backend's
 * input_gone, ws131-p006): the input thread lets go of the descriptor, and
 * the record goes once the device's last event is applied (a touch pad's
 * release of its buttons among them).  Returns 1 when the seat is to wait
 * for kwl_input_answer's answer before it closes the descriptor.
 */
int
kwl_input_forget(
	struct kwl_server *server,
	struct kwl_input_device *device)
{
	int later;

	/* A slot not in use has nothing to forget. */
	if (!device->live)
		return 0;

	/* The input thread lets go, unless it has; one going already only answers. */
	later = input_answer_later(server, device);
	if (device->closing)
		return later;

	/* A device the input thread no longer reads goes now. */
	if (!later) {
		input_retire(server, device);
		return 0;
	}

	/* The input thread's last event of it retires the record. */
	device->closing = 1U;
	device->fd = -1;
	return 1;
}

/*
 * Makes the pipe the compositor's threads (and the backend's, which it is
 * lent to) wake the main thread with, before the backend opens.  It stays
 * until kwl_wake_close, after every thread that writes to it has ended.
 * Returns 0 or an errno value.
 */
int
kwl_wake_open(
	struct kwl_server *server)
{
	unsigned index;
	int error;

	/* Made once. */
	if (server->wake[0] >= 0)
		return 0;

	/* The wake pipe; neither end blocks or goes to a program the compositor starts. */
	error = pipe(server->wake);
	if (error != 0) {
		server->wake[0] = -1;
		server->wake[1] = -1;
		return errno;
	}
	for (index = 0; index < 2U; index++) {
		(void)fcntl(server->wake[index], F_SETFL, O_NONBLOCK);
		(void)fcntl(server->wake[index], F_SETFD, FD_CLOEXEC);
	}

	/* Succeeded: the threads can wake the main thread. */
	return 0;
}

/*
 * Closes the wake pipe once no thread writes to it any more (the last
 * step of the compositor's cleanup).
 */
void
kwl_wake_close(
	struct kwl_server *server)
{
	unsigned index;

	/* Each end that is open. */
	for (index = 0; index < 2U; index++) {
		if (server->wake[index] >= 0)
			close(server->wake[index]);
		server->wake[index] = -1;
	}
}

/*
 * Makes sure of the wake pipe and makes the cursor's box (ws213-p004),
 * before the output and its cursor plane open: the cursor thread reads the
 * box from the plane's opening on.
 */
int
kwl_threads_prepare(
	struct kwl_server *server)
{
	int error;

	/* Made once. */
	if (server->threads_prepared)
		return 0;

	/* The wake pipe, unless made before the backend. */
	error = kwl_wake_open(server);
	if (error != 0)
		return error;

	/* The box, its place given when the input thread starts. */
	error = kwl_cursor_box_init(&server->cursor_box, server->pointer_x, server->pointer_y, server->width, server->height);
	if (error != 0)
		return error;

	/* Succeeded: the threads can share the box. */
	server->threads_prepared = 1U;
	return 0;
}

/*
 * Starts the input thread with the pointer where it starts in the
 * cursor's box (ws213-p004).
 */
int
kwl_input_threads_start(
	struct kwl_server *server)
{
	struct kwl_input_settings settings;
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	struct kwl_cursor_motion place;
	int error;

	/* The wake pipe and the box, unless made before the output. */
	error = kwl_threads_prepare(server);
	if (error != 0)
		return error;

	/* The pointer's first place, and the outputs, in the box. */
	(void)kwl_outputs(server, outputs);
	place.x = server->pointer_x;
	place.y = server->pointer_y;
	place.output = server->pointer_output;
	kwl_pointer_warp(server, outputs, pointer_warp_place, &place);

	/* The input thread with the user's settings, reading through the backend. */
	input_settings(server, &settings);
	error = kwl_input_thread_init(&server->input_thread, &server->cursor_box, server->wake[1], kl_backend_input_read, &settings);
	if (error != 0)
		return error;

	/* The settings the input thread has, compared at each pass. */
	server->input_settings_given = settings;

	/* The thread runs. */
	error = kwl_input_thread_start(&server->input_thread);
	if (error != 0) {
		kwl_input_thread_destroy(&server->input_thread);
		return error;
	}

	/* No answer is owed the seat yet. */
	kwl_seat_answers_init(&server->seat_answers);

	/* Succeeded: the devices opened from now on are read by the input thread. */
	server->input_thread_ready = 1U;
	return 0;
}

/*
 * Stops the input thread, which then uses no descriptor, and frees what it
 * leaves, the box (the devices stay open for kwl_input_cleanup, the wake
 * pipe for kwl_wake_close).  The cursor thread has stopped with the output.
 */
void
kwl_input_threads_stop(
	struct kwl_server *server)
{
	unsigned index;

	/* The thread ends, and no device is attached any more. */
	if (server->input_thread_ready) {
		kwl_input_thread_stop(&server->input_thread);
		kwl_input_thread_destroy(&server->input_thread);
		for (index = 0; index < KWL_INPUT_MAX; index++)
			server->inputs[index].attached = 0U;
		server->input_thread_ready = 0U;
	}

	/* Nothing else was made. */
	if (!server->threads_prepared)
		return;

	/* The box. */
	kwl_cursor_box_destroy(&server->cursor_box);

	/* Nothing is left to share. */
	server->threads_prepared = 0U;
}

/*
 * Applies the input thread's events in the order it made them: the
 * clients' pointer and keyboard events, the hover, the shell's gestures,
 * the devices that ended.  The main thread empties its wake pipe before.
 */
void
kwl_input_drain(
	struct kwl_server *server)
{
	struct kwl_input_event *events;
	struct kwl_input_event *event;
	unsigned dropped;
	int failed;

	/* Nothing runs before the thread starts. */
	if (!server->input_thread_ready)
		return;

	/* Events lost for want of memory, and a thread that stopped. */
	failed = kwl_input_thread_failure(&server->input_thread, &dropped);
	if (dropped != 0U)
		printf("KWL INPUT dropped=%u\n", dropped);
	if (failed != 0) {
		kwl_fatal("KWL FAILED site=input-thread errno=%d", failed);
		server->failed = 1U;
	}

	/* Each event in order, freed once applied. */
	events = kwl_input_thread_take(&server->input_thread);
	while (events != NULL) {
		event = events;
		events = event->next;
		apply_event(server, event);
		free(event);
	}
}

/*
 * Gives the input thread the settings and the outputs' rectangles when
 * they changed since it was last given them (once a pass, before the poll).
 */
void
kwl_input_sync(
	struct kwl_server *server)
{
	struct kwl_input_settings settings;
	int same;

	/* Nothing runs before the thread starts. */
	if (!server->input_thread_ready)
		return;

	/* The settings, when the user changed them. */
	input_settings(server, &settings);
	same = memcmp(&settings, &server->input_settings_given, sizeof(settings));
	if (same != 0) {
		kwl_input_thread_settings(&server->input_thread, &settings);
		server->input_settings_given = settings;
	}

	/* The geometry the motions are computed over. */
	kwl_input_geometry(server);
}

/*
 * Gives the input thread the geometry the pointer's place is computed over
 * as the main thread has it now (WS217: the outputs' rectangles, panels and
 * angles), at once.
 */
void
kwl_input_geometry(
	struct kwl_server *server)
{
	struct kwl_cursor_geometry geometry;

	/* Nothing runs before the thread starts. */
	if (!server->input_thread_ready)
		return;

	/*
	 * The geometry as it is, given whole; another panel or angle makes the
	 * motions computed before it no place of the pointer (review M7), and
	 * the main thread takes the box's new warp count.
	 */
	kwl_output_geometry(server, &geometry);
	kwl_cursor_box_outputs(&server->cursor_box, &geometry, &server->pointer_warp);
}

/*
 * Ends what the absolute devices were doing when the angle an output shows
 * changed (WS217, plan/ws217/phase001 4.6 A 5): a drag and drop is
 * given up, the pointer's grabs end, every finger on a touch screen is
 * cancelled until it lifts, and every pen lifts and leaves its surface (a pen still near comes back at its next report, where the new
 * angle maps it).  The pointer and the geometry follow the turn's warp.
 */
void
kwl_input_rotated(
	struct kwl_server *server)
{
	/*
	 * A drag and drop is given up first (a button let go below would drop
	 * it where the old angle put it), then the pointer's grabs end, then
	 * the fingers and the pens.
	 */
	kwl_data_drag_cancel(server);
	kwl_seat_grabs_end(server);
	kwl_touch_cancel(server, "rotated");
	kwl_tablet_cancel(server);
}

/*
 * Stops reading one input the seat paused (libkeiland-backend's
 * input_paused); its descriptor stays the seat's, and its record stays for
 * the resume.  Returns 1 when the seat is to wait for kwl_input_answer's
 * answer.
 */
int
kwl_input_pause(
	struct kwl_server *server,
	struct kwl_input_device *device)
{
	int later;

	/* The input thread lets go before the seat revokes the descriptor. */
	later = input_answer_later(server, device);
	device->fd = -1;
	return later;
}

/*
 * Reads one input again on the new descriptor the seat gave
 * (libkeiland-backend's input_resumed); a report of the old one does not
 * enter it.  Returns 1 when the seat is to wait for kwl_input_answer's
 * answer before it closes the old one.
 */
int
kwl_input_resume(
	struct kwl_server *server,
	struct kwl_input_device *device,
	int descriptor)
{
	int later;

	/* The old descriptor, should the input thread still read it, is let go first. */
	later = input_answer_later(server, device);

	/* The new one, read afresh. */
	device->fd = descriptor;
	device->seat_fd = descriptor;
	input_attach(server, device);
	return later;
}

/*
 * Stops reading every input (the seat paused the session): each attached
 * device's detach is asked without waiting.  Returns the last one's
 * number, whose acknowledgement comes after every earlier one (0 when none
 * was asked).
 */
uint64_t
kwl_input_detach_all(
	struct kwl_server *server)
{
	uint64_t last;
	unsigned index;

	/* Nothing runs before the thread starts. */
	if (!server->input_thread_ready)
		return 0U;

	/* Each attached device's detach. */
	last = 0U;
	for (index = 0; index < KWL_INPUT_MAX; index++) {
		if (!server->inputs[index].attached)
			continue;
		input_detach(server, &server->inputs[index]);
		last = server->inputs[index].detach_sequence;
	}

	/* Succeeded: the last asked. */
	return last;
}

/*
 * Attaches again every input the session's pause detached whose
 * descriptor is still open (the seat gave no input of its own back).
 */
void
kwl_input_attach_all(
	struct kwl_server *server)
{
	struct kwl_input_device *device;
	unsigned index;

	/* Each live device that has a descriptor and is not read. */
	for (index = 0; index < KWL_INPUT_MAX; index++) {
		device = &server->inputs[index];
		if (!device->live || device->closing || device->attached || device->fd < 0)
			continue;
		input_attach(server, device);
	}
}

/*
 * Answers the seat (kl_backend_seat_device_let_go) for every descriptor
 * whose detach the input thread has acknowledged since the last pass, in
 * the order its callbacks came; and ends the compositor when an
 * acknowledgement has been awaited past KWL_INPUT_DETACH_WAIT_MS (the
 * input thread does not let go of a descriptor that may become another
 * file's).  Called once a pass, after kwl_input_drain.
 */
void
kwl_input_answer(
	struct kwl_server *server)
{
	uint64_t acked;
	uint64_t now;
	uint64_t waited;
	int descriptor;
	int late;
	int due;

	/* Nothing is asked before the thread starts. */
	if (!server->input_thread_ready)
		return;

	/* What the input thread has acknowledged. */
	now = kwl_milliseconds();
	acked = kwl_input_thread_acked(&server->input_thread);
	kwl_seat_answers_heard(&server->seat_answers, acked, now);

	/* Each answer due, oldest first. */
	for (;;) {
		due = kwl_seat_answers_next(&server->seat_answers, &descriptor);
		if (!due)
			break;
		kl_backend_seat_device_let_go(server->backend, descriptor);
	}

	/* An input thread that does not answer leaves nothing safe. */
	late = kwl_seat_answers_late(&server->seat_answers, now, KWL_INPUT_DETACH_WAIT_MS, &waited);
	if (late)
		input_detach_failed("input-detach", server, NULL, server->seat_answers.count, server->seat_answers.asked, now - waited, ETIMEDOUT);
}

/*
 * Tells whether the input thread has acknowledged the detach numbered
 * sequence and every one before it (as kwl_input_answer last looked).
 */
int
kwl_input_detached_through(
	struct kwl_server *server,
	uint64_t sequence)
{
	int pending;

	/* Without the thread nothing reads. */
	if (!server->input_thread_ready)
		return 1;

	/* Acknowledged, or not yet. */
	pending = kwl_seat_answers_pending(&server->seat_answers, sequence);
	if (pending)
		return 0;
	return 1;
}

/*
 * Moves the pointer for the main thread (a warp, design 6.1): the move
 * computes the new place from the cursor's box under its lock, the
 * outputs' rectangles (when given) change in the same section, and the
 * main thread's view becomes the new place.  The input thread's motions
 * computed before are not taken as the pointer's place after it.
 */
void
kwl_pointer_warp(
	struct kwl_server *server,
	const struct kwl_plane_rect *outputs,
	kwl_cursor_box_move_fn *move,
	const void *context)
{
	struct kwl_cursor_geometry geometry;
	struct kwl_cursor_motion motion;
	int32_t x;
	int32_t y;
	unsigned output;

	/* Without the box the main thread's view is the place itself. */
	if (!server->threads_prepared) {
		x = server->pointer_x;
		y = server->pointer_y;
		output = server->pointer_output;
		move(context, outputs, &x, &y, &output);
		server->pointer_x = x;
		server->pointer_y = y;
		server->pointer_output = output;
		return;
	}

	/*
	 * The box moves, with the rectangles given and the panels and angles
	 * as they are now (WS217); the main thread's view and warp count
	 * follow it.
	 */
	if (outputs != NULL) {
		kwl_output_geometry(server, &geometry);
		memcpy(geometry.outputs, outputs, sizeof(geometry.outputs));
		geometry.output_count = KWL_PLANE_SLOTS;
		kwl_cursor_box_warp(&server->cursor_box, &geometry, move, context, &motion);
	} else {
		kwl_cursor_box_warp(&server->cursor_box, NULL, move, context, &motion);
	}

	/* The main thread's view of the place. */
	server->pointer_x = motion.x;
	server->pointer_y = motion.y;
	server->pointer_output = motion.output;
	server->pointer_warp = motion.warp;
}

/*
 * Places the pointer on the anchor where an absolute device (a pen tablet,
 * a touch screen) put it, as a warp.
 */
void
kwl_pointer_warp_to(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	struct kwl_cursor_motion place;

	/* The place, on the anchor. */
	place.x = x;
	place.y = y;
	place.output = KWL_PLANE_ANCHOR;

	/* Written as a warp. */
	kwl_pointer_warp(server, NULL, pointer_warp_place, &place);
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

/*
 * Carries out the actions of the touch pad layer on the seat, as a mouse's
 * report would; its motions are already in the cursor's box, and the main
 * thread's view of the pointer follows them.
 */
static void
apply_touchpad_actions(
	struct kwl_server *server,
	const struct kwl_input_pad *pad,
	uint32_t time)
{
	const struct kwl_touchpad_actions *actions;
	const struct kwl_touchpad_action *action;
	int64_t down_um;
	unsigned activity;
	unsigned index;
	int moved;
	int taken;

	/* Each action in order. */
	actions = &pad->actions;
	activity = 0;
	for (index = 0; index < actions->count; index++) {
		action = &actions->actions[index];

		/* Its kind. */
		switch (action->kind) {
		case KWL_TOUCHPAD_MOTION:
			/* The fingers moved the pointer: its arrow is shown from now on (ws035-p116). */
			if (server->pointer_unmoved && pad->nonzero[index]) {
				server->pointer_unmoved = 0U;
				kwl_damage_pointer(server, server->pointer_x, server->pointer_y);
				printf("KWL CURSOR shown by=touchpad\n");
			}

			/* The pointer is where the input thread moved it, at the user's speed. */
			box_measure(server, &pad->motions[index]);
			moved = apply_motion(server, &pad->motions[index], 0U, time);
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
 * Gives the input thread the touch pads' acceleration and scrolling
 * direction again, after the settings changed them (ws089-p024).
 */
void
kwl_input_touchpads_changed(
	struct kwl_server *server)
{
	/* The settings go with the rest. */
	kwl_input_sync(server);
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
	device->seat_fd = descriptor;
	device->pointer = 1;
	device->touchpad = 1;
	snprintf(device->path, sizeof(device->path), "%s", path);

	/* The input thread starts the layer with the fingers' resolution and range. */
	device->reading.resolution_x = x.resolution;
	device->reading.resolution_y = y.resolution;
	device->reading.pad_x_minimum = x.minimum;
	device->reading.pad_x_maximum = x.maximum;
	device->reading.pad_y_minimum = y.minimum;
	device->reading.pad_y_maximum = y.maximum;

	/* The slot is published only when it is completely filled in, and the input thread reads it. */
	device->live = 1;
	input_attach(server, device);

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
	const char *path,
	unsigned direct)
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
	device->seat_fd = descriptor;
	device->tablet = 1;
	device->direct = direct;
	snprintf(device->path, sizeof(device->path), "%s", path);

	/* The tablet reads the axes and tells the bound tablet seats (tablet.c). */
	error = kwl_tablet_add(server, device);
	if (error != 0) {
		device->fd = -1;
		device->tablet = 0;
		return error;
	}

	/* The slot is published only when it is completely filled in, and the input thread reads it. */
	device->live = 1;
	input_attach(server, device);

	/* One line lets a test see which devices the seat uses. */
	printf("KWL INPUT device=%s kind=tablet abs=1 direct=%u\n", device->path, device->direct);

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
	device->seat_fd = descriptor;
	device->touch = 1;
	device->direct = 1U;
	snprintf(device->path, sizeof(device->path), "%s", path);

	/* The touch screen reads its range (touch.c). */
	error = kwl_touch_add(device);
	if (error != 0) {
		device->fd = -1;
		device->touch = 0;
		return error;
	}

	/* The slot is published only when it is completely filled in, and the input thread reads it. */
	device->live = 1;
	input_attach(server, device);

	/* One line lets a test see which devices the seat uses. */
	printf("KWL INPUT device=%s kind=touch abs=1\n", device->path);

	/* Bound seats learn that a touch screen (and the pointer of its fallback) is there. */
	update_capabilities(server);

	/* Succeeded: the event loop now reads this device. */
	return 0;
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

/* Applies one event of the input thread to its device. */
static void
apply_event(
	struct kwl_server *server,
	const struct kwl_input_event *event)
{
	struct kwl_input_device *device;

	/* A device no longer in the table has nothing to apply to. */
	if (event->slot >= KWL_INPUT_MAX)
		return;
	device = &server->inputs[event->slot];
	if (!device->live)
		return;

	/*
	 * The last input, which a layer's picture drawn in the quiet waits on
	 * (layer-cache.c), and the exit summary's count of evdev events.
	 */
	if (event->read_ms > server->input_ms)
		server->input_ms = event->read_ms;
	server->input_events += event->counted;

	/* By what the event carries. */
	switch (event->kind) {
	case KWL_INPUT_EVENT_FRAME:
		apply_frame(server, device, event);
		break;
	case KWL_INPUT_EVENT_TOUCHPAD:
		/* The latest report's kernel time, for the cursor plane's log line of a move the fingers make (ws213-p001 review-2). */
		if (event->frame_time_us != 0U)
			server->pointer_event_us = event->frame_time_us;
		apply_touchpad_actions(server, &event->u.pad, event->time);
		break;
	case KWL_INPUT_EVENT_REPORT:
		apply_report(server, device, event);
		break;
	case KWL_INPUT_EVENT_DETACHED:
		apply_detached(server, device, &event->u.detached);
		break;
	default:
		break;
	}
}

/*
 * Applies a mouse's or keyboard's report: the pointer's motion (already in
 * the box), then buttons and keys in the order the device reported them,
 * then scrolling, then a pointer frame.
 */
static void
apply_frame(
	struct kwl_server *server,
	struct kwl_input_device *device,
	const struct kwl_input_event *event)
{
	const struct kwl_input_frame *frame;
	const struct kwl_input_key *key;
	int32_t wheel;
	int32_t horizontal_wheel;
	unsigned pointer_activity;
	unsigned index;
	int moved;

	/*
	 * The device placed or moved the pointer, even onto the place it
	 * already had: its arrow is shown from now on, drawn where it is
	 * (ws035-p116).
	 */
	frame = &event->u.frame;
	if (server->pointer_unmoved && frame->placed) {
		server->pointer_unmoved = 0U;
		kwl_damage_pointer(server, server->pointer_x, server->pointer_y);
		printf("KWL CURSOR shown by=pointer device=%s\n", device->path);
	}

	/* The report's kernel time, for the cursor plane's log line (ws213-p001 review-1 C1), and the box's lag. */
	if (frame->placed) {
		server->pointer_event_us = event->frame_time_us;
		box_measure(server, &frame->motion);
	}

	/* A changed position is reported as motion before any button of the same report. */
	pointer_activity = 0;
	moved = apply_motion(server, &frame->motion, frame->absolute, event->time);
	if (moved)
		pointer_activity = 1;

	/* Buttons and keys follow in the order the device reported them. */
	for (index = 0; index < frame->key_count; index++) {
		key = &frame->keys[index];

		/* A pointer's mouse buttons become wl_pointer buttons with their BTN_ code. */
		if (device->pointer &&
		    key->code >= INPUT_BUTTON_FIRST &&
		    key->code <= INPUT_BUTTON_LAST) {
			kwl_seat_button(server, event->time, key->code, (uint32_t)key->value);
			pointer_activity = 1;
			continue;
		}

		/* The rest of the button block is neither a mouse button nor a key. */
		if (key->code >= INPUT_BUTTON_BLOCK_FIRST && key->code < INPUT_BUTTON_BLOCK_END)
			continue;

		/* A keyboard's keys become wl_keyboard keys with their evdev code. */
		if (device->keyboard)
			apply_key(server, event->time, key->code, key->value);
	}

	/* The user's natural scrolling for a mouse (ws089-p007, p024) turns the wheel round. */
	wheel = frame->wheel;
	horizontal_wheel = frame->horizontal_wheel;
	if (server->mouse_natural != 0) {
		wheel = -wheel;
		horizontal_wheel = -horizontal_wheel;
	}

	/* Wheel notches scroll; evdev counts up as positive, Wayland counts down as positive. */
	if (wheel != 0 || horizontal_wheel != 0) {
		kwl_seat_axis(server, event->time, -wheel, horizontal_wheel);
		pointer_activity = 1;
	}

	/* Version 5 pointers are told where this report's events end. */
	if (pointer_activity)
		kwl_seat_frame(server);
}

/* Applies a pen tablet's or touch screen's report through its own module (tablet.c, touch.c). */
static void
apply_report(
	struct kwl_server *server,
	struct kwl_input_device *device,
	const struct kwl_input_event *event)
{
	/* The report as the module reads it from the device's record. */
	device->frame_count = event->u.report.count;
	memcpy(device->frame, event->u.report.events, event->u.report.count * sizeof(device->frame[0]));
	device->frame_time_us = event->frame_time_us;

	/* Applied as one group by its module. */
	if (device->tablet) {
		kwl_tablet_frame(server, device, event->time);
	} else if (device->touch) {
		kwl_touch_frame(server, device, event->time);
	}

	/* The next report starts empty. */
	device->frame_count = 0;
}

/*
 * Applies a device's last event: a device the main thread let go of goes
 * now if it was closing; one that ended is closed; one the seat revoked
 * waits for the seat when logind keeps it, and is closed otherwise.
 */
static void
apply_detached(
	struct kwl_server *server,
	struct kwl_input_device *device,
	const struct kwl_input_detached *detached)
{
	int retained;

	/*
	 * The main thread asked: a forgotten device's record goes once its
	 * last detach is acknowledged (an earlier one's event may come first,
	 * of a pause before a resume), a paused one stays.
	 */
	if (detached->reason == KWL_INPUT_DETACH_ASKED) {
		if (device->closing && detached->sequence >= device->detach_sequence)
			input_retire(server, device);
		return;
	}

	/* The input thread let go by itself; a newer attach of the same device is not read either. */
	device->attached = 0U;

	/* A revoked device the seat keeps waits for the seat (ws105-p009, ws131-p007). */
	if (detached->reason == KWL_INPUT_DETACH_REVOKED && device->fd >= 0) {
		retained = kl_backend_seat_device_revoked(server->backend, device->fd);
		if (retained != 0) {
			printf("KWL SEAT input_revoked path=%s lease=retained\n", device->path);
			device->fd = -1;
			return;
		}
	}

	/* Any other end closes the device. */
	printf("KWL INPUT_CLOSED device=%s kind=%s reason=%s errno=%d\n", device->path, input_kind_name(device), input_reason_name(detached->reason), detached->error);
	if (device->fd >= 0)
		kwl_io_input_close(server, device->fd);
	input_retire(server, device);
}

/*
 * Moves the main thread's view of the pointer to where a motion of the
 * input thread left it, and tells the seat: unless the motion was computed
 * before a later warp, or left the pointer where the view has it.  Reports
 * whether the view moved.
 */
static int
apply_motion(
	struct kwl_server *server,
	const struct kwl_cursor_motion *motion,
	unsigned absolute,
	uint32_t time)
{
	int32_t old_x;
	int32_t old_y;

	/* No write of the box (a report that did not move the pointer). */
	if (motion->write_us == 0U)
		return 0;

	/* A motion computed before the main thread's last warp is not the pointer's place. */
	if (motion->warp != server->pointer_warp)
		return 0;

	/* The view already has this place. */
	if (motion->x == server->pointer_x &&
	    motion->y == server->pointer_y &&
	    motion->output == server->pointer_output)
		return 0;

	/* A crossing to another output is logged once, and the outputs are drawn again (ws113-p007). */
	if (motion->output != server->pointer_output) {
		if (absolute) {
			printf("KWL POINTER output=%u absolute\n", motion->output);
		} else {
			printf("KWL POINTER output=%u x=%d y=%d\n", motion->output, motion->x, motion->y);
		}

		/* Every output is drawn with the cursor where it is now. */
		server->dirty = 1;
	}

	/* The view moves; the seat hears it, and window mode draws the cursor at its new place (and where it was, damage.c). */
	old_x = server->pointer_x;
	old_y = server->pointer_y;
	server->pointer_x = motion->x;
	server->pointer_y = motion->y;
	server->pointer_output = motion->output;
	kwl_seat_motion(server, time);
	kwl_damage_pointer(server, old_x, old_y);

	/* Succeeded: the view moved. */
	return 1;
}

/*
 * Counts one write of the cursor's box for KWL PERF cursor (design 6.4):
 * its lag from the report's evdev time to the write, the lag beyond the
 * reports' own spacing, and the time since the previous write while the
 * pointer moves.  A lag over a refresh is logged (at most 20 lines a
 * window) with where the input thread spent it (when its poll began and
 * returned, when the read returned), for comparison with the kernel's
 * hold_us lines.
 */
static void
box_measure(
	struct kwl_server *server,
	const struct kwl_cursor_motion *motion)
{
	struct kwl_perf *perf;
	uint64_t lag;
	uint64_t write_gap;
	uint64_t event_gap;
	uint64_t excess;

	/* Only a write that came from a report has both ends. */
	if (motion->write_us == 0U || motion->event_us == 0U)
		return;

	/* The lag, never negative. */
	lag = 0U;
	if (motion->write_us > motion->event_us)
		lag = motion->write_us - motion->event_us;

	/* The write and the report since the previous write; the excess is the first beyond the second. */
	excess = 0U;
	write_gap = 0U;
	event_gap = UINT64_MAX;
	if (server->box_last_write_us != 0U && motion->write_us >= server->box_last_write_us && motion->event_us >= server->box_last_event_us) {
		write_gap = motion->write_us - server->box_last_write_us;
		event_gap = motion->event_us - server->box_last_event_us;
		if (write_gap > event_gap)
			excess = write_gap - event_gap;
	}

	/* This write is the previous one for the next. */
	server->box_last_write_us = motion->write_us;
	server->box_last_event_us = motion->event_us;

	/* The window keeps it while it has room. */
	perf = &server->perf;
	if (perf->box_count >= KWL_PERF_BOX) {
		perf->box_dropped++;
		return;
	}

	/* The lag and the excess, kept in 32 bits (over an hour is kept as the most). */
	if (lag > 0xffffffffU)
		lag = 0xffffffffU;
	if (excess > 0xffffffffU)
		excess = 0xffffffffU;
	perf->box_lag_us[perf->box_count] = (uint32_t)lag;
	perf->box_excess_us[perf->box_count] = (uint32_t)excess;
	perf->box_count++;

	/* The gap only between reports at most 50 ms apart: the pointer moves. */
	if (event_gap <= 50000U && write_gap <= 0xffffffffU) {
		perf->box_gap_us[perf->box_gap_count] = (uint32_t)write_gap;
		perf->box_gap_count++;
	}

	/* A lag over a refresh, logged for its cause. */
	if (lag <= 16700U)
		return;
	perf->box_over++;
	if (perf->box_lag_lines >= 20U)
		return;
	perf->box_lag_lines++;
	printf("KWL CURSOR_LAG event_us=%llu write_us=%llu lag_us=%llu poll_entered_us=%llu poll_returned_us=%llu read_us=%llu\n",
	       (unsigned long long)motion->event_us,
	       (unsigned long long)motion->write_us,
	       (unsigned long long)lag,
	       (unsigned long long)motion->poll_entered_us,
	       (unsigned long long)motion->poll_returned_us,
	       (unsigned long long)motion->read_us);
}

/* Hands a live device's descriptor to the input thread, with what it needs to read it. */
static void
input_attach(
	struct kwl_server *server,
	struct kwl_input_device *device)
{
	struct kwl_input_attach *reading;
	int error;

	/* Nothing runs before the thread starts (the seat's first scan comes after). */
	if (!server->input_thread_ready || device->fd < 0)
		return;

	/* The device's kind and ranges, in its slot. */
	reading = &device->reading;
	reading->slot = (unsigned)(device - server->inputs);
	reading->fd = device->fd;
	reading->pointer = device->pointer;
	reading->keyboard = device->keyboard;
	reading->absolute = device->absolute;
	reading->tablet = device->tablet;
	reading->touch = device->touch;
	reading->touchpad = device->touchpad;
	reading->abs_x_minimum = device->abs_x_minimum;
	reading->abs_x_maximum = device->abs_x_maximum;
	reading->abs_y_minimum = device->abs_y_minimum;
	reading->abs_y_maximum = device->abs_y_maximum;

	/* The input thread reads it from its next pass. */
	error = kwl_input_thread_attach(&server->input_thread, reading);
	if (error != 0) {
		printf("KWL INPUT attach device=%s errno=%d\n", device->path, error);
		return;
	}

	/* Succeeded: the descriptor is the input thread's to read until a detach. */
	device->attached = 1U;
}

/*
 * Asks the input thread to stop reading a device, without waiting: its
 * acknowledgement comes after the device's last event (apply_detached),
 * and the detach's number is kept for the answers it makes due.  The
 * compositor ends if the input thread cannot be asked, as a descriptor
 * number it might still read could be another file's after a close.
 */
static void
input_detach(
	struct kwl_server *server,
	struct kwl_input_device *device)
{
	uint64_t sequence;
	uint64_t asked_ms;
	int error;

	/* A device not read by the thread needs nothing. */
	if (!device->attached)
		return;
	device->attached = 0U;
	if (!server->input_thread_ready)
		return;

	/* The detach, numbered (the failure's line says when it was asked). */
	asked_ms = kwl_milliseconds();
	error = kwl_input_thread_detach(&server->input_thread, (unsigned)(device - server->inputs), &sequence);
	if (error != 0)
		input_detach_failed("input-detach", server, device, 1U, 0U, asked_ms, error);

	/* Its acknowledgement is awaited from now (kwl_input_answer). */
	device->detach_sequence = sequence;
	kwl_seat_answers_asked(&server->seat_answers, sequence, asked_ms);
}

/*
 * Lets the input thread go of the device's descriptor the seat gave
 * (seat_fd) for a callback of the seat, and tells whether the seat is to
 * wait for the answer (1: kwl_input_answer calls
 * kl_backend_seat_device_let_go once the detach is acknowledged) or may
 * go on now (0: nothing reads it).  Without room for the answer the
 * acknowledgement is waited for here.
 */
static int
input_answer_later(
	struct kwl_server *server,
	struct kwl_input_device *device)
{
	uint64_t asked_ms;
	int pending;
	int error;

	/* The input thread stops reading it, if it does. */
	input_detach(server, device);

	/* Without a descriptor of the seat's, or without the thread, nothing reads it. */
	if (device->seat_fd < 0 || !server->input_thread_ready)
		return 0;

	/* A detach acknowledged already (as last looked) leaves nothing to wait for. */
	pending = kwl_seat_answers_pending(&server->seat_answers, device->detach_sequence);
	if (!pending)
		return 0;

	/* The answer owed, after every one before it. */
	error = kwl_seat_answers_add(&server->seat_answers, device->detach_sequence, device->seat_fd);
	if (error == 0)
		return 1;

	/* No room: the acknowledgement here, within the limit (the failure's line says how long it waited). */
	asked_ms = kwl_milliseconds();
	error = kwl_input_thread_detach_wait(&server->input_thread, device->detach_sequence, KWL_INPUT_DETACH_WAIT_MS);
	if (error != 0)
		input_detach_failed("input-answer", server, device, 1U, device->detach_sequence, asked_ms, error);

	/* Succeeded: the seat may go on now. */
	return 0;
}

/*
 * Ends the compositor at once when a device cannot be taken back from the
 * input thread (design 6.1 N-1), saying which device (NULL: every attached
 * one, count of them), the detach's number and how long it was waited for.
 */
static void
input_detach_failed(
	const char *site,
	const struct kwl_server *server,
	const struct kwl_input_device *device,
	unsigned count,
	uint64_t sequence,
	uint64_t asked_ms,
	int error)
{
	const char *path;
	const char *kind;
	unsigned slot;

	/* The device, or all of them. */
	path = "all";
	kind = "all";
	slot = KWL_INPUT_MAX;
	if (device != NULL) {
		path = device->path;
		kind = input_kind_name(device);
		slot = (unsigned)(device - server->inputs);
	}

	/* No cleanup: another thread may be anywhere, and the descriptor must not be closed under it. */
	kwl_fatal("KWL FAILED site=%s device=%s kind=%s slot=%u devices=%u sequence=%llu waited_ms=%llu errno=%d", site, path, kind, slot, count, (unsigned long long)sequence, (unsigned long long)(kwl_milliseconds() - asked_ms), error);
	_exit(1);
}

/* Takes a device out of the seat: its tablet's tools or touch screen's fingers end, and the slot is free. */
static void
input_retire(
	struct kwl_server *server,
	struct kwl_input_device *device)
{
	/* A pen tablet's clients hear that it and its tools are gone (tablet.c). */
	if (device->tablet)
		kwl_tablet_remove(server, device, 1);

	/* A touch screen's fingers end (touch.c). */
	if (device->touch)
		kwl_touch_remove(server, device, 1);

	/* The slot is free. */
	device->fd = -1;
	device->live = 0;
	device->closing = 0U;
	device->attached = 0U;

	/* Bound seats learn that a class of device may be gone. */
	update_capabilities(server);
}

/*
 * Gives the settings the input thread applies, as the user has them now,
 * and whether the touch pads make two fingers moving down a gesture: while
 * App Home shows, outside the login and lock screens, its swipe down
 * closes it (ws181-p012).
 */
static void
input_settings(
	struct kwl_server *server,
	struct kwl_input_settings *settings)
{
	float home;

	/* Every field, so that two copies compare whole. */
	memset(settings, 0, sizeof(*settings));
	settings->mouse_speed = server->mouse_speed;
	settings->mouse_acceleration = server->mouse_acceleration;
	settings->touchpad_speed = server->touchpad_speed;
	settings->touchpad_acceleration = server->touchpad_acceleration;
	settings->touchpad_natural = server->touchpad_natural;

	/* App Home showing, or on its way to. */
	home = kwl_home_progress(server);
	if ((home > 0.0f || server->home_to > 0.0f) && !server->greeter && !server->locked)
		settings->touchpad_down = 1;
}

/* Gives the place an absolute device put the pointer at (a warp's move). */
static void
pointer_warp_place(
	const void *context,
	const struct kwl_plane_rect *outputs,
	int32_t *x,
	int32_t *y,
	unsigned *output)
{
	const struct kwl_cursor_motion *place;

	/* The place given, whatever the box had. */
	(void)outputs;
	place = context;
	*x = place->x;
	*y = place->y;
	*output = place->output;
}

/* Names a device's kind for a log line. */
static const char *
input_kind_name(
	const struct kwl_input_device *device)
{
	/* The kinds read by their own module first. */
	if (device->tablet)
		return "tablet";
	if (device->touch)
		return "touch";
	if (device->touchpad)
		return "touchpad";

	/* A pointer, a keyboard, or both in one node. */
	if (device->pointer && device->keyboard)
		return "pointer+keyboard";
	if (device->pointer && device->absolute)
		return "absolute";
	if (device->pointer)
		return "pointer";
	if (device->keyboard)
		return "keyboard";

	/* A record without a kind (not expected of a live one). */
	return "none";
}

/* Names why the input thread let a device go, for a log line. */
static const char *
input_reason_name(
	enum kwl_input_detach_reason reason)
{
	/* By the reason the device's last event carries. */
	switch (reason) {
	case KWL_INPUT_DETACH_ASKED:
		return "asked";
	case KWL_INPUT_DETACH_ENDED:
		return "ended";
	case KWL_INPUT_DETACH_REVOKED:
		return "revoked";
	default:
		break;
	}

	/* A reason this file does not know. */
	return "other";
}
