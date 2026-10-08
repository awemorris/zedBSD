/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Touch screens: wl_touch for the clients and the compositor's own touch
 * gestures (WS079 p013, plan/ws079/phase013/phase.md).
 *
 * An evdev node with ABS_MT_SLOT, ABS_MT_TRACKING_ID and both
 * ABS_MT_POSITION axes speaks multitouch protocol B (the kernel's USB touch
 * screen and the test injector, ws079-p012) and is a touch screen (input.c).
 * Its whole area maps onto the whole output.  Each report is applied as one
 * group: the fingers that lifted end first, then the fingers that moved
 * move, then the new fingers touch, and every client that heard something
 * hears wl_touch.frame.
 *
 * The compositor sees every finger before a client does.  Where a new
 * finger goes is decided when it touches and holds until it lifts:
 *
 * - A finger on a floating title bar waits TITLE_PAIR_MS for a second one
 *   on the same title bar.  Two fingers there that flick up quickly
 *   ("go away", the 2026-09-28 user's words: both move up at least
 *   TITLE_FLICK_DISTANCE, more up than sideways, and the first lifts within
 *   TITLE_FLICK_MS of the second's touch) send the window to the back, as a
 *   triple click does (kwl_glass_lower).  Anything else of the two fingers
 *   (slow, down, sideways, too short) does nothing.  A single finger that
 *   lifts in time is a tap (a click: the window comes forward); one that
 *   stays longer drags the title bar like the mouse.
 * - Any other first finger is the pointer's left button for the shell
 *   (seat.c's _shell functions), with server->shell_source saying it is a
 *   finger: the edges' gestures (App Home, Notes, Wiseview, Home's bottom
 *   edge), the system bar, the title bars' buttons, Wiseview and App Home
 *   themselves take it as they take the mouse.
 * - What the shell leaves goes to the surface under the finger (the shell
 *   has brought its window forward): by wl_touch to a client that has one,
 *   otherwise as the pointer's left button, the finger moving the pointer.
 * - While a finger has the pointer or the title bar, another finger goes
 *   only to a client with wl_touch under it, or nowhere.
 * - A finger on the open on-screen keyboard's panel is the keyboard's own
 *   press (ROUTE_OSK, ws102-p009), whatever the other fingers do: each
 *   finger presses its key, and a second finger touching while the first
 *   holds a key makes the first key act (two thumbs typing, keyboard.c).
 *
 * Once the compositor takes a finger for itself, every finger a client was
 * hearing by wl_touch is cancelled (wl_touch.cancel), and the client hears
 * nothing more of those fingers.
 *
 * ws081-p014: a client's finger starts a drag and drop
 * (wl_data_device.start_drag with its wl_touch.down's serial, data.c,
 * kwl_touch_drag_start).  The finger then drives the drag as the pointer
 * would: each report moves the pointer to it (the drag's icon and target
 * follow), its lift drops, and a screen that goes cancels the drag.  The
 * client hears wl_touch.cancel (the finger is the drag's now), and its
 * other fingers go nowhere until they lift.
 *
 * WS081 (plan/ws081/design.md section 4): a report's time is when the panel
 * scanned it, from its Scan Time (MSC_TIMESTAMP) mapped onto the host clock
 * by libkeiland's touch motion, or when it arrived for a panel without one;
 * the clients' wl_touch times are that time's milliseconds.  Every finger's
 * reports go into a touch motion, which teaches the screen's device its
 * period, delay and noise.  A finger the shell has (a title bar's drag, the
 * edges' gestures) does not move the pointer to each report: at every pass
 * of the event loop the pointer goes to the point the motion gives for that
 * moment, so what the shell draws follows the finger smoothly even when the
 * panel reports 30 times a second.  The lift puts the pointer where the
 * finger was last reported.
 */

#include "desktop.h"
#include "kwl.h"
#include "touch.h"
#include "data.h"
#include "extras.h"
#include "popup.h"
#include "subsurface.h"
#include <keiland/keiland.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The touch screens the compositor reads at once, and the fingers (protocol B slots) of each. */
#define TOUCH_SCREENS		2U
#define TOUCH_SLOTS		16U

/* The clients one report can have told something (every finger on another client, at most). */
#define TOUCH_HEARD_MAX		(TOUCH_SCREENS * TOUCH_SLOTS)

/*
 * Where a finger goes: no finger in the slot, a client by wl_touch, the
 * shell as the pointer's left button, a client as the pointer's left
 * button, a floating title bar's wait or flick, nowhere, a drag, or the
 * open on-screen keyboard's own press (ws102-p009).
 */
#define ROUTE_NONE		0
#define ROUTE_CLIENT		1
#define ROUTE_SHELL		2
#define ROUTE_POINTER		3
#define ROUTE_TITLE		4
#define ROUTE_IGNORED		5
#define ROUTE_DRAG		6
#define ROUTE_OSK		7

/*
 * The title bar's fingers: none; one waiting for a second; two that may
 * flick; and fingers whose gesture is decided, waiting to lift.
 */
#define TITLE_NONE		0
#define TITLE_HELD		1
#define TITLE_PAIR		2
#define TITLE_DONE		3

/* How long a finger on a title bar waits for a second one before it drags (or, lifted, taps). */
#define TITLE_PAIR_MS		150U

/* The two-finger flick up: how far both move up, and how soon after the second touched the first lifts. */
#define TITLE_FLICK_DISTANCE	24
#define TITLE_FLICK_MS		250U

/* Event opcodes of wl_touch. */
#define TOUCH_DOWN		0U
#define TOUCH_UP		1U
#define TOUCH_MOTION		2U
#define TOUCH_FRAME		3U
#define TOUCH_CANCEL		4U

/*
 * One finger of a touch screen, in its protocol B slot.
 *
 * tracking is the kernel's number for the finger, -1 when the slot is
 * empty.  The place is in 24.8 fixed point output pixels.  began, moved and
 * ended say what the report being applied did to it.  route is decided when
 * the finger touches; surface names the surface that heard wl_touch.down
 * (ROUTE_CLIENT) and is cleared before that surface is freed
 * (kwl_touch_object_gone), and down_serial is the serial of that
 * wl_touch.down (a drag names it, ws081-p014).  motion holds the finger's reports (made the
 * first time the slot has a finger, kept for the next ones); following says
 * the shell has the finger and the pointer follows the motion's point, the
 * last of which is follow_x, follow_y (output pixels).
 */
struct touch_contact {
	int32_t tracking;
	int32_t raw_x;
	int32_t raw_y;
	int32_t place_x;
	int32_t place_y;
	unsigned began;
	unsigned moved;
	unsigned ended;
	int route;
	struct kwl_object *surface;
	uint32_t down_serial;
	int32_t start_x;
	int32_t start_y;
	struct kl_motion *motion;
	unsigned following;
	int32_t follow_x;
	int32_t follow_y;
};

/*
 * One touch screen: its evdev node, the range of its fingers' places, the
 * slot its reports address now, its fingers, and what WS081 keeps of it:
 * the touch motion's device (its period, delay, noise and Scan Time), the
 * MSC_TIMESTAMP of the report being applied (msc_present when it had one)
 * and that report's time in microseconds.
 *
 * A slot of the table is in use while input is set; it lives from
 * kwl_touch_add to kwl_touch_remove, which frees the motions.
 */
struct touch_screen {
	struct kwl_input_device *input;
	struct input_absinfo axis_x;
	struct input_absinfo axis_y;
	int32_t slot;
	struct touch_contact contacts[TOUCH_SLOTS];
	struct kl_motion_device *motion_device;
	unsigned msc_present;
	uint32_t msc;
	uint64_t report_us;
};

/*
 * The fingers on a floating title bar: the state (TITLE_*), their screen
 * and slots, the window, and when each touched, in the input events' time
 * (the evdev time Wayland carries) and in the compositor's clock (for the
 * wait that passes without an event, kwl_touch_tick).
 *
 * window is cleared (and the gesture given up) before the window is freed.
 */
struct touch_title {
	unsigned state;
	struct touch_screen *screen;
	unsigned first;
	unsigned second;
	struct kwl_object *window;
	uint32_t first_time;
	uint64_t first_clock;
	uint32_t pair_time;
	uint64_t pair_clock;
};

/*
 * What one report has done so far: its time, the clients told something by
 * wl_touch (each hears one frame at the end), and whether the pointer moved
 * or clicked for a client (a wl_pointer.frame at the end).
 *
 * One instance lives on the stack while a report is applied.
 */
struct touch_report {
	uint32_t time;
	struct kwl_client *heard[TOUCH_HEARD_MAX];
	unsigned heard_count;
	unsigned pointer_activity;
};

/*
 * The touch screens the compositor reads.
 *
 * A slot's input pointer says whether it is in use; the table lives as long
 * as the process, and the event loop is its only user.
 */
static struct touch_screen screens[TOUCH_SCREENS];

/*
 * The fingers on a floating title bar (at most one gesture at a time).
 *
 * Its state is TITLE_NONE whenever no finger of it is down.
 */
static struct touch_title title;

static struct touch_screen *screen_of(struct kwl_input_device *input);
static int read_axes(struct touch_screen *screen);
static void read_report(struct kwl_server *server, struct touch_screen *screen);
static void contact_begin(struct kwl_server *server, struct touch_screen *screen, unsigned slot, struct touch_report *report);
static void contact_move(struct kwl_server *server, struct touch_screen *screen, unsigned slot, struct touch_report *report);
static void contact_end(struct kwl_server *server, struct touch_screen *screen, unsigned slot, struct touch_report *report);
static void deliver_first(struct kwl_server *server, struct touch_screen *screen, unsigned slot, struct touch_report *report);
static void title_check(struct kwl_server *server, uint32_t time);
static void title_lift(struct kwl_server *server, unsigned slot, struct touch_report *report);
static void title_flick(struct kwl_server *server, uint32_t time);
static void title_refuse(const char *reason, uint32_t elapsed);
static void title_promote(struct kwl_server *server, uint32_t time);
static void title_tap(struct kwl_server *server, uint32_t time);
static int shell_press(struct kwl_server *server, int32_t x, int32_t y, uint32_t time, uint32_t state);
static void shell_motion(struct kwl_server *server, const struct touch_contact *contact, uint32_t time);
static void shell_motion_at(struct kwl_server *server, int32_t x, int32_t y, uint32_t time);
static void motion_begin(struct touch_screen *screen, struct touch_contact *contact);
static void motion_add(struct touch_screen *screen, struct touch_contact *contact);
static void follow_start(struct touch_contact *contact);
static void follow_step(struct kwl_server *server, struct touch_screen *screen, unsigned slot, uint32_t time);
static void screen_forget_motions(struct touch_screen *screen);
static uint64_t now_microseconds(void);
static int shell_busy(void);
static struct kwl_object *surface_at(struct kwl_server *server, int32_t x, int32_t y);
static int surface_contains(const struct kwl_object *surface, int32_t x, int32_t y);
static int client_has_touch(struct kwl_client *client);
static void touch_down(struct kwl_server *server, struct touch_screen *screen, unsigned slot, struct kwl_object *surface, struct touch_report *report);
static void cancel_clients(const char *reason);
static void cancel_client(struct kwl_client *client, const char *reason);
static void place_pointer(struct kwl_server *server, int32_t x, int32_t y);
static void hide_cursor(struct kwl_server *server);
static void send_touch(struct kwl_client *client, uint32_t opcode, const void *payload, size_t size);
static void report_heard(struct touch_report *report, struct kwl_client *client);
static uint32_t contact_id(const struct touch_screen *screen, unsigned slot);
static int32_t scale_fixed(int32_t value, int32_t minimum, int32_t maximum, uint32_t size);
static int32_t magnitude(int32_t value);
static void emit(struct kwl_client *client, uint32_t id, uint32_t opcode, const void *payload, size_t size);

/*
 * Takes an evdev node classified as a touch screen: reads the range of its
 * fingers' places and the slot its reports start in.
 */
int
kwl_touch_add(
	struct kwl_input_device *input)
{
	struct touch_screen *screen;
	unsigned index;
	int error;

	/* Finds a free touch screen slot. */
	screen = NULL;
	for (index = 0; index < TOUCH_SCREENS; index++) {
		/* A slot without an input is free. */
		if (screens[index].input == NULL) {
			screen = &screens[index];
			break;
		}
	}

	/* No more touch screens can be read. */
	if (screen == NULL)
		return ENOSPC;

	/* The slot starts empty and reads this node. */
	memset(screen, 0, sizeof(*screen));
	screen->input = input;

	/* The touch motion's device learns this screen from its strokes. */
	screen->motion_device = kl_motion_device_create();
	if (screen->motion_device == NULL) {
		screen->input = NULL;
		return ENOMEM;
	}

	/* No slot holds a finger yet (-1 is the kernel's number for none). */
	for (index = 0; index < TOUCH_SLOTS; index++)
		screen->contacts[index].tracking = -1;

	/* The range of the fingers' places and the slot the reports address first. */
	error = read_axes(screen);
	if (error != 0) {
		kl_motion_device_destroy(screen->motion_device);
		screen->motion_device = NULL;
		screen->input = NULL;
		return error;
	}

	/* One line lets a test see the touch screen's range. */
	printf("KWL TOUCH added device=%s x=%d..%d y=%d..%d\n", input->path, screen->axis_x.minimum, screen->axis_x.maximum, screen->axis_y.minimum, screen->axis_y.maximum);

	/* Succeeded: the touch screen's reports are applied from now on. */
	return 0;
}

/*
 * Forgets a touch screen whose node is closing; with notify, its fingers
 * end: a client hearing one is cancelled, and a finger that is the
 * pointer's button lets it go.
 */
void
kwl_touch_remove(
	struct kwl_server *server,
	struct kwl_input_device *input,
	int notify)
{
	struct touch_screen *screen;
	struct touch_contact *contact;
	unsigned slot;

	/* A node that is not a known touch screen has nothing to forget. */
	screen = screen_of(input);
	if (screen == NULL)
		return;

	/* A gesture on a title bar of this screen is given up. */
	if (title.screen == screen) {
		title.state = TITLE_NONE;
		title.screen = NULL;
		title.window = NULL;
	}

	/* The fingers that hold the pointer's left button let it go. */
	for (slot = 0; slot < TOUCH_SLOTS; slot++) {
		contact = &screen->contacts[slot];

		/* Only a finger down that pressed the button, and only when anyone is told. */
		if (!notify || contact->tracking < 0)
			continue;

		/* The shell's press, or a client's, is released where the finger was; a drag the finger drove is given up. */
		if (contact->route == ROUTE_SHELL) {
			(void)shell_press(server, contact->place_x / 256, contact->place_y / 256, 0, 0U);
		} else if (contact->route == ROUTE_POINTER) {
			kwl_seat_button(server, 0, KWL_BUTTON_LEFT, 0U);
			kwl_seat_frame(server);
		} else if (contact->route == ROUTE_DRAG) {
			kwl_data_drag_cancel(server);
		} else if (contact->route == ROUTE_OSK) {
			kwl_keyboard_touch_cancel(server, contact_id(screen, slot));
		}

		/* The finger no longer has a route of its own. */
		if (contact->route != ROUTE_CLIENT)
			contact->route = ROUTE_IGNORED;
	}

	/* The clients hearing a finger of it are cancelled. */
	if (notify)
		cancel_clients("removed");

	/* The fingers' motions and the screen's device go with it. */
	screen_forget_motions(screen);

	/* The slot is free. */
	printf("KWL TOUCH removed device=%s\n", input->path);
	screen->input = NULL;
}

/*
 * Applies one completed report of a touch screen.
 */
void
kwl_touch_frame(
	struct kwl_server *server,
	struct kwl_input_device *input,
	uint32_t time)
{
	struct touch_screen *screen;
	struct touch_contact *contact;
	struct touch_report report;
	uint64_t stamp;
	unsigned slot;
	unsigned index;

	/* A node that is not a known touch screen is ignored. */
	screen = screen_of(input);
	if (screen == NULL)
		return;

	/* A finger is input: the lock screen's idle time starts again. */
	server->lock_input_ms = kwl_milliseconds();

	/* The report's changes to the fingers, before anything is delivered. */
	read_report(server, screen);
	memset(&report, 0, sizeof(report));

	/*
	 * The report's time: when the panel scanned it, from its Scan Time on
	 * the host clock, or when it arrived (the evdev time) without one.  The
	 * clients hear its milliseconds.
	 */
	stamp = input->frame_time_us;
	if (stamp == 0U)
		stamp = (uint64_t)time * 1000U;
	if (screen->msc_present)
		(void)kl_motion_device_time(screen->motion_device, stamp, screen->msc, &stamp);
	screen->report_us = stamp;
	report.time = (uint32_t)(stamp / 1000U);

	/* Every finger's report goes into its motion: a new finger starts one, a moved one adds to it. */
	for (slot = 0; slot < TOUCH_SLOTS; slot++) {
		contact = &screen->contacts[slot];
		if (contact->began) {
			motion_begin(screen, contact);
		} else if (contact->moved && contact->tracking >= 0) {
			motion_add(screen, contact);
		} else {
			continue;
		}

		/* A test sees each finger's report, its time and the time it arrived, with --log-frames. */
		if (server->log_frames) {
			printf("KWL TOUCH report contact=%u x=%d y=%d stamp_ms=%llu host_ms=%llu scan=%u\n", contact_id(screen, slot),
			       contact->place_x / 256, contact->place_y / 256, (unsigned long long)(stamp / 1000U),
			       (unsigned long long)(input->frame_time_us / 1000U), screen->msc_present);
		}
	}

	/* The fingers that lifted end first. */
	for (slot = 0; slot < TOUCH_SLOTS; slot++) {
		/* Only a finger that lifted in this report. */
		if (screen->contacts[slot].ended)
			contact_end(server, screen, slot, &report);
	}

	/* The fingers still down that moved move. */
	for (slot = 0; slot < TOUCH_SLOTS; slot++) {
		contact = &screen->contacts[slot];

		/* Only a finger that was down before this report and moved in it. */
		if (contact->moved &&
		    !contact->began &&
		    contact->tracking >= 0)
			contact_move(server, screen, slot, &report);
	}

	/* The title bar's fingers are judged once all of them are where the report put them. */
	title_check(server, report.time);

	/* The new fingers touch. */
	for (slot = 0; slot < TOUCH_SLOTS; slot++) {
		/* Only a finger that touched in this report. */
		if (screen->contacts[slot].began)
			contact_begin(server, screen, slot, &report);
	}

	/* Every client told something hears where the group ends. */
	for (index = 0; index < report.heard_count; index++)
		send_touch(report.heard[index], TOUCH_FRAME, NULL, 0U);

	/* The pointer's events for a client end with the pointer's frame. */
	if (report.pointer_activity)
		kwl_seat_frame(server);

	/* The next report starts with nothing changed. */
	for (slot = 0; slot < TOUCH_SLOTS; slot++) {
		screen->contacts[slot].began = 0;
		screen->contacts[slot].moved = 0;
		screen->contacts[slot].ended = 0;
	}
}

/*
 * Keeps the title bar's time when no finger moves: a single finger that has
 * waited long enough for a second one starts dragging, and two fingers that
 * have not flicked in time are let go.
 */
void
kwl_touch_tick(
	struct kwl_server *server)
{
	struct touch_screen *screen;
	uint64_t now;
	uint32_t time;
	unsigned index;
	unsigned slot;

	/* The pointer follows each finger the shell has to where its motion says it is now. */
	now = kwl_milliseconds();
	for (index = 0; index < TOUCH_SCREENS; index++) {
		screen = &screens[index];
		if (screen->input == NULL)
			continue;
		for (slot = 0; slot < TOUCH_SLOTS; slot++) {
			if (screen->contacts[slot].following)
				follow_step(server, screen, slot, (uint32_t)now);
		}
	}

	/* Only fingers on a title bar wait for anything. */
	if (title.state != TITLE_HELD && title.state != TITLE_PAIR)
		return;

	/* The time an event would carry now, counted on from the first finger's. */
	time = title.first_time + (uint32_t)(now - title.first_clock);

	/* Succeeded: the title bar's fingers are judged at that time. */
	title_check(server, time);
}

/*
 * Stops naming a surface that is going: a finger on it is no longer
 * delivered, and a gesture on its title bar is given up.
 */
void
kwl_touch_object_gone(
	struct kwl_object *object)
{
	struct touch_contact *contact;
	unsigned index;
	unsigned slot;

	/* Only surfaces are named by the fingers. */
	if (object->kind != KWL_SURFACE)
		return;

	/* Every finger delivered to the surface goes nowhere until it lifts. */
	for (index = 0; index < TOUCH_SCREENS; index++) {
		/* A free slot has no fingers. */
		if (screens[index].input == NULL)
			continue;

		/* Each finger of the screen. */
		for (slot = 0; slot < TOUCH_SLOTS; slot++) {
			contact = &screens[index].contacts[slot];

			/* Only a finger on this surface. */
			if (contact->surface != object)
				continue;

			/* It names the surface no more and goes nowhere until it lifts. */
			contact->surface = NULL;
			contact->route = ROUTE_IGNORED;
		}
	}

	/* The fingers on the window's title bar wait to lift without an effect. */
	if (title.window == object) {
		title.window = NULL;
		if (title.state != TITLE_NONE)
			title.state = TITLE_DONE;
	}
}

/*
 * Gives a client's finger to a drag and drop it starts
 * (wl_data_device.start_drag, data.c, ws081-p014): the finger still down
 * whose wl_touch.down had the serial.  The pointer goes to the finger, the
 * client hears wl_touch.cancel (its fingers are the drag's, or nowhere,
 * from now on) and the finger drives the drag until it lifts.
 *
 * Returns 1 when a finger was given, 0 when the client has none of that
 * serial down.
 */
int
kwl_touch_drag_start(
	struct kwl_server *server,
	struct kwl_client *client,
	uint32_t serial)
{
	struct touch_contact *contact;
	struct touch_contact *found;
	unsigned slot;
	unsigned index;
	uint32_t number;

	/* The client's finger down with that serial. */
	found = NULL;
	number = 0;
	for (index = 0; index < TOUCH_SCREENS && found == NULL; index++) {
		/* A free slot of the table has no fingers. */
		if (screens[index].input == NULL)
			continue;

		/* Each finger of the screen. */
		for (slot = 0; slot < TOUCH_SLOTS; slot++) {
			contact = &screens[index].contacts[slot];
			if (contact->tracking < 0 || contact->route != ROUTE_CLIENT)
				continue;
			if (contact->surface == NULL || contact->surface->client != client || contact->down_serial != serial)
				continue;
			found = contact;
			number = contact_id(&screens[index], slot);
			break;
		}
	}

	/* No such finger: the drag needs another press. */
	if (found == NULL)
		return 0;

	/* The finger is the drag's; its client's fingers are cancelled, and the pointer goes to it. */
	found->route = ROUTE_DRAG;
	found->surface = NULL;
	cancel_client(client, "drag");
	place_pointer(server, found->place_x / 256, found->place_y / 256);
	printf("KWL TOUCH drag start client=%llu contact=%u x=%d y=%d\n", (unsigned long long)client->number, number, found->place_x / 256,
	       found->place_y / 256);

	/* Succeeded: the finger drives the drag. */
	return 1;
}

/* Finds the touch screen slot of an input device, NULL when there is none. */
static struct touch_screen *
screen_of(
	struct kwl_input_device *input)
{
	unsigned index;

	/* Compares every slot in use. */
	for (index = 0; index < TOUCH_SCREENS; index++) {
		/* The slot reading this node. */
		if (screens[index].input == input)
			return &screens[index];
	}

	/* The node is not a known touch screen. */
	return NULL;
}

/* Reads the range of a touch screen's fingers' places and its current slot. */
static int
read_axes(
	struct touch_screen *screen)
{
	struct input_absinfo slot;
	int descriptor;
	int error;

	/* The horizontal range of the fingers' places. */
	descriptor = screen->input->fd;
	error = kl_backend_input_absinfo(descriptor, ABS_MT_POSITION_X, &screen->axis_x);
	if (error != 0)
		return error;

	/* The vertical range. */
	error = kl_backend_input_absinfo(descriptor, ABS_MT_POSITION_Y, &screen->axis_y);
	if (error != 0)
		return error;

	/* An empty or inverted range has no pixel to map to. */
	if (screen->axis_x.maximum <= screen->axis_x.minimum)
		return EINVAL;
	if (screen->axis_y.maximum <= screen->axis_y.minimum)
		return EINVAL;

	/* The slot the next report's finger events address until one names another. */
	memset(&slot, 0, sizeof(slot));
	error = kl_backend_input_absinfo(descriptor, ABS_MT_SLOT, &slot);
	if (error != 0)
		return error;
	screen->slot = slot.value;

	/* Succeeded: the touch screen's places can be mapped onto the output. */
	return 0;
}

/*
 * Takes one report's events into the fingers: which touched, moved and
 * lifted, and where each is now on the output.
 */
static void
read_report(
	struct kwl_server *server,
	struct touch_screen *screen)
{
	const struct input_event *event;
	struct touch_contact *contact;
	unsigned index;
	unsigned slot;

	/* The report has no Scan Time until one is read. */
	screen->msc_present = 0;

	/* Each event of the report, in order. */
	for (index = 0; index < screen->input->frame_count; index++) {
		event = &screen->input->frame[index];

		/* The panel's Scan Time, for the report's time (WS081). */
		if (event->type == EV_MSC && event->code == MSC_TIMESTAMP) {
			screen->msc_present = 1;
			screen->msc = (uint32_t)event->value;
			continue;
		}

		/* Only the multitouch axes matter (BTN_TOUCH and ABS_X/Y repeat the first finger). */
		if (event->type != EV_ABS)
			continue;

		/* ABS_MT_SLOT chooses the finger the following events are about. */
		if (event->code == ABS_MT_SLOT) {
			screen->slot = event->value;
			continue;
		}

		/* A slot beyond the table is not followed. */
		if (screen->slot < 0 || screen->slot >= (int32_t)TOUCH_SLOTS)
			continue;
		contact = &screen->contacts[screen->slot];

		/* The finger's number, its place across, or its place down. */
		switch (event->code) {
		case ABS_MT_TRACKING_ID:
			/* -1 lifts the finger. */
			if (event->value < 0) {
				/* A finger that touched in this same report never was; one down before ends. */
				if (contact->tracking >= 0 && contact->began) {
					contact->began = 0;
				} else if (contact->tracking >= 0) {
					contact->ended = 1;
				}

				/* The slot is empty. */
				contact->tracking = -1;
				break;
			}

			/* A new number in a slot with a finger replaces it: the old one ends. */
			if (contact->tracking >= 0 &&
			    contact->tracking != event->value &&
			    !contact->began)
				contact->ended = 1;

			/* The new finger touches. */
			if (contact->tracking != event->value)
				contact->began = 1;
			contact->tracking = event->value;
			break;
		case ABS_MT_POSITION_X:
			contact->raw_x = event->value;
			contact->moved = 1;
			break;
		case ABS_MT_POSITION_Y:
			contact->raw_y = event->value;
			contact->moved = 1;
			break;
		default:
			break;
		}
	}

	/* The whole touch screen maps onto the whole output. */
	for (slot = 0; slot < TOUCH_SLOTS; slot++) {
		contact = &screen->contacts[slot];
		contact->place_x = scale_fixed(contact->raw_x, screen->axis_x.minimum, screen->axis_x.maximum, server->width);
		contact->place_y = scale_fixed(contact->raw_y, screen->axis_y.minimum, screen->axis_y.maximum, server->height);
	}
}

/*
 * Starts a finger: on a floating title bar it waits for a second one; the
 * first finger otherwise goes to the shell as the pointer's left button,
 * and on to the surface under it when the shell leaves it; a finger while
 * another has the pointer goes only to a client with wl_touch.
 */
static void
contact_begin(
	struct kwl_server *server,
	struct touch_screen *screen,
	unsigned slot,
	struct touch_report *report)
{
	struct touch_contact *contact;
	struct kwl_object *window;
	struct kwl_object *target;
	uint32_t elapsed;
	int32_t x;
	int32_t y;
	int busy;
	int bound;
	int taken;
	int inside;

	/* A touch while the Windows key is down: no tap of its own (ws142-p002). */
	kwl_super_tap_cancel(&server->super_tap);

	/* The cursor hides while the fingers drive the pointer, until a mouse or a touch pad moves it (BUG-267). */
	hide_cursor(server);

	/* Where the finger touched. */
	contact = &screen->contacts[slot];
	x = contact->place_x / 256;
	y = contact->place_y / 256;
	contact->start_x = x;
	contact->start_y = y;
	contact->surface = NULL;

	/*
	 * A finger on the open keyboard's panel is the keyboard's own press,
	 * whatever other fingers do (ws102-p009); one in a bottom corner is
	 * the corner's swipe (the shell's, below), also where the panel reaches
	 * the corner.
	 */
	inside = kwl_keyboard_at(x, y);
	if (y >= (int32_t)server->height - KWL_KEYBOARD_ZONE &&
	    (x < KWL_KEYBOARD_ZONE || x >= (int32_t)server->width - KWL_KEYBOARD_ZONE))
		inside = 0;
	if (inside) {
		taken = kwl_keyboard_touch_down(server, contact_id(screen, slot), x, y, report->time);
		if (taken) {
			contact->route = ROUTE_OSK;
			printf("KWL TOUCH osk contact=%u x=%d y=%d\n", contact_id(screen, slot), x, y);
			return;
		}
	}

	/* A finger joining one that waits on a title bar may make the pair of the flick. */
	if (title.state == TITLE_HELD) {
		window = kwl_glass_title_at(server, x, y);
		elapsed = report->time - title.first_time;
		if (window == title.window &&
		    title.screen == screen &&
		    elapsed <= TITLE_PAIR_MS) {
			title.state = TITLE_PAIR;
			title.second = slot;
			title.pair_time = report->time;
			title.pair_clock = kwl_milliseconds();
			contact->route = ROUTE_TITLE;
			printf("KWL TOUCH title pair window=%llu:%u contact=%u x=%d y=%d after_ms=%u\n", (unsigned long long)window->client->number, window->id, contact_id(screen, slot), x, y, elapsed);
			return;
		}

		/* Otherwise the waiting finger does what a single finger does, and this is another finger. */
		title_promote(server, report->time);
	}

	/* While another finger has the pointer or a title bar, this one goes only to a client with wl_touch. */
	busy = shell_busy();
	if (busy) {
		/* The surface under it. */
		target = surface_at(server, x, y);

		/* Its client must have wl_touch. */
		bound = 0;
		if (target != NULL)
			bound = client_has_touch(target->client);

		/* Otherwise the finger goes nowhere until it lifts. */
		if (!bound) {
			contact->route = ROUTE_IGNORED;
			printf("KWL TOUCH ignored contact=%u x=%d y=%d\n", contact_id(screen, slot), x, y);
			return;
		}

		/* The client hears down. */
		touch_down(server, screen, slot, target, report);
		return;
	}

	/* A first finger on a floating title bar waits a moment for a second one. */
	window = kwl_glass_title_at(server, x, y);
	if (window != NULL) {
		title.state = TITLE_HELD;
		title.screen = screen;
		title.first = slot;
		title.window = window;
		title.first_time = report->time;
		title.first_clock = kwl_milliseconds();
		contact->route = ROUTE_TITLE;
		printf("KWL TOUCH title held window=%llu:%u contact=%u x=%d y=%d\n", (unsigned long long)window->client->number, window->id, contact_id(screen, slot), x, y);
		return;
	}

	/* Otherwise the finger is the pointer's left button for the compositor first. */
	taken = shell_press(server, x, y, report->time, 1U);
	if (taken) {
		contact->route = ROUTE_SHELL;
		follow_start(contact);
		printf("KWL TOUCH shell contact=%u x=%d y=%d\n", contact_id(screen, slot), x, y);
		cancel_clients("shell");
		return;
	}

	/* Succeeded: what the compositor left goes to the surface under the finger. */
	deliver_first(server, screen, slot, report);
}

/*
 * Delivers a first finger the shell did not take to the surface under it:
 * by wl_touch to a client that has one, otherwise as the pointer's left
 * button (which the shell's press has already recorded as held).
 */
static void
deliver_first(
	struct kwl_server *server,
	struct touch_screen *screen,
	unsigned slot,
	struct touch_report *report)
{
	struct touch_contact *contact;
	struct kwl_object *target;
	int32_t x;
	int32_t y;
	int inside;
	int bound;

	/* The pointer's surface (the shell has raised the window under the finger). */
	contact = &screen->contacts[slot];
	x = contact->place_x / 256;
	y = contact->place_y / 256;
	kwl_seat_pointer_update(server);
	target = server->pointer_surface;

	/* The finger must be on it. */
	inside = 0;
	if (target != NULL)
		inside = surface_contains(target, x, y);

	/* Otherwise the surface at the finger's point, found without the pointer. */
	if (!inside)
		target = surface_at(server, x, y);

	/* Nothing under the finger: the button the shell recorded is not held for anyone. */
	if (target == NULL) {
		server->buttons_down &= ~1U;
		contact->route = ROUTE_IGNORED;
		printf("KWL TOUCH ignored contact=%u x=%d y=%d\n", contact_id(screen, slot), x, y);
		return;
	}

	/* A client with wl_touch hears down; the pointer's button is not held for it. */
	bound = client_has_touch(target->client);
	if (bound) {
		server->buttons_down &= ~1U;
		touch_down(server, screen, slot, target, report);
		return;
	}

	/*
	 * Succeeded: any other client hears the pointer come to the finger,
	 * then its left button, and the finger moves the pointer from now on.
	 */
	kwl_seat_motion_deliver(server, report->time);
	kwl_seat_button_deliver(server, report->time, KWL_BUTTON_LEFT, 1U);
	contact->route = ROUTE_POINTER;
	report->pointer_activity = 1;
	printf("KWL TOUCH pointer client=%llu surface=%u contact=%u x=%d y=%d\n", (unsigned long long)target->client->number, target->id, contact_id(screen, slot), x, y);
}

/* Moves a finger the way it is delivered. */
static void
contact_move(
	struct kwl_server *server,
	struct touch_screen *screen,
	unsigned slot,
	struct touch_report *report)
{
	struct touch_contact *contact;
	struct kwl_object *surface;
	uint32_t words[4];

	/* The finger and where it goes. */
	contact = &screen->contacts[slot];

	/* Each route moves in its own way; a title bar's fingers are judged together (title_check). */
	switch (contact->route) {
	case ROUTE_CLIENT:
		/* The client hears the finger's surface-local place. */
		surface = contact->surface;
		if (surface == NULL || surface->dead)
			break;
		words[0] = report->time;
		words[1] = contact_id(screen, slot);
		words[2] = (uint32_t)(contact->place_x - surface->x * 256);
		words[3] = (uint32_t)(contact->place_y - surface->y * 256);
		send_touch(surface->client, TOUCH_MOTION, words, sizeof(words));
		report_heard(report, surface->client);
		break;
	case ROUTE_SHELL:
		/* The shell follows the pointer, which follows the finger's motion (a move, a gesture, a screen). */
		follow_step(server, screen, slot, report->time);
		break;
	case ROUTE_POINTER:
		/* The pointer moves, through the shell to the client. */
		place_pointer(server, contact->place_x / 256, contact->place_y / 256);
		server->shell_source = KWL_CONTACT_TOUCH;
		kwl_seat_motion(server, report->time);
		server->shell_source = KWL_CONTACT_POINTER;
		report->pointer_activity = 1;
		break;
	case ROUTE_DRAG:
		/* The drag follows the finger: the pointer goes there, and the target under it hears the drag (ws081-p014). */
		place_pointer(server, contact->place_x / 256, contact->place_y / 256);
		kwl_data_drag_motion(server, report->time);
		break;
	case ROUTE_OSK:
		/* The keyboard's press follows its finger (ws102-p009). */
		(void)kwl_keyboard_touch_motion(server, contact_id(screen, slot), contact->place_x / 256, contact->place_y / 256, report->time);
		break;
	default:
		break;
	}
}

/* Ends a finger the way it was delivered; the slot is empty afterwards. */
static void
contact_end(
	struct kwl_server *server,
	struct touch_screen *screen,
	unsigned slot,
	struct touch_report *report)
{
	struct touch_contact *contact;
	struct kwl_object *surface;
	uint32_t words[3];

	/* The finger and where it went. */
	contact = &screen->contacts[slot];

	/* Each route ends in its own way. */
	switch (contact->route) {
	case ROUTE_CLIENT:
		/* The client hears up, unless the surface went meanwhile. */
		surface = contact->surface;
		if (surface == NULL || surface->dead)
			break;
		words[0] = kwl_next_serial(server);
		words[1] = report->time;
		words[2] = contact_id(screen, slot);
		send_touch(surface->client, TOUCH_UP, words, sizeof(words));
		report_heard(report, surface->client);
		break;
	case ROUTE_SHELL:
		/* The pointer goes to where the finger was last reported, and the shell hears the release there. */
		shell_motion(server, contact, report->time);
		(void)shell_press(server, contact->place_x / 256, contact->place_y / 256, report->time, 0U);
		break;
	case ROUTE_POINTER:
		/* The button is released, through the shell to the client. */
		server->shell_source = KWL_CONTACT_TOUCH;
		kwl_seat_button(server, report->time, KWL_BUTTON_LEFT, 0U);
		server->shell_source = KWL_CONTACT_POINTER;
		report->pointer_activity = 1;
		break;
	case ROUTE_TITLE:
		/* A title bar's finger ends its wait or its flick. */
		title_lift(server, slot, report);
		break;
	case ROUTE_DRAG:
		/* The lift drops where the finger was last reported (ws081-p014). */
		place_pointer(server, contact->place_x / 256, contact->place_y / 256);
		kwl_data_drag_motion(server, report->time);
		kwl_data_drag_release(server);
		printf("KWL TOUCH drag lift contact=%u x=%d y=%d\n", contact_id(screen, slot), contact->place_x / 256, contact->place_y / 256);
		break;
	case ROUTE_OSK:
		/* The keyboard's press ends where the finger was last reported (ws102-p009). */
		(void)kwl_keyboard_touch_up(server, contact_id(screen, slot), contact->place_x / 256, contact->place_y / 256, report->time);
		break;
	default:
		break;
	}

	/* The stroke teaches the screen's device; the slot holds no finger any more. */
	if (contact->motion != NULL)
		kl_motion_end(contact->motion);
	contact->following = 0;
	contact->route = ROUTE_NONE;
	contact->surface = NULL;
}

/*
 * Judges the title bar's fingers at a time: a single finger that has
 * waited TITLE_PAIR_MS starts what a single finger does; two fingers that
 * have gone on longer than TITLE_FLICK_MS, or moved down or sideways, are
 * no flick.
 */
static void
title_check(
	struct kwl_server *server,
	uint32_t time)
{
	struct touch_contact *first;
	struct touch_contact *second;
	uint32_t elapsed;
	int32_t dx;
	int32_t dy;
	int32_t across;
	unsigned index;

	/* A single finger that waited long enough drags (or presses) the title bar. */
	if (title.state == TITLE_HELD) {
		elapsed = time - title.first_time;
		if (elapsed > TITLE_PAIR_MS)
			title_promote(server, time);
		return;
	}

	/* Only two fingers that may still flick are judged further. */
	if (title.state != TITLE_PAIR)
		return;

	/* Too long together is a slow drag, not a flick. */
	elapsed = time - title.pair_time;
	if (elapsed > TITLE_FLICK_MS) {
		title_refuse("slow", elapsed);
		return;
	}

	/* Each of the two fingers must not have gone down or mostly sideways. */
	first = &title.screen->contacts[title.first];
	second = &title.screen->contacts[title.second];
	for (index = 0; index < 2U; index++) {
		/* The first finger, then the second. */
		if (index == 0U) {
			dx = first->place_x / 256 - first->start_x;
			dy = first->place_y / 256 - first->start_y;
		} else {
			dx = second->place_x / 256 - second->start_x;
			dy = second->place_y / 256 - second->start_y;
		}

		/* Down by the flick's distance is the other way. */
		if (dy >= TITLE_FLICK_DISTANCE) {
			title_refuse("down", elapsed);
			return;
		}

		/* Sideways by the flick's distance, and more sideways than up, is no flick either. */
		across = magnitude(dx);
		if (across >= TITLE_FLICK_DISTANCE && across > -dy) {
			title_refuse("sideways", elapsed);
			return;
		}
	}
}

/*
 * Ends one of the title bar's fingers: a single one lifted in time taps;
 * the first of two lifted decides the flick; the last finger of a decided
 * gesture frees the title bar.
 */
static void
title_lift(
	struct kwl_server *server,
	unsigned slot,
	struct touch_report *report)
{
	struct touch_contact *other;
	unsigned other_slot;

	/* A single finger lifted before its wait was over is a tap. */
	if (title.state == TITLE_HELD && slot == title.first) {
		title_tap(server, report->time);
		return;
	}

	/* The first of two fingers to lift decides whether they flicked. */
	if (title.state == TITLE_PAIR) {
		title_flick(server, report->time);
		return;
	}

	/* Only a decided gesture is left, whose title bar is free once neither finger is down. */
	if (title.state != TITLE_DONE)
		return;

	/* The gesture's other finger. */
	other_slot = title.first;
	if (slot == title.first)
		other_slot = title.second;
	other = &title.screen->contacts[other_slot];

	/* A single finger's gesture, or an other finger lifted already (or never the title bar's), frees it. */
	if (other_slot == slot ||
	    other->tracking < 0 ||
	    other->route != ROUTE_TITLE) {
		title.state = TITLE_NONE;
		title.screen = NULL;
		title.window = NULL;
	}
}

/*
 * Decides the two fingers' flick when the first of them lifts: both moved
 * up far enough, more up than sideways, soon enough after the second
 * touched, and the window goes to the back.
 */
static void
title_flick(
	struct kwl_server *server,
	uint32_t time)
{
	struct touch_contact *first;
	struct touch_contact *second;
	struct kwl_object *window;
	uint32_t elapsed;
	int32_t first_dx;
	int32_t first_dy;
	int32_t second_dx;
	int32_t second_dy;
	int32_t first_across;
	int32_t second_across;

	/* How long the two were together, and how far each went. */
	elapsed = time - title.pair_time;
	first = &title.screen->contacts[title.first];
	second = &title.screen->contacts[title.second];
	first_dx = first->place_x / 256 - first->start_x;
	first_dy = first->place_y / 256 - first->start_y;
	second_dx = second->place_x / 256 - second->start_x;
	second_dy = second->place_y / 256 - second->start_y;

	/* Too long together is a slow drag. */
	if (elapsed > TITLE_FLICK_MS) {
		title_refuse("slow", elapsed);
		return;
	}

	/* Each finger must have gone up by the flick's distance. */
	if (-first_dy < TITLE_FLICK_DISTANCE || -second_dy < TITLE_FLICK_DISTANCE) {
		printf("KWL TOUCH flick dy=%d,%d\n", first_dy, second_dy);
		title_refuse("short", elapsed);
		return;
	}

	/* Each finger must have gone more up than sideways. */
	first_across = magnitude(first_dx);
	second_across = magnitude(second_dx);
	if (first_across > -first_dy || second_across > -second_dy) {
		title_refuse("sideways", elapsed);
		return;
	}

	/* The gesture is decided; the window, when it is still there, goes to the back ("go away"). */
	window = title.window;
	title.state = TITLE_DONE;
	if (window == NULL ||
	    window->dead ||
	    !window->mapped) {
		printf("KWL TOUCH flick gone ms=%u\n", elapsed);
		return;
	}

	/* The log line comes before the shell's own line of the lowering. */
	printf("KWL TOUCH flick window=%llu:%u dx=%d,%d dy=%d,%d ms=%u\n", (unsigned long long)window->client->number, window->id, first_dx, second_dx, first_dy, second_dy, elapsed);
	cancel_clients("flick");

	/* Succeeded: the same as a triple click on its title bar. */
	kwl_glass_lower(server, window, "two-finger-flick");
}

/* Gives up the two fingers' flick: they wait to lift without an effect. */
static void
title_refuse(
	const char *reason,
	uint32_t elapsed)
{
	/* The log line says why. */
	printf("KWL TOUCH flick refused reason=%s ms=%u\n", reason, elapsed);

	/* Succeeded: the fingers are the title bar's until they lift. */
	title.state = TITLE_DONE;
}

/*
 * Lets a single finger that waited on a title bar do what a single finger
 * does: the shell hears the press where it touched, then the way it moved
 * since (a drag of the title bar).
 */
static void
title_promote(
	struct kwl_server *server,
	uint32_t time)
{
	struct touch_contact *contact;
	struct touch_screen *screen;
	int taken;

	/* The waiting finger and its screen; the title bar is free again. */
	screen = title.screen;
	contact = &screen->contacts[title.first];
	title.state = TITLE_NONE;
	title.screen = NULL;
	title.window = NULL;

	/* The press where it touched, as the pointer's left button. */
	taken = shell_press(server, contact->start_x, contact->start_y, time, 1U);
	if (!taken) {
		server->buttons_down &= ~1U;
		contact->route = ROUTE_IGNORED;
		printf("KWL TOUCH title drag refused contact=%u\n", contact_id(screen, title.first));
		return;
	}

	/* The shell has the finger now, and the clients' fingers are cancelled. */
	contact->route = ROUTE_SHELL;
	follow_start(contact);
	printf("KWL TOUCH title drag contact=%u x=%d y=%d\n", contact_id(screen, title.first), contact->start_x, contact->start_y);
	cancel_clients("shell");

	/* Succeeded: the way it moved since it touched follows at once. */
	shell_motion(server, contact, time);
}

/*
 * Lets a single finger that lifted before its wait was over tap the title
 * bar: the shell hears the press and the release, as a click.
 */
static void
title_tap(
	struct kwl_server *server,
	uint32_t time)
{
	struct touch_contact *contact;
	struct touch_screen *screen;
	int taken;

	/* The finger and its screen; the title bar is free again. */
	screen = title.screen;
	contact = &screen->contacts[title.first];
	title.state = TITLE_NONE;
	title.screen = NULL;
	title.window = NULL;
	printf("KWL TOUCH title tap contact=%u x=%d y=%d\n", contact_id(screen, title.first), contact->start_x, contact->start_y);

	/* The press where it touched. */
	taken = shell_press(server, contact->start_x, contact->start_y, time, 1U);
	if (!taken) {
		server->buttons_down &= ~1U;
		return;
	}

	/* Succeeded: the release where it lifted. */
	(void)shell_press(server, contact->place_x / 256, contact->place_y / 256, time, 0U);
}

/*
 * Passes a finger's press or release through the shell as the pointer's
 * left button at a point; reports whether the shell took it.
 */
static int
shell_press(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	uint32_t time,
	uint32_t state)
{
	int taken;

	/* The pointer is where the finger is, and the shell knows a finger is its source. */
	place_pointer(server, x, y);
	server->shell_source = KWL_CONTACT_TOUCH;
	taken = kwl_seat_button_shell(server, time, KWL_BUTTON_LEFT, state);
	server->shell_source = KWL_CONTACT_POINTER;

	/* Succeeded: whether the shell took it. */
	return taken;
}

/* Passes a finger's movement through the shell as the pointer's, to where it was last reported. */
static void
shell_motion(
	struct kwl_server *server,
	const struct touch_contact *contact,
	uint32_t time)
{
	/* The finger's last reported place. */
	shell_motion_at(server, contact->place_x / 256, contact->place_y / 256, time);
}

/* Passes a movement to a point through the shell as the pointer's. */
static void
shell_motion_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	uint32_t time)
{
	/* The pointer goes to the point. */
	place_pointer(server, x, y);

	/* The shell's move, gesture or screen follows the pointer. */
	server->shell_source = KWL_CONTACT_TOUCH;
	(void)kwl_seat_motion_shell(server, time);
	server->shell_source = KWL_CONTACT_POINTER;
}

/*
 * Starts a finger's motion with its first report: the slot's motion is
 * made the first time it has a finger and kept for the next ones.  Without
 * memory the finger has none, and the shell follows its reports as they
 * come.
 */
static void
motion_begin(
	struct touch_screen *screen,
	struct touch_contact *contact)
{
	/* The slot's motion, made once. */
	if (contact->motion == NULL)
		contact->motion = kl_motion_create(screen->motion_device);
	contact->following = 0;
	if (contact->motion == NULL)
		return;

	/* A new stroke from this report. */
	kl_motion_begin(contact->motion);
	motion_add(screen, contact);
}

/* Adds a finger's report (its place and the report's time) to its motion. */
static void
motion_add(
	struct touch_screen *screen,
	struct touch_contact *contact)
{
	uint64_t arrival;
	int error;

	/* A finger without a motion is followed by its reports. */
	if (contact->motion == NULL)
		return;

	/* The report, in output pixels, when it was scanned and when it was read. */
	arrival = now_microseconds();
	error = kl_motion_add(contact->motion, screen->report_us, arrival, contact->place_x / 256.0,
				   contact->place_y / 256.0);

	/* A report older than the last (a clock started again) starts the stroke again from it. */
	if (error != 0) {
		kl_motion_begin(contact->motion);
		(void)kl_motion_add(contact->motion, screen->report_us, arrival, contact->place_x / 256.0,
					 contact->place_y / 256.0);
	}
}

/* Lets the pointer follow a finger the shell has just taken, from where the finger is. */
static void
follow_start(
	struct touch_contact *contact)
{
	/* A finger without a motion is followed by its reports (shell_motion). */
	if (contact->motion == NULL)
		return;

	/* The pointer is at the finger now. */
	contact->following = 1;
	contact->follow_x = contact->place_x / 256;
	contact->follow_y = contact->place_y / 256;
}

/*
 * Moves the pointer, and the shell with it, to where a finger the shell has
 * is now: the motion's point for this moment.  A finger the shell has
 * without a motion goes to its last report.  The pointer moves only when
 * the point is on another pixel.
 */
static void
follow_step(
	struct kwl_server *server,
	struct touch_screen *screen,
	unsigned slot,
	uint32_t time)
{
	struct touch_contact *contact;
	uint64_t now;
	double x;
	double y;
	int32_t point_x;
	int32_t point_y;
	int error;

	/* Only a finger down that the shell has. */
	contact = &screen->contacts[slot];
	if (contact->tracking < 0 || contact->route != ROUTE_SHELL)
		return;

	/* Without a motion the finger's last report is where it is. */
	if (!contact->following) {
		shell_motion(server, contact, time);
		return;
	}

	/* The motion's point for now. */
	now = now_microseconds();
	error = kl_motion_point(contact->motion, now, KL_MOTION_EXTRAPOLATION_CONTENT, &x, &y);
	if (error != 0)
		return;

	/* On the output, rounded to a pixel. */
	point_x = (int32_t)(x + 0.5);
	point_y = (int32_t)(y + 0.5);
	if (point_x < 0)
		point_x = 0;
	if (point_y < 0)
		point_y = 0;
	if (point_x >= (int32_t)server->width)
		point_x = (int32_t)server->width - 1;
	if (point_y >= (int32_t)server->height)
		point_y = (int32_t)server->height - 1;

	/* The same pixel as last time moves nothing. */
	if (point_x == contact->follow_x && point_y == contact->follow_y)
		return;
	contact->follow_x = point_x;
	contact->follow_y = point_y;

	/* The pointer and the shell go there; a test sees each step with --log-frames. */
	shell_motion_at(server, point_x, point_y, time);
	if (server->log_frames) {
		printf("KWL TOUCH follow contact=%u x=%d y=%d report_x=%d report_y=%d now_ms=%llu\n", contact_id(screen, slot), point_x,
		       point_y, contact->place_x / 256, contact->place_y / 256, (unsigned long long)(now / 1000U));
	}
}

/* Frees a screen's motions and its motion device. */
static void
screen_forget_motions(
	struct touch_screen *screen)
{
	unsigned slot;

	/* Each slot's motion, without teaching the device that goes too. */
	for (slot = 0; slot < TOUCH_SLOTS; slot++) {
		kl_motion_destroy(screen->contacts[slot].motion);
		screen->contacts[slot].motion = NULL;
		screen->contacts[slot].following = 0;
	}

	/* The device. */
	kl_motion_device_destroy(screen->motion_device);
	screen->motion_device = NULL;
}

/* Reads the monotonic clock in microseconds, the touch motion's time. */
static uint64_t
now_microseconds(void)
{
	struct timespec now;
	int error;

	/* A clock that cannot be read gives the time zero, which draws the last report. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Succeeded: the time. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/* Reports whether a finger has the pointer (the shell's or a client's) or a title bar. */
static int
shell_busy(
	void)
{
	unsigned index;
	unsigned slot;
	int route;

	/* Fingers on a title bar hold it until they lift. */
	if (title.state != TITLE_NONE)
		return 1;

	/* Any finger that is the pointer's left button. */
	for (index = 0; index < TOUCH_SCREENS; index++) {
		/* A free slot has no fingers. */
		if (screens[index].input == NULL)
			continue;

		/* Each finger of the screen: one that is the shell's or a client's pointer button. */
		for (slot = 0; slot < TOUCH_SLOTS; slot++) {
			route = screens[index].contacts[slot].route;
			if (route == ROUTE_SHELL || route == ROUTE_POINTER)
				return 1;
		}
	}

	/* No finger has the pointer. */
	return 0;
}

/*
 * Finds the surface a finger at a point touches without the shell's help:
 * the window whose body is on top there (the fullscreen or front window
 * outside the glass look's window mode), or its sub-surface there; NULL
 * when there is none or a popup's grab has the input.
 */
static struct kwl_object *
surface_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	struct kwl_object *window;
	struct kwl_object *deeper;
	int inside;

	/* A popup's grab has the input. */
	if (server->popup_grab != NULL)
		return NULL;

	/* The window: the glass look's topmost body at the point, otherwise the focused one. */
	if (server->glass && server->windowed) {
		window = kwl_glass_body_at(server, x, y);
	} else {
		window = server->focus;
	}

	/* Where no window is, the desktop's icons (desktop.c). */
	if (window == NULL)
		window = kwl_desktop_at(server, x, y);

	/* No window, or one that is going. */
	if (window == NULL ||
	    window->dead ||
	    window->client->fatal)
		return NULL;

	/* Its sub-surface at the point, when there is one. */
	deeper = kwl_subsurface_at(window, x, y);
	if (deeper != NULL)
		window = deeper;

	/* The point must be on it. */
	inside = surface_contains(window, x, y);
	if (!inside)
		return NULL;

	/* Succeeded: the surface under the point. */
	return window;
}

/* Reports whether a point of the output is on a surface. */
static int
surface_contains(
	const struct kwl_object *surface,
	int32_t x,
	int32_t y)
{
	uint32_t width;
	uint32_t height;

	/* The surface's size (its viewport's, else its buffer's). */
	kwl_surface_size(surface, &width, &height);

	/* Left of it or above it. */
	if (x < surface->x || y < surface->y)
		return 0;

	/* Right of it or below it. */
	if (x >= surface->x + (int32_t)width)
		return 0;
	if (y >= surface->y + (int32_t)height)
		return 0;

	/* Succeeded: the point is on the surface. */
	return 1;
}

/* Reports whether a client has a live wl_touch. */
static int
client_has_touch(
	struct kwl_client *client)
{
	struct kwl_object *object;

	/* A failed client has nothing. */
	if (client->fatal)
		return 0;

	/* Looks for a wl_touch among the client's objects. */
	for (object = client->objects; object != NULL; object = object->next) {
		/* A live wl_touch. */
		if (object->kind == KWL_TOUCH && !object->dead)
			return 1;
	}

	/* The client has none. */
	return 0;
}

/*
 * Tells a surface's client that a finger touched it (wl_touch.down with the
 * surface-local place); the finger belongs to the surface until it lifts.
 */
static void
touch_down(
	struct kwl_server *server,
	struct touch_screen *screen,
	unsigned slot,
	struct kwl_object *surface,
	struct touch_report *report)
{
	struct touch_contact *contact;
	uint32_t words[6];

	/* The serial, the time, the surface, the finger's number and the surface-local place. */
	contact = &screen->contacts[slot];
	words[0] = kwl_next_serial(server);
	words[1] = report->time;
	words[2] = surface->id;
	words[3] = contact_id(screen, slot);
	words[4] = (uint32_t)(contact->place_x - surface->x * 256);
	words[5] = (uint32_t)(contact->place_y - surface->y * 256);
	send_touch(surface->client, TOUCH_DOWN, words, sizeof(words));
	report_heard(report, surface->client);

	/* The finger is the surface's; down's serial is what a move or a menu the client asks for names. */
	contact->route = ROUTE_CLIENT;
	contact->surface = surface;
	contact->down_serial = words[0];
	server->press_serial = words[0];

	/* Succeeded: one line lets a test see where the finger went. */
	printf("KWL TOUCH down client=%llu surface=%u contact=%u x=%d y=%d\n", (unsigned long long)surface->client->number, surface->id, words[3], contact->place_x / 256, contact->place_y / 256);
}

/*
 * Cancels every finger a client hears by wl_touch: each such client hears
 * wl_touch.cancel once, and its fingers go nowhere until they lift.
 */
static void
cancel_clients(
	const char *reason)
{
	struct kwl_client *clients[TOUCH_HEARD_MAX];
	struct touch_contact *contact;
	unsigned count;
	unsigned index;
	unsigned seen;
	unsigned slot;
	unsigned screen;

	/* The clients of the fingers delivered by wl_touch, each once. */
	count = 0;
	for (screen = 0; screen < TOUCH_SCREENS; screen++) {
		/* A free slot has no fingers. */
		if (screens[screen].input == NULL)
			continue;

		/* Each finger of the screen. */
		for (slot = 0; slot < TOUCH_SLOTS; slot++) {
			contact = &screens[screen].contacts[slot];

			/* Only a finger a client hears. */
			if (contact->route != ROUTE_CLIENT)
				continue;

			/* The finger goes nowhere from now on; a surface that went has no client to tell. */
			contact->route = ROUTE_IGNORED;
			if (contact->surface == NULL)
				continue;

			/* Whether its client is noted already. */
			seen = 0;
			for (index = 0; index < count; index++) {
				/* A client already noted. */
				if (clients[index] == contact->surface->client)
					seen = 1;
			}

			/* A new client, while there is room (there is one for every finger). */
			if (!seen && count < TOUCH_HEARD_MAX) {
				clients[count] = contact->surface->client;
				count++;
			}

			/* The finger names the surface no more. */
			contact->surface = NULL;
		}
	}

	/* Each client hears cancel. */
	for (index = 0; index < count; index++) {
		send_touch(clients[index], TOUCH_CANCEL, NULL, 0U);
		printf("KWL TOUCH cancel client=%llu reason=%s\n", (unsigned long long)clients[index]->number, reason);
	}
}

/* Cancels the fingers one client hears by wl_touch: it hears wl_touch.cancel once, and they go nowhere until they lift. */
static void
cancel_client(
	struct kwl_client *client,
	const char *reason)
{
	struct touch_contact *contact;
	unsigned slot;
	unsigned screen;
	unsigned count;

	/* The client's fingers go nowhere from now on. */
	count = 0;
	for (screen = 0; screen < TOUCH_SCREENS; screen++) {
		/* A free slot has no fingers. */
		if (screens[screen].input == NULL)
			continue;

		/* Each finger of the client's. */
		for (slot = 0; slot < TOUCH_SLOTS; slot++) {
			contact = &screens[screen].contacts[slot];
			if (contact->route != ROUTE_CLIENT || contact->surface == NULL || contact->surface->client != client)
				continue;
			contact->route = ROUTE_IGNORED;
			contact->surface = NULL;
			count++;
		}
	}

	/* The client hears cancel, for the finger that went to the drag too. */
	send_touch(client, TOUCH_CANCEL, NULL, 0U);
	printf("KWL TOUCH cancel client=%llu reason=%s others=%u\n", (unsigned long long)client->number, reason, count);
}

/* Moves the pointer to a point, redrawing the cursor where it was and is. */
static void
place_pointer(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	int32_t old_x;
	int32_t old_y;

	/* An unchanged place needs nothing. */
	if (x == server->pointer_x && y == server->pointer_y)
		return;

	/* Succeeded: the pointer moves (on the anchor, where the screen maps, ws113-p007), and the cursor is redrawn (damage.c). */
	kwl_pointer_absolute(server);
	old_x = server->pointer_x;
	old_y = server->pointer_y;
	server->pointer_x = x;
	server->pointer_y = y;
	kwl_damage_pointer(server, old_x, old_y);
}

/*
 * Hides the cursor for a finger on the touch screen (BUG-267: the user's
 * "タッチパネルにタッチされたらマウスカーソルを非表示に"): the pointer is
 * taken as not moved by a pointing device again, which input.c undoes when
 * a mouse, a touch pad or a tablet moves it.  The place it was drawn is
 * redrawn without it.
 */
static void
hide_cursor(
	struct kwl_server *server)
{
	/* Already hidden. */
	if (server->pointer_unmoved)
		return;

	/* Not drawn from the next frame; where it was is drawn again. */
	server->pointer_unmoved = 1U;
	kwl_damage_pointer(server, server->pointer_x, server->pointer_y);
	printf("KWL CURSOR hidden by=touch\n");
}

/* Sends one event to every live wl_touch of a client. */
static void
send_touch(
	struct kwl_client *client,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	struct kwl_object *object;

	/* Each live wl_touch of the client. */
	for (object = client->objects; object != NULL; object = object->next) {
		/* Only live wl_touch objects. */
		if (object->kind != KWL_TOUCH || object->dead)
			continue;

		/* Queues the event. */
		emit(client, object->id, opcode, payload, size);
	}
}

/* Notes that a client heard something in this report, so it hears the frame once. */
static void
report_heard(
	struct touch_report *report,
	struct kwl_client *client)
{
	unsigned index;

	/* A client already noted. */
	for (index = 0; index < report->heard_count; index++) {
		/* The same client. */
		if (report->heard[index] == client)
			return;
	}

	/* A new client, while there is room (there is one for every finger). */
	if (report->heard_count < TOUCH_HEARD_MAX) {
		report->heard[report->heard_count] = client;
		report->heard_count++;
	}
}

/* Reports the number the clients know a finger by: unique among the fingers down. */
static uint32_t
contact_id(
	const struct touch_screen *screen,
	unsigned slot)
{
	unsigned index;

	/* Succeeded: each screen's slots have their own numbers. */
	index = (unsigned)(screen - screens);
	return index * TOUCH_SLOTS + slot;
}

/* Maps one axis value onto 0 .. size - 1 pixels in 24.8 fixed point. */
static int32_t
scale_fixed(
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
		return ((int32_t)size - 1) * 256;

	/* Everything between scales linearly. */
	offset = (int64_t)value - minimum;
	range = (int64_t)maximum - minimum;

	/* Succeeded: the place in 1/256 pixels. */
	return (int32_t)((offset * ((int64_t)size - 1) * 256) / range);
}

/* Reports how far a signed distance is, whichever way. */
static int32_t
magnitude(
	int32_t value)
{
	/* A distance the other way. */
	if (value < 0)
		return -value;

	/* Succeeded: the distance as it is. */
	return value;
}

/* Queues one event and marks the client failed when the queue refuses it. */
static void
emit(
	struct kwl_client *client,
	uint32_t id,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	int error;

	/* A failed client is only waiting for cleanup and hears nothing further. */
	if (client->fatal)
		return;

	/* A client that stopped reading loses its connection rather than compositor memory. */
	error = kwl_emit(client, id, opcode, payload, size);
	if (error != 0) {
		client->fatal = 1;
		client->fatal_time = kwl_milliseconds();
		return;
	}

	/* The exit summary counts every queued seat event. */
	client->server->seat_events++;
}
