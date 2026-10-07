/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pen tablets: zwp_tablet_manager_v2 version 1 and the pen as the pointer
 * (WS079 p003, plan/ws079/design-input-notes.md section 3).
 *
 * An evdev node with BTN_TOOL_PEN, ABS_PRESSURE and both position axes is a
 * tablet (input.c).  Each tablet is announced to every zwp_tablet_seat_v2 as
 * a zwp_tablet_v2; its two tools, the pen tip and the eraser end, are
 * announced as zwp_tablet_tool_v2 the first time each comes into proximity.
 * No pad is offered.
 *
 * The pen always moves the pointer: the whole tablet maps onto the whole
 * output.  The compositor's own grabs, screens and title bars see the pen first,
 * exactly as they see the pointer (seat.c's _shell functions); only what they
 * leave goes to a client.  A client with the tool gets the tablet protocol:
 * proximity_in and proximity_out as the pen enters and leaves its surface,
 * down and up with the touch, an implicit grab of the surface from down to
 * up, the pressure normalized to 0..65535 and the tilt in degrees.  Any other
 * client gets the pen as its pointer: the touch is BTN_LEFT and the barrel
 * buttons are BTN_RIGHT and BTN_MIDDLE.  Which of the two a touch goes to is
 * decided when it starts and holds until it ends.
 */

#include "kwl.h"
#include "tablet.h"
#include "extras.h"
#include "popup.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The pen tablets the compositor reads at once. */
#define TABLET_MAX		4U

/* The tools of one tablet: the pen tip and the eraser end. */
#define TABLET_TOOLS		2U
#define TOOL_PEN		0
#define TOOL_ERASER		1
#define TOOL_NONE		(-1)

/* How a touch in progress is delivered: not at all, by the tablet protocol, or as the pointer's left button. */
#define ROUTE_NONE		0
#define ROUTE_TABLET		1
#define ROUTE_POINTER		2

/* Event opcodes of zwp_tablet_seat_v2. */
#define SEAT_TABLET_ADDED	0U
#define SEAT_TOOL_ADDED		1U

/* Event opcodes of zwp_tablet_v2. */
#define TABLET_NAME		0U
#define TABLET_ID		1U
#define TABLET_PATH		2U
#define TABLET_DONE		3U
#define TABLET_REMOVED		4U

/* Event opcodes of zwp_tablet_tool_v2. */
#define TOOL_TYPE		0U
#define TOOL_CAPABILITY		3U
#define TOOL_DONE		4U
#define TOOL_REMOVED		5U
#define TOOL_PROXIMITY_IN	6U
#define TOOL_PROXIMITY_OUT	7U
#define TOOL_DOWN		8U
#define TOOL_UP			9U
#define TOOL_MOTION		10U
#define TOOL_PRESSURE		11U
#define TOOL_TILT		13U
#define TOOL_BUTTON		17U
#define TOOL_FRAME		18U

/* Request opcodes. */
#define MANAGER_GET_TABLET_SEAT	0U
#define MANAGER_DESTROY		1U
#define SEAT_DESTROY		0U
#define TABLET_DESTROY		0U
#define TOOL_SET_CURSOR		0U
#define TOOL_DESTROY		1U

/* zwp_tablet_tool_v2's capability values. */
#define CAPABILITY_TILT		1U
#define CAPABILITY_PRESSURE	2U

/* The pressure the protocol reports at full force. */
#define PRESSURE_FULL		65535U

/* The tilt the axis's ends mean when the device gives no resolution, in degrees. */
#define TILT_DEFAULT_DEGREES	60

/* The pointer buttons the pen's barrel buttons are for a client without the tablet. */
#define POINTER_BUTTON_RIGHT	0x111U
#define POINTER_BUTTON_MIDDLE	0x112U

/* The longest name or path a tablet reports, with its terminator. */
#define TABLET_TEXT_MAX		64U

/* The barrel buttons' bits in a tablet's button state. */
#define BARREL_FIRST		0x1U
#define BARREL_SECOND		0x2U

/*
 * One tool of a tablet.
 *
 * A tool is announced to the tablet seats the first time it comes into
 * proximity and stays until its tablet goes.
 */
struct tablet_tool {
	unsigned used;
};

/*
 * One pen tablet: its evdev node, what the node says about its axes, the
 * state its reports have left, and where the pen's events go.
 *
 * A slot is in use while input is set; it lives from kwl_tablet_add to
 * kwl_tablet_remove.  focus is cleared before the surface it names is freed
 * (kwl_tablet_object_gone).  The pressure and tilt last sent to the focus
 * are kept so an unchanged value is not sent again; axes_fresh says the
 * focus has just heard proximity_in and must hear every axis once.
 */
struct tablet_device {
	struct kwl_input_device *input;
	struct input_absinfo axis_x;
	struct input_absinfo axis_y;
	struct input_absinfo axis_pressure;
	struct input_absinfo axis_tilt_x;
	struct input_absinfo axis_tilt_y;
	unsigned has_tilt;
	char name[TABLET_TEXT_MAX];
	uint32_t vendor;
	uint32_t product;
	int32_t raw_x;
	int32_t raw_y;
	int32_t raw_pressure;
	int32_t raw_tilt_x;
	int32_t raw_tilt_y;
	unsigned pen_near;
	unsigned eraser_near;
	unsigned touch;
	unsigned barrels;
	int tool;
	int route;
	struct kwl_object *focus;
	unsigned axes_fresh;
	uint32_t sent_pressure;
	int32_t sent_tilt_x;
	int32_t sent_tilt_y;
	unsigned cursor_set;
	int32_t place_x;
	int32_t place_y;
	struct tablet_tool tools[TABLET_TOOLS];
};

/*
 * What one report changed.
 *
 * One instance lives on the stack while a report is applied.
 */
struct tablet_change {
	unsigned moved;
	unsigned pressure;
	unsigned tilt;
	unsigned barrels;
	unsigned touch;
};

/*
 * The tablets the compositor reads.
 *
 * A slot's input pointer says whether it is in use; the table lives as long
 * as the process, and the event loop is its only user.
 */
static struct tablet_device tablets[TABLET_MAX];

/*
 * The number the next zwp_tablet_seat_v2 is given.
 *
 * It only increases, so a tablet or tool object names the seat it was
 * announced on even after that seat's protocol ID is reused.
 */
static uint64_t tablet_seat_next = 1;

static struct tablet_device *device_of(struct kwl_input_device *input);
static unsigned device_slot(const struct tablet_device *device);
static unsigned tool_slot(const struct tablet_device *device, int tool);
static int read_axes(struct tablet_device *device);
static void read_report(struct tablet_device *device, struct tablet_change *change);
static void apply_report(struct kwl_server *server, struct tablet_device *device, const struct tablet_change *change, uint32_t time);
static void touch_press(struct kwl_server *server, struct tablet_device *device, uint32_t time, unsigned *sent, unsigned *pointer_activity);
static void touch_end(struct kwl_server *server, struct tablet_device *device, uint32_t time, unsigned *sent, unsigned *pointer_activity);
static void hover(struct kwl_server *server, struct tablet_device *device, uint32_t time, unsigned *sent, unsigned *pointer_activity);
static void barrel_changes(struct kwl_server *server, struct tablet_device *device, unsigned changed, uint32_t time, unsigned *sent, unsigned *pointer_activity);
static struct kwl_object *tablet_target(struct kwl_server *server, struct tablet_device *device);
static int client_has_tool(struct kwl_client *client, unsigned slot);
static void place_pointer(struct kwl_server *server, struct tablet_device *device);
static int32_t scale_fixed(int32_t value, int32_t minimum, int32_t maximum, uint32_t size);
static void focus_set(struct kwl_server *server, struct tablet_device *device, struct kwl_object *surface);
static void send_axes(struct tablet_device *device, unsigned motion, unsigned pressure, unsigned tilt);
static void send_tool(struct tablet_device *device, uint32_t opcode, const void *payload, size_t size);
static uint32_t normalized_pressure(const struct tablet_device *device);
static int32_t tilt_fixed(const struct input_absinfo *axis, int32_t value);
static void tool_use(struct kwl_server *server, struct tablet_device *device);
static void announce_tablet(struct kwl_object *seat, struct tablet_device *device);
static void announce_tool(struct kwl_object *seat, struct tablet_device *device, int tool);
static int seat_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static int tool_set_cursor(struct kwl_object *tool, const unsigned char *bytes, size_t size);
static void emit(struct kwl_client *client, uint32_t id, uint32_t opcode, const void *payload, size_t size);
static void emit_string(struct kwl_client *client, uint32_t id, uint32_t opcode, const char *text);

/*
 * Takes an evdev node classified as a pen tablet: reads its axes, name and
 * USB identity, and announces it to every tablet seat.
 */
int
kwl_tablet_add(
	struct kwl_server *server,
	struct kwl_input_device *input)
{
	struct tablet_device *device;
	struct kwl_client *client;
	struct kwl_object *object;
	unsigned index;
	int error;

	/* Finds a free tablet slot. */
	device = NULL;
	for (index = 0; index < TABLET_MAX; index++) {
		/* A slot without an input is free. */
		if (tablets[index].input == NULL) {
			device = &tablets[index];
			break;
		}
	}

	/* No more tablets can be read. */
	if (device == NULL)
		return ENOSPC;

	/* The slot starts empty: no tool near, nothing delivered. */
	memset(device, 0, sizeof(*device));
	device->input = input;
	device->tool = TOOL_NONE;
	device->route = ROUTE_NONE;

	/* The axes the pen reports, its name and identity. */
	error = read_axes(device);
	if (error != 0) {
		device->input = NULL;
		return error;
	}

	/* Every tablet seat hears about the new tablet. */
	for (client = server->clients; client != NULL; client = client->next) {
		/* Each live tablet seat of the client. */
		for (object = client->objects; object != NULL; object = object->next) {
			/* Only tablet seats announce tablets. */
			if (object->kind != KWL_TABLET_SEAT || object->dead)
				continue;

			/* The seat gets a zwp_tablet_v2 for it. */
			announce_tablet(object, device);
		}
	}

	/* One line lets a test see the tablet's axes. */
	printf("KWL TABLET added device=%s name=\"%s\" x=%d..%d y=%d..%d pressure=%d..%d tilt=%u\n", input->path, device->name, device->axis_x.minimum, device->axis_x.maximum, device->axis_y.minimum, device->axis_y.maximum, device->axis_pressure.minimum, device->axis_pressure.maximum, device->has_tilt);

	/* Succeeded: the tablet's reports are applied from now on. */
	return 0;
}

/*
 * Forgets a pen tablet whose node is closing; with notify, its tool leaves
 * its surface and every tablet and tool object of it hears removed.
 */
void
kwl_tablet_remove(
	struct kwl_server *server,
	struct kwl_input_device *input,
	int notify)
{
	struct tablet_device *device;
	struct kwl_client *client;
	struct kwl_object *object;
	unsigned slot;
	unsigned first_tool;
	unsigned sent;
	unsigned pointer_activity;

	/* A node that is not a known tablet has nothing to forget. */
	device = device_of(input);
	if (device == NULL)
		return;

	/* The pen stops what it was doing: a touch ends, the tool leaves its surface. */
	if (notify) {
		sent = 0;
		pointer_activity = 0;
		touch_end(server, device, 0, &sent, &pointer_activity);
		focus_set(server, device, NULL);
		if (pointer_activity)
			kwl_seat_frame(server);
	}

	/* Every object of this tablet and its tools hears removed and names nothing any more. */
	slot = device_slot(device);
	first_tool = tool_slot(device, TOOL_PEN);
	for (client = server->clients; client != NULL; client = client->next) {
		/* Each live object of the client. */
		for (object = client->objects; object != NULL; object = object->next) {
			/* Retired objects hear nothing. */
			if (object->dead)
				continue;

			/* A tool of this tablet. */
			if (object->kind == KWL_TABLET_TOOL &&
			    object->tool_slot >= first_tool &&
			    object->tool_slot < first_tool + TABLET_TOOLS) {
				if (notify)
					emit(client, object->id, TOOL_REMOVED, NULL, 0);
				object->tool_slot = KWL_TABLET_SLOT_NONE;
			}

			/* The tablet itself. */
			if (object->kind == KWL_TABLET && object->tablet_slot == slot) {
				if (notify)
					emit(client, object->id, TABLET_REMOVED, NULL, 0);
				object->tablet_slot = KWL_TABLET_SLOT_NONE;
			}
		}
	}

	/* The slot is free. */
	printf("KWL TABLET removed device=%s\n", input->path);
	device->input = NULL;
	device->focus = NULL;
}

/*
 * Applies one completed report of a pen tablet.
 */
void
kwl_tablet_frame(
	struct kwl_server *server,
	struct kwl_input_device *input,
	uint32_t time)
{
	struct tablet_device *device;
	struct tablet_change change;

	/* A node that is not a known tablet is ignored. */
	device = device_of(input);
	if (device == NULL)
		return;

	/* The pen is input: the lock screen's idle time starts again. */
	server->lock_input_ms = kwl_milliseconds();

	/* Reads what the report changed, then delivers it. */
	read_report(device, &change);
	apply_report(server, device, &change, time);
}

/*
 * Carries out a request of zwp_tablet_manager_v2, zwp_tablet_seat_v2,
 * zwp_tablet_v2 or zwp_tablet_tool_v2.
 */
int
kwl_tablet_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* The manager makes a seat's tablet seat, or goes. */
	if (object->kind == KWL_TABLET_MANAGER) {
		/* destroy: what it made stays. */
		if (opcode == MANAGER_DESTROY && size == 0U) {
			kwl_object_destroy(object);
			return 0;
		}

		/* Only get_tablet_seat is left. */
		if (opcode != MANAGER_GET_TABLET_SEAT)
			return EPROTO;
		error = seat_create(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* A tool names its cursor, or goes. */
	if (object->kind == KWL_TABLET_TOOL) {
		/* set_cursor. */
		if (opcode == TOOL_SET_CURSOR) {
			error = tool_set_cursor(object, bytes, size);
			if (error != 0)
				return error;
			return 0;
		}

		/* Only destroy is left. */
		if (opcode != TOOL_DESTROY || size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* A tablet seat and a tablet only go (their destroy is opcode 0). */
	if (opcode != SEAT_DESTROY || size != 0U)
		return EPROTO;
	kwl_object_destroy(object);

	/* Succeeded: the request was carried out. */
	return 0;
}

/*
 * Stops naming a surface that is going: a tool in proximity of it leaves,
 * and a touch delivered to it ends without an up.
 */
void
kwl_tablet_object_gone(
	struct kwl_object *object)
{
	struct kwl_server *server;
	unsigned index;

	/* Only surfaces are named by the tablets. */
	if (object->kind != KWL_SURFACE)
		return;

	/* Every tablet whose tool is on the surface. */
	server = object->client->server;
	for (index = 0; index < TABLET_MAX; index++) {
		/* A free slot or another surface is left alone. */
		if (tablets[index].input == NULL || tablets[index].focus != object)
			continue;

		/* The tool leaves (proximity_out names no surface), and its touch is lost. */
		focus_set(server, &tablets[index], NULL);
		if (tablets[index].route == ROUTE_TABLET)
			tablets[index].route = ROUTE_NONE;
	}
}

/* Finds the tablet slot of an input device, NULL when there is none. */
static struct tablet_device *
device_of(
	struct kwl_input_device *input)
{
	unsigned index;

	/* Compares every slot in use. */
	for (index = 0; index < TABLET_MAX; index++) {
		/* The slot reading this node. */
		if (tablets[index].input == input)
			return &tablets[index];
	}

	/* The node is not a known tablet. */
	return NULL;
}

/* Reports a tablet's slot number. */
static unsigned
device_slot(
	const struct tablet_device *device)
{
	/* Succeeded: its index in the table. */
	return (unsigned)(device - tablets);
}

/* Reports the number a tool of a tablet is named by in tool objects. */
static unsigned
tool_slot(
	const struct tablet_device *device,
	int tool)
{
	unsigned slot;

	/* Two tools per tablet, the pen first. */
	slot = device_slot(device) * TABLET_TOOLS + (unsigned)tool;

	/* Reports the tool's number. */
	return slot;
}

/* Reads a tablet's axes, name and USB identity from its node. */
static int
read_axes(
	struct tablet_device *device)
{
	struct input_id identity;
	int descriptor;
	int error;

	/* The horizontal and vertical ranges were checked by the classification. */
	descriptor = device->input->fd;
	error = kl_backend_input_absinfo(descriptor, ABS_X, &device->axis_x);
	if (error != 0)
		return error;

	/* Reads the vertical range of the pen. */
	error = kl_backend_input_absinfo(descriptor, ABS_Y, &device->axis_y);
	if (error != 0)
		return error;

	/* The pressure's range. */
	error = kl_backend_input_absinfo(descriptor, ABS_PRESSURE, &device->axis_pressure);
	if (error != 0)
		return error;

	/* The tilt is optional: both axes, or none. */
	device->has_tilt = 0;
	error = kl_backend_input_absinfo(descriptor, ABS_TILT_X, &device->axis_tilt_x);
	if (error == 0) {
		/* Keeps tilt only when the second axis also supplies a range. */
		error = kl_backend_input_absinfo(descriptor, ABS_TILT_Y, &device->axis_tilt_y);
		if (error == 0 && device->axis_tilt_x.maximum > device->axis_tilt_x.minimum)
			device->has_tilt = 1;
	}

	/* The pen starts where the node says it is. */
	device->raw_x = device->axis_x.value;
	device->raw_y = device->axis_y.value;
	device->raw_pressure = device->axis_pressure.minimum;

	/* The name, empty when the node gives none. */
	memset(device->name, 0, sizeof(device->name));
	error = kl_backend_input_name(descriptor, device->name, sizeof(device->name) - 1U);
	if (error != 0)
		device->name[0] = '\0';

	/* The USB vendor and product, when the node is a USB device. */
	memset(&identity, 0, sizeof(identity));
	error = kl_backend_input_id(descriptor, &identity);
	if (error == 0 && identity.bustype == BUS_USB) {
		device->vendor = identity.vendor;
		device->product = identity.product;
	}

	/* Succeeded: the tablet's axes are known. */
	return 0;
}

/* Takes one report's events into the tablet's state and notes what changed. */
static void
read_report(
	struct tablet_device *device,
	struct tablet_change *change)
{
	const struct input_event *event;
	unsigned barrels;
	unsigned touch;
	unsigned pressed;
	unsigned index;

	/* Nothing has changed yet. */
	memset(change, 0, sizeof(*change));
	barrels = device->barrels;
	touch = device->touch;

	/* Each event of the report, in order. */
	for (index = 0; index < device->input->frame_count; index++) {
		event = &device->input->frame[index];

		/* An axis takes its new value. */
		if (event->type == EV_ABS) {
			/* Each axis the pen reports. */
			switch (event->code) {
			case ABS_X:
				device->raw_x = event->value;
				change->moved = 1;
				break;
			case ABS_Y:
				device->raw_y = event->value;
				change->moved = 1;
				break;
			case ABS_PRESSURE:
				device->raw_pressure = event->value;
				change->pressure = 1;
				break;
			case ABS_TILT_X:
				device->raw_tilt_x = event->value;
				change->tilt = 1;
				break;
			case ABS_TILT_Y:
				device->raw_tilt_y = event->value;
				change->tilt = 1;
				break;
			default:
				break;
			}

			continue;
		}

		/* Only keys are left that matter. */
		if (event->type != EV_KEY)
			continue;

		/* A key's new state: pressed (1) or released (0). */
		pressed = 0;
		if (event->value != 0)
			pressed = 1;

		/* A tool's proximity, the touch, or a barrel button. */
		switch (event->code) {
		case BTN_TOOL_PEN:
			device->pen_near = pressed;
			break;
		case BTN_TOOL_RUBBER:
			device->eraser_near = pressed;
			break;
		case BTN_TOUCH:
			device->touch = pressed;
			break;
		case BTN_STYLUS:
			/* The first barrel button's bit. */
			if (pressed) {
				device->barrels |= BARREL_FIRST;
			} else {
				device->barrels &= ~BARREL_FIRST;
			}

			break;
		case BTN_STYLUS2:
			/* The second barrel button's bit. */
			if (pressed) {
				device->barrels |= BARREL_SECOND;
			} else {
				device->barrels &= ~BARREL_SECOND;
			}

			break;
		default:
			break;
		}
	}

	/* The buttons and the touch that differ from before the report. */
	change->barrels = barrels ^ device->barrels;
	if (touch != device->touch)
		change->touch = 1;
}

/*
 * Delivers one report: a tool that left or changed ends what it did, then
 * the pen moves the pointer, and its axes, buttons and touch go where the
 * touch in progress, or the surface under the pen, says.
 */
static void
apply_report(
	struct kwl_server *server,
	struct tablet_device *device,
	const struct tablet_change *change,
	uint32_t time)
{
	unsigned sent;
	unsigned pointer_activity;
	unsigned entering;
	int tool;

	/* The tool now near: the eraser end wins when both are reported. */
	tool = TOOL_NONE;
	if (device->pen_near)
		tool = TOOL_PEN;
	if (device->eraser_near)
		tool = TOOL_ERASER;

	/* A tool that went away or was replaced ends its touch and leaves its surface. */
	sent = 0;
	pointer_activity = 0;
	if (device->tool != TOOL_NONE && tool != device->tool) {
		/* The up and the proximity_out share the frame focus_set ends with. */
		touch_end(server, device, time, &sent, &pointer_activity);
		focus_set(server, device, NULL);
		device->tool = TOOL_NONE;
		sent = 0;
	}

	/* Without a tool near there is nothing more to deliver. */
	if (tool == TOOL_NONE) {
		if (pointer_activity)
			kwl_seat_frame(server);
		return;
	}

	/* A tool coming near is announced to the tablet seats the first time. */
	entering = 0;
	if (device->tool == TOOL_NONE) {
		device->tool = tool;
		tool_use(server, device);
		entering = 1;
	}

	/* The pen's place moves the pointer. */
	if (change->moved || entering)
		place_pointer(server, device);

	/* A touch delivered by the tablet protocol goes on to its surface (the implicit grab). */
	if (device->route == ROUTE_TABLET) {
		/* The axes and buttons, then the up that ends the touch. */
		send_axes(device, change->moved, change->pressure, change->tilt);
		sent = 1;
		barrel_changes(server, device, change->barrels, time, &sent, &pointer_activity);
		if (!device->touch)
			touch_end(server, device, time, &sent, &pointer_activity);
	} else if (device->route == ROUTE_POINTER) {
		/* A touch delivered as the pointer's left button moves and clicks like the pointer. */
		if (change->moved) {
			kwl_seat_motion(server, time);
			pointer_activity = 1;
		}

		/* The barrel buttons, then the release that ends the touch. */
		barrel_changes(server, device, change->barrels, time, &sent, &pointer_activity);
		if (!device->touch)
			touch_end(server, device, time, &sent, &pointer_activity);
	} else {
		/* Otherwise the pen hovers: the surface under it hears it, then its buttons and a new touch. */
		if (change->moved || entering) {
			hover(server, device, time, &sent, &pointer_activity);
		} else if (device->focus != NULL &&
		           (change->pressure ||
		            change->tilt)) {
			send_axes(device, 0, change->pressure, change->tilt);
			sent = 1;
		}

		/* The barrel buttons, then a touch that starts. */
		barrel_changes(server, device, change->barrels, time, &sent, &pointer_activity);
		if (device->touch && change->touch)
			touch_press(server, device, time, &sent, &pointer_activity);
	}

	/* A touch that ended may leave the pen over another surface. */
	if (device->route == ROUTE_NONE &&
	    change->touch &&
	    !device->touch)
		hover(server, device, time, &sent, &pointer_activity);

	/* The tool's events of this report end with a frame; the pointer's with its own. */
	if (sent && device->focus != NULL)
		send_tool(device, TOOL_FRAME, &time, sizeof(time));
	if (pointer_activity)
		kwl_seat_frame(server);
}

/*
 * Starts a touch: the compositor's own UI takes it first as BTN_LEFT; otherwise a
 * client with the tool hears down, and any other client the left button.
 */
static void
touch_press(
	struct kwl_server *server,
	struct tablet_device *device,
	uint32_t time,
	unsigned *sent,
	unsigned *pointer_activity)
{
	struct kwl_object *target;
	uint32_t serial;
	int taken;

	/* The compositor's grabs, screens and title bars see the press first. */
	taken = kwl_seat_button_shell(server, time, KWL_BUTTON_LEFT, 1U);
	if (taken) {
		focus_set(server, device, NULL);
		device->route = ROUTE_POINTER;
		return;
	}

	/* A client with the tool hears down on its surface, which keeps the pen until up. */
	target = tablet_target(server, device);
	if (target != NULL) {
		/* The tool comes over the surface first when it was not on it. */
		if (device->focus != target) {
			focus_set(server, device, target);
			send_axes(device, 1, 1, 1);
		}

		/* down, whose serial a move or a resize the client asks for names. */
		serial = kwl_next_serial(server);
		send_tool(device, TOOL_DOWN, &serial, sizeof(serial));
		server->press_serial = serial;
		device->route = ROUTE_TABLET;
		*sent = 1;
		return;
	}

	/* Any other client hears the pointer's left button. */
	focus_set(server, device, NULL);
	kwl_seat_button_deliver(server, time, KWL_BUTTON_LEFT, 1U);
	device->route = ROUTE_POINTER;
	*pointer_activity = 1;
}

/* Ends a touch in progress the way it was delivered. */
static void
touch_end(
	struct kwl_server *server,
	struct tablet_device *device,
	uint32_t time,
	unsigned *sent,
	unsigned *pointer_activity)
{
	/* A touch by the tablet protocol hears up; the left button is no longer held. */
	if (device->route == ROUTE_TABLET) {
		/* A surface that went during the touch hears nothing. */
		if (device->focus != NULL) {
			send_tool(device, TOOL_UP, NULL, 0);
			*sent = 1;
		}

		/* BTN_LEFT's bit of the held buttons, which the press set (seat.c), and the route end. */
		server->buttons_down &= ~1U;
		device->route = ROUTE_NONE;
		return;
	}

	/* A touch as the pointer releases its left button, through the compositor first. */
	if (device->route == ROUTE_POINTER) {
		kwl_seat_button(server, time, KWL_BUTTON_LEFT, 0U);
		device->route = ROUTE_NONE;
		*pointer_activity = 1;
	}
}

/*
 * Delivers a hovering pen's place: to the compositor's grabs and screens first,
 * then to the surface under it by the tablet protocol, or as the pointer's
 * motion to a client without the tool.
 */
static void
hover(
	struct kwl_server *server,
	struct tablet_device *device,
	uint32_t time,
	unsigned *sent,
	unsigned *pointer_activity)
{
	struct kwl_object *target;
	struct kwl_object *surface;
	int taken;
	int bound;

	/* The compositor's grabs and screens take the motion first; the tool leaves any surface. */
	taken = kwl_seat_motion_shell(server, time);
	if (taken) {
		focus_set(server, device, NULL);
		return;
	}

	/* A client with the tool: proximity (again when the surface changed) and the axes. */
	target = tablet_target(server, device);
	if (target != NULL) {
		/* A new surface hears proximity_in and every axis; the same one hears the place. */
		if (device->focus != target) {
			focus_set(server, device, target);
			send_axes(device, 1, 1, 1);
		} else {
			send_axes(device, 1, 0, 0);
		}

		/* The report ends with a frame. */
		*sent = 1;
		return;
	}

	/* No surface of a client with the tool is under the pen. */
	focus_set(server, device, NULL);

	/* A client without the tool hears the pointer's motion; one with it hears nothing off its surface. */
	surface = server->pointer_surface;
	if (surface == NULL)
		return;
	bound = client_has_tool(surface->client, tool_slot(device, device->tool));
	if (bound)
		return;
	kwl_seat_motion_deliver(server, time);
	*pointer_activity = 1;
}

/*
 * Delivers the barrel buttons that changed: as tool buttons to a surface
 * with the tool, otherwise as the pointer's right and middle buttons.
 */
static void
barrel_changes(
	struct kwl_server *server,
	struct tablet_device *device,
	unsigned changed,
	uint32_t time,
	unsigned *sent,
	unsigned *pointer_activity)
{
	static const unsigned bits[2] = { BARREL_FIRST, BARREL_SECOND };
	static const uint32_t tool_codes[2] = { BTN_STYLUS, BTN_STYLUS2 };
	static const uint32_t pointer_codes[2] = { POINTER_BUTTON_RIGHT, POINTER_BUTTON_MIDDLE };
	uint32_t words[3];
	uint32_t state;
	unsigned index;

	/* Each of the two barrel buttons. */
	for (index = 0; index < 2U; index++) {
		/* An unchanged button says nothing. */
		if ((changed & bits[index]) == 0U)
			continue;

		/* Its new state. */
		state = 0;
		if ((device->barrels & bits[index]) != 0U)
			state = 1;

		/* The tool's button, with the Linux BTN_ code, to a surface with the tool. */
		if (device->focus != NULL) {
			words[0] = kwl_next_serial(server);
			words[1] = tool_codes[index];
			words[2] = state;
			send_tool(device, TOOL_BUTTON, words, sizeof(words));
			*sent = 1;
			continue;
		}

		/* Otherwise the pointer's right or middle button. */
		kwl_seat_button(server, time, pointer_codes[index], state);
		*pointer_activity = 1;
	}
}

/*
 * Finds the surface the pen's tablet events go to: the surface under the
 * pointer, when the pen is on it and its client has the tool.
 */
static struct kwl_object *
tablet_target(
	struct kwl_server *server,
	struct tablet_device *device)
{
	struct kwl_object *surface;
	uint32_t width;
	uint32_t height;
	int bound;

	/* The surface the pointer belongs to now (seat.c). */
	kwl_seat_pointer_update(server);
	surface = server->pointer_surface;
	if (surface == NULL || surface->dead)
		return NULL;

	/* Its client must have this tool. */
	bound = client_has_tool(surface->client, tool_slot(device, device->tool));
	if (!bound)
		return NULL;

	/* The pen must be on the surface. */
	kwl_surface_size(surface, &width, &height);
	if (server->pointer_x < surface->x || server->pointer_y < surface->y)
		return NULL;
	if (server->pointer_x >= surface->x + (int32_t)width)
		return NULL;
	if (server->pointer_y >= surface->y + (int32_t)height)
		return NULL;

	/* Succeeded: the surface hears the tablet. */
	return surface;
}

/* Reports whether a client has a live object for a tool. */
static int
client_has_tool(
	struct kwl_client *client,
	unsigned slot)
{
	struct kwl_object *object;

	/* A failed client has nothing. */
	if (client->fatal)
		return 0;

	/* Looks for the tool among the client's objects. */
	for (object = client->objects; object != NULL; object = object->next) {
		/* A live object of the tool. */
		if (object->kind == KWL_TABLET_TOOL &&
		    !object->dead &&
		    object->tool_slot == slot)
			return 1;
	}

	/* The client does not have the tool. */
	return 0;
}

/* Moves the pointer to the pen's place, mapping the whole tablet onto the output. */
static void
place_pointer(
	struct kwl_server *server,
	struct tablet_device *device)
{
	int32_t old_x;
	int32_t old_y;

	/* The pen's place on the output in 24.8 fixed point. */
	device->place_x = scale_fixed(device->raw_x, device->axis_x.minimum, device->axis_x.maximum, server->width);
	device->place_y = scale_fixed(device->raw_y, device->axis_y.minimum, device->axis_y.maximum, server->height);

	/* The pointer follows in whole pixels on the anchor (ws113-p007); the cursor is redrawn where it was and is (damage.c). */
	kwl_pointer_absolute(server);
	old_x = server->pointer_x;
	old_y = server->pointer_y;
	server->pointer_x = device->place_x / 256;
	server->pointer_y = device->place_y / 256;
	if (old_x != server->pointer_x || old_y != server->pointer_y)
		kwl_damage_pointer(server, old_x, old_y);
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

/*
 * Moves the tool's proximity to a surface, or to none: the tool objects of
 * the old surface's client hear proximity_out, those of the new one's
 * proximity_in with their tablet.
 */
static void
focus_set(
	struct kwl_server *server,
	struct tablet_device *device,
	struct kwl_object *surface)
{
	struct kwl_object *object;
	struct kwl_object *tablet;
	uint32_t words[3];
	unsigned slot;
	unsigned device_number;

	/* An unchanged surface hears nothing. */
	if (device->focus == surface)
		return;

	/* The old surface's tools hear proximity_out and a frame; a cursor the tool set gives way to the arrow. */
	if (device->focus != NULL) {
		send_tool(device, TOOL_PROXIMITY_OUT, NULL, 0);
		words[0] = (uint32_t)kwl_milliseconds();
		send_tool(device, TOOL_FRAME, words, sizeof(words[0]));
		if (device->cursor_set) {
			kwl_cursor_default(server);
			device->cursor_set = 0;
		}
	}

	/* focus names the surface whose tool objects were told proximity_in; it hears every axis next. */
	device->focus = surface;
	device->axes_fresh = 1;
	if (surface == NULL)
		return;

	/* Each tool object of the new client hears proximity_in with its seat's tablet. */
	words[0] = kwl_next_serial(server);
	words[2] = surface->id;
	slot = tool_slot(device, device->tool);
	device_number = device_slot(device);
	for (object = surface->client->objects; object != NULL; object = object->next) {
		/* Only live objects of this tool. */
		if (object->kind != KWL_TABLET_TOOL ||
		    object->dead ||
		    object->tool_slot != slot)
			continue;

		/* The zwp_tablet_v2 announced on the same seat. */
		for (tablet = surface->client->objects; tablet != NULL; tablet = tablet->next) {
			/* The tablet of this device on the tool's seat. */
			if (tablet->kind == KWL_TABLET &&
			    !tablet->dead &&
			    tablet->tablet_slot == device_number &&
			    tablet->tablet_seat_number == object->tablet_seat_number)
				break;
		}

		/* A tool without its tablet cannot name one. */
		if (tablet == NULL)
			continue;

		/* proximity_in: the serial, the tablet and the surface. */
		words[1] = tablet->id;
		emit(object->client, object->id, TOOL_PROXIMITY_IN, words, sizeof(words));
	}
}

/* Sends the pen's place, pressure and tilt to the tool objects on the focus. */
static void
send_axes(
	struct tablet_device *device,
	unsigned motion,
	unsigned pressure,
	unsigned tilt)
{
	int32_t words[2];
	uint32_t value;
	unsigned fresh;

	/* Nobody hears without a focus. */
	if (device->focus == NULL)
		return;

	/* A focus that has just heard proximity_in hears every axis once. */
	fresh = device->axes_fresh;
	device->axes_fresh = 0;

	/* The surface-local place in 24.8 fixed point. */
	if (motion || fresh) {
		words[0] = device->place_x - device->focus->x * 256;
		words[1] = device->place_y - device->focus->y * 256;
		send_tool(device, TOOL_MOTION, words, sizeof(words));

		/* The time the place went out, when the per-frame lines were asked for (ws099-p015's pen latency). */
		if (device->focus->client->server->log_frames)
			printf("KWL LAT pen surface=%u at_us=%llu\n", device->focus->id, (unsigned long long)kwl_microseconds());
	}

	/* The pressure, 0..65535, when it differs from what was sent. */
	if (pressure || fresh) {
		value = normalized_pressure(device);
		if (fresh || value != device->sent_pressure) {
			send_tool(device, TOOL_PRESSURE, &value, sizeof(value));
			device->sent_pressure = value;
		}
	}

	/* The tilt in degrees as wl_fixed, when it differs from what was sent. */
	if ((tilt ||
	     fresh) &&
	    device->has_tilt) {
		words[0] = tilt_fixed(&device->axis_tilt_x, device->raw_tilt_x);
		words[1] = tilt_fixed(&device->axis_tilt_y, device->raw_tilt_y);
		if (fresh ||
		    words[0] != device->sent_tilt_x ||
		    words[1] != device->sent_tilt_y) {
			send_tool(device, TOOL_TILT, words, sizeof(words));
			device->sent_tilt_x = words[0];
			device->sent_tilt_y = words[1];
		}
	}
}

/* Sends one event to every tool object of the tool in use on the focus. */
static void
send_tool(
	struct tablet_device *device,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	struct kwl_object *object;
	unsigned slot;

	/* Nobody hears without a focus. */
	if (device->focus == NULL || device->tool == TOOL_NONE)
		return;

	/* Each live object of the tool in the focus's client. */
	slot = tool_slot(device, device->tool);
	for (object = device->focus->client->objects; object != NULL; object = object->next) {
		/* Only live objects of this tool. */
		if (object->kind != KWL_TABLET_TOOL ||
		    object->dead ||
		    object->tool_slot != slot)
			continue;

		/* Queues the event. */
		emit(object->client, object->id, opcode, payload, size);
	}
}

/* Maps the raw pressure onto 0..65535. */
static uint32_t
normalized_pressure(
	const struct tablet_device *device)
{
	int64_t offset;
	int64_t range;

	/* An empty range, or a value at or below its low end, is no pressure. */
	range = (int64_t)device->axis_pressure.maximum - device->axis_pressure.minimum;
	offset = (int64_t)device->raw_pressure - device->axis_pressure.minimum;
	if (range <= 0 || offset <= 0)
		return 0;

	/* A value at or above the high end is full pressure. */
	if (offset >= range)
		return PRESSURE_FULL;

	/* Succeeded: the pressure scaled linearly. */
	return (uint32_t)((offset * PRESSURE_FULL) / range);
}

/*
 * Converts one raw tilt value to degrees as wl_fixed: by the axis's
 * resolution (units per radian) when it has one, otherwise by mapping its
 * range onto -60..+60 degrees.
 */
static int32_t
tilt_fixed(
	const struct input_absinfo *axis,
	int32_t value)
{
	double degrees;
	double range;

	/* With a resolution the value converts exactly. */
	if (axis->resolution > 0) {
		degrees = (double)value * 180.0 / (3.14159265358979323846 * (double)axis->resolution);
	} else {
		/* Without one the range's ends are the default angles. */
		range = (double)axis->maximum - (double)axis->minimum;
		degrees = -TILT_DEFAULT_DEGREES + ((double)value - (double)axis->minimum) * (2.0 * TILT_DEFAULT_DEGREES) / range;
	}

	/* Rounds to the nearest 1/256 degree, away from zero at the half. */
	if (degrees < 0.0)
		return (int32_t)(degrees * 256.0 - 0.5);

	/* Succeeded: the tilt as wl_fixed. */
	return (int32_t)(degrees * 256.0 + 0.5);
}

/* Announces a tool coming near for the first time to every tablet seat. */
static void
tool_use(
	struct kwl_server *server,
	struct tablet_device *device)
{
	struct kwl_client *client;
	struct kwl_object *object;

	/* A tool already announced is known to every seat. */
	if (device->tools[device->tool].used)
		return;
	device->tools[device->tool].used = 1;

	/* Every live tablet seat gets a zwp_tablet_tool_v2 for it. */
	for (client = server->clients; client != NULL; client = client->next) {
		/* Each live tablet seat of the client. */
		for (object = client->objects; object != NULL; object = object->next) {
			/* Only tablet seats announce tools. */
			if (object->kind != KWL_TABLET_SEAT || object->dead)
				continue;

			/* The seat hears tool_added and the tool's description. */
			announce_tool(object, device, device->tool);
		}
	}
}

/*
 * Makes a zwp_tablet_v2 for a tablet on one tablet seat: tablet_added, then
 * the name, the USB identity when known, the path and done.
 */
static void
announce_tablet(
	struct kwl_object *seat,
	struct tablet_device *device)
{
	struct kwl_object *tablet;
	uint32_t words[2];

	/* The server-made object the event carries. */
	tablet = kwl_create_server(seat->client, KWL_TABLET, seat->version);
	if (tablet == NULL) {
		seat->client->fatal = 1;
		seat->client->fatal_time = kwl_milliseconds();
		return;
	}

	/* It names its seat and its tablet, and no tool. */
	tablet->tablet_seat_number = seat->tablet_seat_number;
	tablet->tablet_slot = device_slot(device);
	tablet->tool_slot = KWL_TABLET_SLOT_NONE;

	/* tablet_added introduces it. */
	words[0] = tablet->id;
	emit(seat->client, seat->id, SEAT_TABLET_ADDED, words, sizeof(words[0]));

	/* Its name, then its USB identity when it is a USB device. */
	emit_string(seat->client, tablet->id, TABLET_NAME, device->name);
	if (device->vendor != 0U || device->product != 0U) {
		words[0] = device->vendor;
		words[1] = device->product;
		emit(seat->client, tablet->id, TABLET_ID, words, sizeof(words));
	}

	/* Its node's path, and the end of the description. */
	emit_string(seat->client, tablet->id, TABLET_PATH, device->input->path);
	emit(seat->client, tablet->id, TABLET_DONE, NULL, 0);
}

/*
 * Makes a zwp_tablet_tool_v2 for a tool on one tablet seat: tool_added,
 * then its type, its capabilities and done.
 */
static void
announce_tool(
	struct kwl_object *seat,
	struct tablet_device *device,
	int tool)
{
	struct kwl_object *object;
	uint32_t word;

	/* The server-made object the event carries. */
	object = kwl_create_server(seat->client, KWL_TABLET_TOOL, seat->version);
	if (object == NULL) {
		seat->client->fatal = 1;
		seat->client->fatal_time = kwl_milliseconds();
		return;
	}

	/* It names its seat, its tablet and its tool. */
	object->tablet_seat_number = seat->tablet_seat_number;
	object->tablet_slot = device_slot(device);
	object->tool_slot = tool_slot(device, tool);

	/* tool_added introduces it. */
	word = object->id;
	emit(seat->client, seat->id, SEAT_TOOL_ADDED, &word, sizeof(word));

	/* The type is the evdev tool code: pen 0x140, eraser 0x141. */
	word = BTN_TOOL_PEN;
	if (tool == TOOL_ERASER)
		word = BTN_TOOL_RUBBER;
	emit(seat->client, object->id, TOOL_TYPE, &word, sizeof(word));

	/* Every pen tablet reports pressure; the tilt when the device has it. */
	word = CAPABILITY_PRESSURE;
	emit(seat->client, object->id, TOOL_CAPABILITY, &word, sizeof(word));
	if (device->has_tilt) {
		word = CAPABILITY_TILT;
		emit(seat->client, object->id, TOOL_CAPABILITY, &word, sizeof(word));
	}

	/* The description is complete. */
	emit(seat->client, object->id, TOOL_DONE, NULL, 0);
}

/*
 * Carries out get_tablet_seat: a new zwp_tablet_seat_v2 hears every tablet
 * and every tool already used.
 */
static int
seat_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *seat;
	struct kwl_object *wl_seat;
	uint32_t words[2];
	unsigned index;
	int tool;

	/* The new ID and the wl_seat. */
	if (size != sizeof(words))
		return EPROTO;
	memcpy(words, bytes, sizeof(words));

	/* The wl_seat must be one of the client's seats. */
	wl_seat = kwl_find(manager->client, words[1]);
	if (wl_seat == NULL || wl_seat->kind != KWL_SEAT)
		return EPROTO;

	/* The tablet seat, with a number its tablets and tools will name. */
	seat = kwl_create(manager->client, words[0], KWL_TABLET_SEAT, manager->version);
	if (seat == NULL)
		return EPROTO;
	seat->tablet_seat_number = tablet_seat_next;
	tablet_seat_next++;
	seat->tablet_slot = KWL_TABLET_SLOT_NONE;
	seat->tool_slot = KWL_TABLET_SLOT_NONE;

	/* Every tablet, and each of its tools already used. */
	for (index = 0; index < TABLET_MAX; index++) {
		/* A free slot has nothing to announce. */
		if (tablets[index].input == NULL)
			continue;

		/* The tablet first, then its tools. */
		announce_tablet(seat, &tablets[index]);
		for (tool = TOOL_PEN; tool <= TOOL_ERASER; tool++) {
			/* A tool never near is announced when it comes. */
			if (tablets[index].tools[tool].used)
				announce_tool(seat, &tablets[index], tool);
		}
	}

	/* One line lets a test see who took the tablets. */
	printf("KWL TABLET seat client=%llu\n", (unsigned long long)manager->client->number);

	/* Succeeded: the client owns the tablet seat. */
	return 0;
}

/*
 * Carries out a tool's set_cursor: while the tool is on one of the
 * client's surfaces, a surface of the client's (or none, hiding the cursor)
 * becomes the cursor.
 */
static int
tool_set_cursor(
	struct kwl_object *tool,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	struct kwl_object *surface;
	struct tablet_device *device;
	uint32_t words[2];
	int32_t hotspot[2];
	unsigned index;
	unsigned slot;

	/* The serial, the surface, and the hotspot. */
	if (size != 16U)
		return EPROTO;
	memcpy(words, bytes, sizeof(words));
	memcpy(hotspot, bytes + 8, sizeof(hotspot));

	/* A surface must be the client's own and have no other role. */
	surface = NULL;
	if (words[1] != 0U) {
		surface = kwl_find(tool->client, words[1]);
		if (surface == NULL || surface->kind != KWL_SURFACE)
			return EPROTO;
		if (surface->role != NULL || surface->sub_role != NULL)
			return EPROTO;
	}

	/* Only while this tool is on one of the client's surfaces. */
	server = tool->client->server;
	device = NULL;
	for (index = 0; index < TABLET_MAX; index++) {
		/* A free slot, or one without a tool near, is not it. */
		if (tablets[index].input == NULL || tablets[index].tool == TOOL_NONE)
			continue;

		/* The tablet whose tool in use this object stands for. */
		slot = tool_slot(&tablets[index], tablets[index].tool);
		if (slot == tool->tool_slot)
			device = &tablets[index];
	}

	/* Another client's surface, or none, keeps its cursor. */
	if (device == NULL ||
	    device->focus == NULL ||
	    device->focus->client != tool->client)
		return 0;

	/* The previous cursor surface is an ordinary surface again. */
	if (server->cursor_surface != NULL && server->cursor_surface != surface)
		server->cursor_surface->cursor_role = 0;

	/* No surface hides the cursor; a surface becomes it with its hotspot. */
	server->dirty = 1;
	server->cursor_shape = 0;
	server->cursor_client = tool->client;
	device->cursor_set = 1;
	if (surface == NULL) {
		server->cursor_surface = NULL;
		server->cursor_hidden = 1;
		return 0;
	}

	/* The surface is the cursor now, with its hotspot. */
	surface->cursor_role = 1;
	server->cursor_surface = surface;
	server->cursor_hotspot_x = hotspot[0];
	server->cursor_hotspot_y = hotspot[1];
	server->cursor_hidden = 0;

	/* Succeeded: the surface is the cursor while the tool is on the client. */
	return 0;
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

/* Queues one event whose only argument is a string. */
static void
emit_string(
	struct kwl_client *client,
	uint32_t id,
	uint32_t opcode,
	const char *text)
{
	unsigned char payload[4U + TABLET_TEXT_MAX + 4U];
	uint32_t length;
	size_t size;

	/* The length counts the terminator; the bytes are padded to a word. */
	length = (uint32_t)strnlen(text, TABLET_TEXT_MAX - 1U) + 1U;
	memset(payload, 0, sizeof(payload));
	memcpy(payload, &length, sizeof(length));
	memcpy(payload + 4, text, length - 1U);
	size = 4U + ((length + 3U) & ~3U);

	/* Queues the event. */
	emit(client, id, opcode, payload, size);
}
