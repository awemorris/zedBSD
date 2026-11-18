/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A pen tablet's tools on the window (WS131 p018, KL_VERSION 44; Notes'
 * tablet.c of ws079-p003 moved here): the tablet protocol
 * (zwp_tablet_manager_v2).
 *
 * The window binds the tablet manager with its other globals, and asks for
 * its seat's tablets when the application takes them
 * (kl_window_accept_tablet): a compositor sends the pen as the tablet
 * protocol to a client that holds a tool, and as the pointer to any other,
 * so a window whose application does not take the tablet keeps the pen as
 * the pointer.  Each tool (a pen's tip, its eraser end) reports its state
 * in pieces -- proximity, contact, position, pressure, tilt, buttons -- and
 * a frame ends each group; at the frame the tool's state becomes the
 * window's inputs: a contact starting is KL_WINDOW_TABLET_DOWN, a move in
 * contact KL_WINDOW_TABLET_MOTION, a lift KL_WINDOW_TABLET_UP; a move over
 * the window without contact is KL_WINDOW_TABLET_HOVER and leaving the
 * window KL_WINDOW_TABLET_LEAVE.  The pressure comes as 0..65535 and is
 * given on as 0..1 (-1 for a tool without pressure); the tilt comes in
 * degrees.  The compositor's cursor is hidden while a tool is over the
 * window (the application shows where the pen is).
 */

#include "window.h"

#include <wayland/tablet-unstable-v2-client-protocol.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The evdev codes of the pen's barrel buttons. */
#define TABLET_BUTTON_STYLUS	0x14bU
#define TABLET_BUTTON_STYLUS2	0x14cU

static void tablet_seat_tablet(void *data, struct zwp_tablet_seat_v2 *seat, struct zwp_tablet_v2 *tablet);
static void tablet_seat_tool(void *data, struct zwp_tablet_seat_v2 *seat, struct zwp_tablet_tool_v2 *tool);
static void tablet_seat_pad(void *data, struct zwp_tablet_seat_v2 *seat, struct zwp_tablet_pad_v2 *pad);
static void tablet_name(void *data, struct zwp_tablet_v2 *tablet, const char *name);
static void tablet_id(void *data, struct zwp_tablet_v2 *tablet, uint32_t vendor, uint32_t product);
static void tablet_path(void *data, struct zwp_tablet_v2 *tablet, const char *path);
static void tablet_done(void *data, struct zwp_tablet_v2 *tablet);
static void tablet_removed(void *data, struct zwp_tablet_v2 *tablet);
static void tool_type(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t type);
static void tool_serial(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t high, uint32_t low);
static void tool_wacom(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t high, uint32_t low);
static void tool_capability(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t capability);
static void tool_done(void *data, struct zwp_tablet_tool_v2 *tool);
static void tool_removed(void *data, struct zwp_tablet_tool_v2 *tool);
static void tool_proximity_in(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t serial, struct zwp_tablet_v2 *tablet, struct wl_surface *surface);
static void tool_proximity_out(void *data, struct zwp_tablet_tool_v2 *tool);
static void tool_down(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t serial);
static void tool_up(void *data, struct zwp_tablet_tool_v2 *tool);
static void tool_motion(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t x, wl_fixed_t y);
static void tool_pressure(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t pressure);
static void tool_distance(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t distance);
static void tool_tilt(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t x, wl_fixed_t y);
static void tool_rotation(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t degrees);
static void tool_slider(void *data, struct zwp_tablet_tool_v2 *tool, int32_t position);
static void tool_wheel(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t degrees, int32_t clicks);
static void tool_button(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t serial, uint32_t button, uint32_t state);
static void tool_frame(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t time);
static void tool_input(struct keiui_tablet_tool *state, unsigned kind, uint32_t time);

/* The seat's tablets and tools as they are announced. */
static const struct zwp_tablet_seat_v2_listener seat_listener = {
	tablet_seat_tablet,
	tablet_seat_tool,
	tablet_seat_pad
};

/* A tablet's description, which the window does not need, and its removal. */
static const struct zwp_tablet_v2_listener tablet_listener = {
	tablet_name,
	tablet_id,
	tablet_path,
	tablet_done,
	tablet_removed
};

/* A tool's description and its state, frame by frame. */
static const struct zwp_tablet_tool_v2_listener tool_listener = {
	tool_type,
	tool_serial,
	tool_wacom,
	tool_capability,
	tool_done,
	tool_removed,
	tool_proximity_in,
	tool_proximity_out,
	tool_down,
	tool_up,
	tool_motion,
	tool_pressure,
	tool_distance,
	tool_tilt,
	tool_rotation,
	tool_slider,
	tool_wheel,
	tool_button,
	tool_frame
};

/*
 * Takes the pen tablet's tools as the window's inputs (KL_WINDOW_TABLET_*)
 * from now on.  Returns 0, EINVAL, or ENOTSUP without the compositor's
 * tablet protocol (the pen then stays the pointer).
 */
int
kl_window_accept_tablet(
	struct kl_window *window)
{
	int status;

	/* A window; taken once. */
	if (window == NULL)
		return EINVAL;
	if (window->tablet_seat != NULL)
		return 0;

	/* Without the manager or the seat the pen comes as the pointer. */
	if (window->tablet_manager == NULL || window->seat == NULL)
		return ENOTSUP;

	/* The seat's tablets, announced to the listener. */
	window->tablet_seat = zwp_tablet_manager_v2_get_tablet_seat(window->tablet_manager, window->seat);
	if (window->tablet_seat == NULL)
		return ENOMEM;
	status = zwp_tablet_seat_v2_add_listener(window->tablet_seat, &seat_listener, window);
	if (status != 0)
		return EINVAL;

	/* Succeeded: the tools are announced with the next events. */
	(void)wl_display_flush(window->display);
	return 0;
}

/*
 * Binds the compositor's tablet manager (version 1; from the registry).
 */
void
keiui_tablet_bind(
	struct kl_window *window,
	struct wl_registry *registry,
	uint32_t name)
{
	/* One manager is enough. */
	if (window->tablet_manager != NULL)
		return;
	window->tablet_manager = wl_registry_bind(registry, name, &zwp_tablet_manager_v2_interface, 1U);
}

/*
 * Releases the tools, the tablets' seat and the manager (before the seat).
 */
void
keiui_tablet_close(
	struct kl_window *window)
{
	unsigned index;

	/* Each tool and its state. */
	for (index = 0; index < window->tool_count; index++) {
		zwp_tablet_tool_v2_destroy(window->tools[index]->tool);
		free(window->tools[index]);
	}
	window->tool_count = 0;

	/* The seat, then the manager. */
	if (window->tablet_seat != NULL)
		zwp_tablet_seat_v2_destroy(window->tablet_seat);
	if (window->tablet_manager != NULL)
		zwp_tablet_manager_v2_destroy(window->tablet_manager);
	window->tablet_seat = NULL;
	window->tablet_manager = NULL;
}

/* A new tablet: its events are listened to, only for its removal. */
static void
tablet_seat_tablet(
	void *data,
	struct zwp_tablet_seat_v2 *seat,
	struct zwp_tablet_v2 *tablet)
{
	/* The tablet's description and removal. */
	(void)seat;
	(void)zwp_tablet_v2_add_listener(tablet, &tablet_listener, data);
}

/* A new tool: its state is kept, and its events listened to. */
static void
tablet_seat_tool(
	void *data,
	struct zwp_tablet_seat_v2 *seat,
	struct zwp_tablet_tool_v2 *tool)
{
	struct keiui_tablet_tool *state;
	struct kl_window *window;

	/* A full table leaves the tool alone (its events are never listened to). */
	(void)seat;
	window = data;
	if (window->tool_count >= KEIUI_TABLET_TOOLS)
		return;

	/* The tool's state, a pen until its type says otherwise. */
	state = calloc(1U, sizeof(*state));
	if (state == NULL)
		return;
	state->window = window;
	state->tool = tool;
	state->type = KL_TABLET_PEN;

	/* Succeeded: the tool is kept and heard. */
	window->tools[window->tool_count] = state;
	window->tool_count++;
	(void)zwp_tablet_tool_v2_add_listener(tool, &tool_listener, state);
}

/* A pad is not used. */
static void
tablet_seat_pad(
	void *data,
	struct zwp_tablet_seat_v2 *seat,
	struct zwp_tablet_pad_v2 *pad)
{
	/* The pad is given back. */
	(void)data;
	(void)seat;
	zwp_tablet_pad_v2_destroy(pad);
}

/* A tablet's name is not used. */
static void
tablet_name(
	void *data,
	struct zwp_tablet_v2 *tablet,
	const char *name)
{
	/* Nothing to do. */
	(void)data;
	(void)tablet;
	(void)name;
}

/* A tablet's identity is not used. */
static void
tablet_id(
	void *data,
	struct zwp_tablet_v2 *tablet,
	uint32_t vendor,
	uint32_t product)
{
	/* Nothing to do. */
	(void)data;
	(void)tablet;
	(void)vendor;
	(void)product;
}

/* A tablet's device path is not used. */
static void
tablet_path(
	void *data,
	struct zwp_tablet_v2 *tablet,
	const char *path)
{
	/* Nothing to do. */
	(void)data;
	(void)tablet;
	(void)path;
}

/* A tablet's description is complete: nothing to do. */
static void
tablet_done(
	void *data,
	struct zwp_tablet_v2 *tablet)
{
	/* Nothing to do. */
	(void)data;
	(void)tablet;
}

/* A tablet was unplugged: its object goes. */
static void
tablet_removed(
	void *data,
	struct zwp_tablet_v2 *tablet)
{
	/* The object. */
	(void)data;
	zwp_tablet_v2_destroy(tablet);
}

/* The tool's kind: an eraser end, or a pen (any other drawing tool). */
static void
tool_type(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t type)
{
	struct keiui_tablet_tool *state;

	/* The eraser, or a pen. */
	(void)tool;
	state = data;
	state->type = KL_TABLET_PEN;
	if (type == ZWP_TABLET_TOOL_V2_TYPE_ERASER)
		state->type = KL_TABLET_ERASER;
}

/* The tool's hardware serial is not used. */
static void
tool_serial(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t high,
	uint32_t low)
{
	/* Nothing to do. */
	(void)data;
	(void)tool;
	(void)high;
	(void)low;
}

/* The tool's Wacom identity is not used. */
static void
tool_wacom(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t high,
	uint32_t low)
{
	/* Nothing to do. */
	(void)data;
	(void)tool;
	(void)high;
	(void)low;
}

/* What the tool reports: its pressure is kept as a capability. */
static void
tool_capability(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t capability)
{
	struct keiui_tablet_tool *state;

	/* The pressure. */
	(void)tool;
	state = data;
	if (capability == ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE)
		state->has_pressure = 1;
}

/* The tool's description is complete: nothing to do. */
static void
tool_done(
	void *data,
	struct zwp_tablet_tool_v2 *tool)
{
	/* Nothing to do. */
	(void)data;
	(void)tool;
}

/* The tool went away: a contact under way ends, and its state goes. */
static void
tool_removed(
	void *data,
	struct zwp_tablet_tool_v2 *tool)
{
	struct keiui_tablet_tool *state;
	struct kl_window *window;
	unsigned index;

	/* A contact still under way ends. */
	state = data;
	window = state->window;
	if (state->down)
		tool_input(state, KL_WINDOW_TABLET_UP, 0U);

	/* The tool leaves the table; the last tool takes its slot. */
	for (index = 0; index < window->tool_count; index++) {
		if (window->tools[index] == state)
			break;
	}
	if (index < window->tool_count) {
		window->tools[index] = window->tools[window->tool_count - 1U];
		window->tool_count--;
	}

	/* The object and the state go. */
	zwp_tablet_tool_v2_destroy(tool);
	free(state);
}

/* The tool came over the window: no cursor (the application shows where the pen is). */
static void
tool_proximity_in(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t serial,
	struct zwp_tablet_v2 *tablet,
	struct wl_surface *surface)
{
	struct keiui_tablet_tool *state;

	/* Near the window, not yet touching. */
	(void)tablet;
	(void)surface;
	state = data;
	state->near = 1;
	zwp_tablet_tool_v2_set_cursor(tool, serial, NULL, 0, 0);
}

/* The tool left the window: the frame that follows ends a contact and says so. */
static void
tool_proximity_out(
	void *data,
	struct zwp_tablet_tool_v2 *tool)
{
	struct keiui_tablet_tool *state;

	/* The tool is gone. */
	(void)tool;
	state = data;
	state->near = 0;
	state->leaving = 1;
	if (state->down)
		state->lifting = 1;
}

/* The tool touched: the frame starts a contact. */
static void
tool_down(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t serial)
{
	struct keiui_tablet_tool *state;

	/* The contact starts with the frame; its serial is the window's last input. */
	(void)tool;
	state = data;
	state->touching = 1;
	state->window->serial = serial;
	state->window->press_serial = serial;
}

/* The tool lifted: the frame ends the contact. */
static void
tool_up(
	void *data,
	struct zwp_tablet_tool_v2 *tool)
{
	struct keiui_tablet_tool *state;

	/* The contact ends with the frame. */
	(void)tool;
	state = data;
	if (state->down)
		state->lifting = 1;
	state->touching = 0;
}

/* The tool moved, in surface coordinates. */
static void
tool_motion(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct keiui_tablet_tool *state;

	/* The new place, and a move for the frame. */
	(void)tool;
	state = data;
	state->x = wl_fixed_to_double(x);
	state->y = wl_fixed_to_double(y);
	state->moved = 1;
}

/* The pressure, 0 to 65535. */
static void
tool_pressure(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t pressure)
{
	struct keiui_tablet_tool *state;

	/* Kept from 0 to 1, and a change for the frame. */
	(void)tool;
	state = data;
	state->pressure = (double)pressure / 65535.0;
	state->moved = 1;
}

/* The distance while hovering is not used. */
static void
tool_distance(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t distance)
{
	/* Nothing to do. */
	(void)data;
	(void)tool;
	(void)distance;
}

/* The tilt, in degrees. */
static void
tool_tilt(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	wl_fixed_t x,
	wl_fixed_t y)
{
	struct keiui_tablet_tool *state;

	/* Kept for the next sample. */
	(void)tool;
	state = data;
	state->tilt_x = wl_fixed_to_double(x);
	state->tilt_y = wl_fixed_to_double(y);
}

/* The rotation is not used. */
static void
tool_rotation(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	wl_fixed_t degrees)
{
	/* Nothing to do. */
	(void)data;
	(void)tool;
	(void)degrees;
}

/* The slider is not used. */
static void
tool_slider(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	int32_t position)
{
	/* Nothing to do. */
	(void)data;
	(void)tool;
	(void)position;
}

/* The wheel is not used. */
static void
tool_wheel(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	wl_fixed_t degrees,
	int32_t clicks)
{
	/* Nothing to do. */
	(void)data;
	(void)tool;
	(void)degrees;
	(void)clicks;
}

/* A barrel button held or released (the first and the second; the others are not used). */
static void
tool_button(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t serial,
	uint32_t button,
	uint32_t state_value)
{
	struct keiui_tablet_tool *state;
	unsigned bit;

	/* Which button. */
	(void)tool;
	(void)serial;
	state = data;
	if (button == TABLET_BUTTON_STYLUS) {
		bit = KL_TABLET_BUTTON_STYLUS;
	} else if (button == TABLET_BUTTON_STYLUS2) {
		bit = KL_TABLET_BUTTON_STYLUS2;
	} else {
		return;
	}

	/* Held or released. */
	state->buttons &= ~bit;
	if (state_value == ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED)
		state->buttons |= bit;
}

/* The end of a group of changes: they become the window's inputs. */
static void
tool_frame(
	void *data,
	struct zwp_tablet_tool_v2 *tool,
	uint32_t time)
{
	struct keiui_tablet_tool *state;

	/* A contact that starts, a move in contact, a move over the window without touching it. */
	(void)tool;
	state = data;
	if (state->touching && !state->down) {
		state->down = 1;
		tool_input(state, KL_WINDOW_TABLET_DOWN, time);
	} else if (state->down &&
	           state->moved &&
	           !state->lifting) {
		tool_input(state, KL_WINDOW_TABLET_MOTION, time);
	} else if (!state->down &&
	           state->near &&
	           state->moved) {
		tool_input(state, KL_WINDOW_TABLET_HOVER, time);
	}

	/* A lift, after the last move of the frame. */
	if (state->lifting) {
		if (state->moved)
			tool_input(state, KL_WINDOW_TABLET_MOTION, time);
		tool_input(state, KL_WINDOW_TABLET_UP, time);
		state->down = 0;
		state->lifting = 0;
	}

	/* A tool that left the window. */
	if (state->leaving) {
		tool_input(state, KL_WINDOW_TABLET_LEAVE, time);
		state->leaving = 0;
	}

	/* The frame's changes are taken. */
	state->moved = 0;
}

/* Queues one input of a tool at its place, with its kind, pressure, tilt and buttons, at the compositor's time. */
static void
tool_input(
	struct keiui_tablet_tool *state,
	unsigned kind,
	uint32_t time)
{
	struct kl_window_event *event;

	/* The input; a full queue drops it. */
	event = keiui_window_push(state->window, kind);
	if (event == NULL)
		return;
	event->x = state->x;
	event->y = state->y;
	event->tool = state->type;
	event->buttons = state->buttons;
	event->pressure = -1.0;
	if (state->has_pressure)
		event->pressure = state->pressure;
	event->tilt_x = state->tilt_x;
	event->tilt_y = state->tilt_y;
	if (time != 0U)
		keiui_window_stamp(event, time);
}
