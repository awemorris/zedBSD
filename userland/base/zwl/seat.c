/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The single wl_seat: pointer and keyboard objects, focus and event delivery.
 *
 * Focus follows the display: the surface currently scanned out is the one
 * that receives pointer and keyboard events, and only that surface's client
 * hears them.  A client that never asks for a pointer or keyboard receives
 * nothing.  Coordinates are surface-local integers carried as wl_fixed, the
 * surface being the whole output of --width by --height pixels.
 */

#include "zwl.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Event opcodes of wl_seat, in protocol order. */
#define SEAT_CAPABILITIES		0U
#define SEAT_NAME			1U

/* Event opcodes of wl_pointer, in protocol order. */
#define POINTER_ENTER			0U
#define POINTER_LEAVE			1U
#define POINTER_MOTION			2U
#define POINTER_BUTTON			3U
#define POINTER_AXIS			4U
#define POINTER_FRAME			5U
#define POINTER_AXIS_SOURCE		6U
#define POINTER_AXIS_DISCRETE		8U

/* Event opcodes of wl_keyboard, in protocol order. */
#define KEYBOARD_KEYMAP			0U
#define KEYBOARD_ENTER			1U
#define KEYBOARD_LEAVE			2U
#define KEYBOARD_KEY			3U
#define KEYBOARD_MODIFIERS		4U
#define KEYBOARD_REPEAT_INFO		5U

/* The first pointer and seat versions that carry frame/axis_source and release. */
#define POINTER_FRAME_VERSION		5U
#define KEYBOARD_REPEAT_VERSION		4U
#define RELEASE_VERSION			3U
#define SEAT_RELEASE_VERSION		5U
#define SEAT_NAME_VERSION		2U

/* Protocol enumeration values used on the wire. */
#define AXIS_VERTICAL			0U
#define AXIS_HORIZONTAL			1U
#define AXIS_SOURCE_WHEEL		0U
#define KEYMAP_NO_KEYMAP		0U

/* One wheel notch scrolls this many surface units, as common compositors do. */
#define WHEEL_STEP			15

/* The repeat delay reported with a zero rate, which tells clients not to repeat. */
#define REPEAT_DELAY_MS			600

static int create_device(struct zwl_object *seat, enum zwl_kind kind, const unsigned char *bytes, size_t size);
static void deliver(struct zwl_client *client, uint32_t id, uint32_t opcode, const void *payload, size_t size);
static void pointer_enter(struct zwl_object *pointer, struct zwl_object *surface, uint32_t serial);
static void keyboard_enter(struct zwl_object *keyboard, struct zwl_object *surface, uint32_t serial);
static void keyboard_modifiers(struct zwl_object *keyboard, uint32_t serial);
static void send_leave(struct zwl_object *surface);
static void set_cursor(struct zwl_server *server, struct zwl_object *surface, const unsigned char *bytes);
static void send_enter(struct zwl_object *surface);
static void report_seat(struct zwl_client *client);
static int keyboard_keymap(struct zwl_object *keyboard);

/*
 * Reserves the next nonzero event serial shared by configure and input events.
 */
uint32_t
zwl_next_serial(
	struct zwl_server *server)
{
	/* Zero is never a valid serial, so the wrap skips it. */
	server->serial++;
	if (server->serial == 0)
		server->serial++;

	/* Succeeded: the caller owns this serial for one logical event. */
	return server->serial;
}

/*
 * Sends a newly bound seat its capabilities and, from version 2, its name.
 */
int
zwl_seat_bind(
	struct zwl_object *seat)
{
	unsigned char name[12];
	uint32_t word;
	int error;

	/* The capability bits describe the devices open right now. */
	word = seat->client->server->capabilities;
	error = zwl_emit(seat->client, seat->id, SEAT_CAPABILITIES, &word, sizeof(word));
	if (error != 0)
		return error;

	/* Version 1 seats have no name event. */
	if (seat->version < SEAT_NAME_VERSION)
		return 0;

	/* The name is the string "seat0": a length word, six bytes and two of padding. */
	memset(name, 0, sizeof(name));
	word = 6;
	memcpy(name, &word, sizeof(word));
	memcpy(name + 4, "seat0", 6);
	error = zwl_emit(seat->client, seat->id, SEAT_NAME, name, sizeof(name));
	if (error != 0)
		return error;

	/* Succeeded: the seat knows what it offers. */
	return 0;
}

/*
 * Applies one wl_seat, wl_pointer or wl_keyboard request.
 */
int
zwl_seat_request(
	struct zwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	uint32_t surface_id;
	struct zwl_object *surface;
	int error;

	/* Each interface validates its own requests; anything unmatched is refused. */
	error = EPROTO;
	switch (object->kind) {
	case ZWL_SEAT:
		/* The seat creates device objects or retires its own binding. */
		if (opcode == 0U) {
			/* get_pointer carries one new_id. */
			error = create_device(object, ZWL_POINTER, bytes, size);
		} else if (opcode == 1U) {
			/* get_keyboard carries one new_id. */
			error = create_device(object, ZWL_KEYBOARD, bytes, size);
		} else if (opcode == 3U && size == 0 && object->version >= SEAT_RELEASE_VERSION) {
			/* release exists from version 5; get_touch is outside this seat. */
			zwl_object_destroy(object);
			error = 0;
		}
		break;
	case ZWL_POINTER:
		/* A pointer names a cursor image or retires its own binding. */
		if (opcode == 0U && size == 16U) {
			/* set_cursor names a surface of the client's own, or none (hides the cursor). */
			memcpy(&surface_id, bytes + 4, sizeof(surface_id));
			error = 0;
			surface = NULL;
			if (surface_id != 0) {
				/* A foreign object, or a surface with a window role, cannot become a cursor. */
				surface = zwl_find(object->client, surface_id);
				if (surface == NULL || surface->kind != ZWL_SURFACE || surface->role != NULL)
					error = EPROTO;
			}

			/* Only the client the pointer is over sets the cursor (design D8). */
			if (error == 0 &&
			    object->client->server->focus != NULL &&
			    object->client->server->focus->client == object->client)
				set_cursor(object->client->server, surface, bytes);
		} else if (opcode == 1U && size == 0 && object->version >= RELEASE_VERSION) {
			/* release exists from version 3. */
			zwl_object_destroy(object);
			error = 0;
		}
		break;
	case ZWL_KEYBOARD:
		/* release, from version 3, is the only keyboard request. */
		if (opcode == 0U && size == 0 && object->version >= RELEASE_VERSION) {
			zwl_object_destroy(object);
			error = 0;
		}
		break;
	default:
		/* The dispatcher routes only seat interfaces here. */
		break;
	}

	/* Reports a malformed or unsupported request. */
	if (error != 0)
		return error;

	/* Succeeded: the request has been applied exactly once. */
	return 0;
}

/*
 * Shows zdesktop's arrow again (when the pointer goes to another client, or
 * the cursor surface goes).
 */
void
zwl_cursor_default(
	struct zwl_server *server)
{
	/* The arrow, shown. */
	if (server->cursor_surface != NULL)
		server->cursor_surface->cursor_role = 0;
	server->cursor_surface = NULL;
	server->cursor_hidden = 0;
	server->dirty = 1;
}

/*
 * Moves input focus to the surface currently on the display.
 */
void
zwl_seat_focus(
	struct zwl_server *server)
{
	struct zwl_object *target;

	/* A dying surface or a failed client cannot take focus. */
	target = server->front_surface;
	if (target != NULL) {
		/* Only a live surface of a healthy client is a focus candidate. */
		if (target->dead || target->client->fatal)
			target = NULL;
	}

	/* An unchanged focus sends nothing. */
	if (target == server->focus)
		return;

	/* The old surface hears leave before the new one hears enter; the arrow comes back. */
	if (server->focus != NULL)
		send_leave(server->focus);
	zwl_cursor_default(server);

	/*
	 * The focus pointer names the surface whose client's pointer and keyboard
	 * objects have all been told enter; it is cleared before that surface is
	 * freed.
	 */
	server->focus = target;
	if (target != NULL)
		send_enter(target);

	/* Succeeded: focus follows the display. */
	return;
}

/*
 * Withdraws focus from a surface whose protocol identity is about to retire.
 */
void
zwl_seat_surface_gone(
	struct zwl_object *surface)
{
	struct zwl_server *server;

	/* Only the focused surface has anything to withdraw. */
	server = surface->client->server;
	if (server->focus != surface)
		return;

	/* Leave still names the surface, because delete_id has not been queued yet. */
	send_leave(surface);
	server->focus = NULL;

	/* Succeeded: no seat state refers to the retiring surface. */
	return;
}

/*
 * Tells every bound seat that the set of open devices changed.
 */
void
zwl_seat_capabilities(
	struct zwl_server *server)
{
	struct zwl_client *client;
	struct zwl_object *object;
	uint32_t word;

	/* Every client with a seat binding learns the new capability bits. */
	word = server->capabilities;
	for (client = server->clients; client != NULL; client = client->next) {
		/* Each live seat object of this client gets the same event. */
		for (object = client->objects; object != NULL; object = object->next) {
			/* Only seat objects carry capabilities. */
			if (object->kind != ZWL_SEAT || object->dead)
				continue;

			/* Queue the new bits for this binding. */
			deliver(client, object->id, SEAT_CAPABILITIES, &word, sizeof(word));
		}
	}

	/* Succeeded: every seat binding has been told. */
	return;
}

/*
 * Reports the current pointer position to the focused client.
 */
void
zwl_seat_motion(
	struct zwl_server *server,
	uint32_t time)
{
	struct zwl_object *object;
	uint32_t words[3];
	int taken;

	/* In the glass look a window being moved takes the motion. */
	if (server->glass && server->windowed) {
		taken = zwl_glass_motion(server);
		if (taken)
			return;
	}

	/* Nobody hears motion while no surface has focus. */
	if (server->focus == NULL)
		return;

	/* Motion carries a timestamp and the surface-local position in 24.8 fixed point. */
	words[0] = time;
	words[1] = (uint32_t)((server->pointer_x - server->focus->x) * 256);
	words[2] = (uint32_t)((server->pointer_y - server->focus->y) * 256);
	for (object = server->focus->client->objects; object != NULL; object = object->next) {
		/* Only live pointer objects receive motion. */
		if (object->kind != ZWL_POINTER || object->dead)
			continue;

		/* Queue the motion for this pointer. */
		deliver(object->client, object->id, POINTER_MOTION, words, sizeof(words));
	}

	/* Succeeded: the focused client knows where the pointer is. */
	return;
}

/*
 * Reports one pointer button press or release to the focused client.
 */
void
zwl_seat_button(
	struct zwl_server *server,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	struct zwl_object *object;
	uint32_t words[4];
	int taken;

	/* In the glass look the title bars and the desktop take their buttons. */
	if (server->glass && server->windowed) {
		taken = zwl_glass_button(server, button, state);
		if (taken)
			return;
	}

	/* A button without focus reaches nobody. */
	if (server->focus == NULL)
		return;

	/* The button event carries a new serial, the time, the Linux BTN_ code and the state. */
	words[0] = zwl_next_serial(server);
	words[1] = time;
	words[2] = button;
	words[3] = state;
	for (object = server->focus->client->objects; object != NULL; object = object->next) {
		/* Only live pointer objects receive buttons. */
		if (object->kind != ZWL_POINTER || object->dead)
			continue;

		/* Queue the button for this pointer. */
		deliver(object->client, object->id, POINTER_BUTTON, words, sizeof(words));
	}

	/* Succeeded: the focused client has heard the button. */
	return;
}

/*
 * Reports wheel scrolling to the focused client.
 *
 * The counts are in Wayland direction: positive vertical scrolls down and
 * positive horizontal scrolls right.  Each notch is WHEEL_STEP units.
 */
void
zwl_seat_axis(
	struct zwl_server *server,
	uint32_t time,
	int32_t vertical,
	int32_t horizontal)
{
	struct zwl_object *object;
	uint32_t words[3];
	uint32_t word;

	/* Scrolling without focus reaches nobody. */
	if (server->focus == NULL)
		return;

	/* Each pointer hears the source first, then per-axis discrete steps and values. */
	for (object = server->focus->client->objects; object != NULL; object = object->next) {
		/* Only live pointer objects receive scrolling. */
		if (object->kind != ZWL_POINTER || object->dead)
			continue;

		/* Version 5 pointers learn that a wheel produced this frame's scrolling. */
		if (object->version >= POINTER_FRAME_VERSION) {
			word = AXIS_SOURCE_WHEEL;
			deliver(object->client, object->id, POINTER_AXIS_SOURCE, &word, sizeof(word));
		}

		/* Vertical notches become a discrete count and a value. */
		if (vertical != 0) {
			/* The discrete count must precede its axis value in the same frame. */
			if (object->version >= POINTER_FRAME_VERSION) {
				words[0] = AXIS_VERTICAL;
				words[1] = (uint32_t)vertical;
				deliver(object->client, object->id, POINTER_AXIS_DISCRETE, words, 2U * sizeof(uint32_t));
			}

			/* The value is the scroll distance in surface units as 24.8 fixed point. */
			words[0] = time;
			words[1] = AXIS_VERTICAL;
			words[2] = (uint32_t)(vertical * WHEEL_STEP * 256);
			deliver(object->client, object->id, POINTER_AXIS, words, sizeof(words));
		}

		/* Horizontal notches follow the same pattern on the other axis. */
		if (horizontal != 0) {
			/* The discrete count must precede its axis value in the same frame. */
			if (object->version >= POINTER_FRAME_VERSION) {
				words[0] = AXIS_HORIZONTAL;
				words[1] = (uint32_t)horizontal;
				deliver(object->client, object->id, POINTER_AXIS_DISCRETE, words, 2U * sizeof(uint32_t));
			}

			/* The value is the scroll distance in surface units as 24.8 fixed point. */
			words[0] = time;
			words[1] = AXIS_HORIZONTAL;
			words[2] = (uint32_t)(horizontal * WHEEL_STEP * 256);
			deliver(object->client, object->id, POINTER_AXIS, words, sizeof(words));
		}
	}

	/* Succeeded: the focused client has heard the scrolling. */
	return;
}

/*
 * Ends one group of pointer events for version 5 pointers of the focused client.
 */
void
zwl_seat_frame(
	struct zwl_server *server)
{
	struct zwl_object *object;

	/* A frame without focus reaches nobody. */
	if (server->focus == NULL)
		return;

	/* Pointers older than version 5 have no frame event. */
	for (object = server->focus->client->objects; object != NULL; object = object->next) {
		/* Only live version 5 pointers are told where a group ends. */
		if (object->kind != ZWL_POINTER ||
		    object->dead ||
		    object->version < POINTER_FRAME_VERSION)
			continue;

		/* Queue the frame for this pointer. */
		deliver(object->client, object->id, POINTER_FRAME, NULL, 0);
	}

	/* Succeeded: the group is closed. */
	return;
}

/*
 * Reports one key press or release to the focused client.
 */
void
zwl_seat_key(
	struct zwl_server *server,
	uint32_t time,
	uint32_t key,
	uint32_t state)
{
	struct zwl_object *object;
	uint32_t words[4];

	/* A key without focus reaches nobody. */
	if (server->focus == NULL)
		return;

	/* The key event carries a new serial, the time, the evdev key code and the state. */
	words[0] = zwl_next_serial(server);
	words[1] = time;
	words[2] = key;
	words[3] = state;
	for (object = server->focus->client->objects; object != NULL; object = object->next) {
		/* Only live keyboard objects receive keys. */
		if (object->kind != ZWL_KEYBOARD || object->dead)
			continue;

		/* Queue the key for this keyboard. */
		deliver(object->client, object->id, KEYBOARD_KEY, words, sizeof(words));
	}

	/* Succeeded: the focused client has heard the key. */
	return;
}

/*
 * Reports the depressed modifier mask in server->modifiers to the focused client.
 */
void
zwl_seat_modifiers(
	struct zwl_server *server)
{
	struct zwl_object *object;
	uint32_t serial;

	/* A modifier change without focus reaches nobody; enter will carry it later. */
	if (server->focus == NULL)
		return;

	/* One serial covers the modifier report on every keyboard of the client. */
	serial = zwl_next_serial(server);
	for (object = server->focus->client->objects; object != NULL; object = object->next) {
		/* Only live keyboard objects receive modifiers. */
		if (object->kind != ZWL_KEYBOARD || object->dead)
			continue;

		/* Queue the new modifier state for this keyboard. */
		keyboard_modifiers(object, serial);
	}

	/* Succeeded: the focused client knows the modifier state. */
	return;
}

/* Creates a pointer or keyboard object and introduces it to the current focus. */
static int
create_device(
	struct zwl_object *seat,
	enum zwl_kind kind,
	const unsigned char *bytes,
	size_t size)
{
	struct zwl_object *device;
	struct zwl_server *server;
	int32_t repeat[2];
	uint32_t id;
	uint32_t serial;
	int error;

	/* The constructor carries exactly one new_id. */
	if (size != 4U)
		return EPROTO;

	/* The child inherits the seat's negotiated version. */
	memcpy(&id, bytes, sizeof(id));
	device = zwl_create(seat->client, id, kind, seat->version);
	if (device == NULL)
		return EPROTO;

	/* A keyboard first learns that no keymap applies: keys are evdev codes. */
	if (kind == ZWL_KEYBOARD) {
		/* The keymap event must carry a descriptor even when there is no keymap. */
		error = keyboard_keymap(device);
		if (error != 0)
			return error;

		/* Version 4 keyboards are told not to repeat keys themselves. */
		if (device->version >= KEYBOARD_REPEAT_VERSION) {
			repeat[0] = 0;
			repeat[1] = REPEAT_DELAY_MS;
			deliver(device->client, device->id, KEYBOARD_REPEAT_INFO, repeat, sizeof(repeat));
		}
	}

	/* A device created while its client has focus is entered at once. */
	server = seat->client->server;
	if (server->focus != NULL && server->focus->client == seat->client) {
		/* The new object alone hears this enter; the others already did. */
		serial = zwl_next_serial(server);
		if (kind == ZWL_POINTER) {
			pointer_enter(device, server->focus, serial);
		} else {
			keyboard_enter(device, server->focus, serial);
		}
	}

	/* The test log records which input objects each client holds. */
	report_seat(seat->client);

	/* Succeeded: the client owns the new input object. */
	return 0;
}

/* Queues one event and marks the client failed when the queue refuses it. */
static void
deliver(
	struct zwl_client *client,
	uint32_t id,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	struct zwl_server *server;
	int error;

	/* A failed client is only waiting for cleanup and hears nothing further. */
	if (client->fatal)
		return;

	/* A client that stopped reading loses its connection rather than compositor memory. */
	error = zwl_emit(client, id, opcode, payload, size);
	if (error != 0) {
		client->fatal = 1;
		client->fatal_time = zwl_milliseconds();
		return;
	}

	/* The exit summary counts every queued seat event. */
	server = client->server;
	server->seat_events++;

	/* Succeeded: the event is queued. */
	return;
}

/* Tells one pointer that it is over the focused surface, and where. */
static void
pointer_enter(
	struct zwl_object *pointer,
	struct zwl_object *surface,
	uint32_t serial)
{
	struct zwl_server *server;
	uint32_t words[4];

	/* Enter carries the serial, the surface and the surface-local position (the window's place taken off). */
	server = pointer->client->server;
	words[0] = serial;
	words[1] = surface->id;
	words[2] = (uint32_t)((server->pointer_x - surface->x) * 256);
	words[3] = (uint32_t)((server->pointer_y - surface->y) * 256);
	deliver(pointer->client, pointer->id, POINTER_ENTER, words, sizeof(words));

	/* Version 5 pointers see enter as a group of its own. */
	if (pointer->version >= POINTER_FRAME_VERSION)
		deliver(pointer->client, pointer->id, POINTER_FRAME, NULL, 0);

	/* Succeeded: the pointer is over the surface. */
	return;
}

/* Tells one keyboard that the focused surface has keyboard focus. */
static void
keyboard_enter(
	struct zwl_object *keyboard,
	struct zwl_object *surface,
	uint32_t serial)
{
	uint32_t words[3];

	/* Enter carries the serial, the surface and an empty array of held keys. */
	words[0] = serial;
	words[1] = surface->id;
	words[2] = 0;
	deliver(keyboard->client, keyboard->id, KEYBOARD_ENTER, words, sizeof(words));

	/* The modifier state that applies from now on follows enter. */
	serial = zwl_next_serial(keyboard->client->server);
	keyboard_modifiers(keyboard, serial);

	/* Succeeded: the keyboard is focused on the surface. */
	return;
}

/* Sends one keyboard the current depressed modifier mask. */
static void
keyboard_modifiers(
	struct zwl_object *keyboard,
	uint32_t serial)
{
	uint32_t words[5];

	/* Only depressed modifiers are tracked; nothing is latched or locked and the group is 0. */
	words[0] = serial;
	words[1] = keyboard->client->server->modifiers;
	words[2] = 0;
	words[3] = 0;
	words[4] = 0;
	deliver(keyboard->client, keyboard->id, KEYBOARD_MODIFIERS, words, sizeof(words));

	/* Succeeded: the keyboard knows the modifier state. */
	return;
}

/* Tells every pointer and keyboard of a surface's client that focus left it. */
static void
send_leave(
	struct zwl_object *surface)
{
	struct zwl_object *object;
	uint32_t words[2];

	/* Leave carries one serial and the surface for every device object. */
	words[0] = zwl_next_serial(surface->client->server);
	words[1] = surface->id;
	for (object = surface->client->objects; object != NULL; object = object->next) {
		/* A dead object has already been told it is gone. */
		if (object->dead)
			continue;

		/* Pointers hear leave and, from version 5, a frame closing it. */
		if (object->kind == ZWL_POINTER) {
			deliver(object->client, object->id, POINTER_LEAVE, words, sizeof(words));

			/* Version 5 pointers see leave as a group of its own. */
			if (object->version >= POINTER_FRAME_VERSION)
				deliver(object->client, object->id, POINTER_FRAME, NULL, 0);
		}

		/* Keyboards hear leave with the same serial. */
		if (object->kind == ZWL_KEYBOARD)
			deliver(object->client, object->id, KEYBOARD_LEAVE, words, sizeof(words));
	}

	/* Succeeded: the client no longer has input focus. */
	return;
}

/* Tells every pointer and keyboard of a surface's client that focus arrived. */
static void
send_enter(
	struct zwl_object *surface)
{
	struct zwl_object *object;
	uint32_t serial;

	/* One serial covers the enter on every device object of the client. */
	serial = zwl_next_serial(surface->client->server);
	for (object = surface->client->objects; object != NULL; object = object->next) {
		/* A dead object cannot be entered. */
		if (object->dead)
			continue;

		/* Pointers and keyboards each have their own enter event. */
		if (object->kind == ZWL_POINTER) {
			pointer_enter(object, surface, serial);
		} else if (object->kind == ZWL_KEYBOARD) {
			keyboard_enter(object, surface, serial);
		}
	}

	/* Succeeded: the client has input focus. */
	return;
}

/* Prints which input objects one client currently holds. */
static void
report_seat(
	struct zwl_client *client)
{
	struct zwl_object *object;
	unsigned pointer;
	unsigned keyboard;

	/* The client's live objects say whether it holds a pointer and a keyboard. */
	pointer = 0;
	keyboard = 0;
	for (object = client->objects; object != NULL; object = object->next) {
		/* Retired objects no longer count. */
		if (object->dead)
			continue;

		/* Record the two device kinds the log line reports. */
		if (object->kind == ZWL_POINTER)
			pointer = 1;
		if (object->kind == ZWL_KEYBOARD)
			keyboard = 1;
	}

	/* One line per constructor keeps the test log short. */
	printf("ZWL SEAT client=%llu pointer=%u keyboard=%u\n", (unsigned long long)client->number, pointer, keyboard);

	/* Succeeded: the line is printed. */
	return;
}

/* Sends a keyboard the no_keymap format with a descriptor of an empty file. */
static int
keyboard_keymap(
	struct zwl_object *keyboard)
{
	uint32_t words[2];
	int descriptor;
	int error;

	/* The protocol requires a descriptor; /dev/null read-only is an empty keymap file. */
	descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* The payload words are the format and the size; the descriptor travels beside them. */
	words[0] = KEYMAP_NO_KEYMAP;
	words[1] = 0;
	error = zwl_emit_fd(keyboard->client, keyboard->id, KEYBOARD_KEYMAP, words, sizeof(words), descriptor);
	if (error != 0)
		return error;

	/* The exit summary counts every queued seat event. */
	keyboard->client->server->seat_events++;

	/* Succeeded: the keyboard knows keys arrive as evdev codes. */
	return 0;
}

/*
 * Makes a surface the cursor, with its hotspot, or hides the cursor when
 * there is none.
 */
static void
set_cursor(
	struct zwl_server *server,
	struct zwl_object *surface,
	const unsigned char *bytes)
{
	int32_t hotspot[2];

	/* The previous cursor surface is an ordinary surface again. */
	if (server->cursor_surface != NULL && server->cursor_surface != surface)
		server->cursor_surface->cursor_role = 0;

	/* No surface hides the cursor. */
	server->dirty = 1;
	if (surface == NULL) {
		server->cursor_surface = NULL;
		server->cursor_hidden = 1;
		return;
	}

	/* The surface and the point of it that is the pointer's position. */
	memcpy(hotspot, bytes + 8, sizeof(hotspot));
	surface->cursor_role = 1;
	server->cursor_surface = surface;
	server->cursor_hotspot_x = hotspot[0];
	server->cursor_hotspot_y = hotspot[1];
	server->cursor_hidden = 0;
}

