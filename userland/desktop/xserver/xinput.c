/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * XInputExtension (version 2.2, with the two requests of version 1 its
 * libraries ask first) and the Generic Event Extension its events need.
 *
 * The server has one pointer and one keyboard, which the extension shows
 * as the two master devices (the virtual core pointer, 2, and keyboard,
 * 3) with one physical device attached to each (4 and 5).  Events come
 * from the master devices, the physical one named as their source.
 *
 * A client selects the extension's events on a window, for a device or
 * for all of them.  A key, button or motion event goes up the tree from
 * its window; on each window a client that selected the extension's
 * event hears it in the extension's form, and only when none did do the
 * core's clients hear the core event (input.c and event.c call here
 * first).  The crossing and focus events are the extension's apart from
 * the core's: each client hears the form it selected.  A grab
 * (XIGrabDevice, or the implicit grab a button press starts for a client
 * that heard it in the extension's form) is one of input.c's, which hears
 * the extension's events its mask names.
 *
 * Requests: GetExtensionVersion and ListInputDevices (version 1),
 * XIQueryPointer, XIWarpPointer, XIChangeCursor, XISetClientPointer,
 * XIGetClientPointer, XISelectEvents, XIQueryVersion, XIQueryDevice,
 * XISetFocus, XIGetFocus, XIGrabDevice, XIUngrabDevice, XIAllowEvents,
 * XIPassiveGrabDevice (whose grabs the server cannot make, and says so for
 * each modifier), XIPassiveUngrabDevice, XIListProperties,
 * XIDeleteProperty, XIGetProperty (the devices have no properties) and
 * XIGetSelectedEvents.  XIChangeHierarchy and XIChangeProperty are
 * refused with BadImplementation.  The Generic Event Extension answers
 * GEQueryVersion.
 *
 * Wire layouts follow the public XInput protocol specification (version
 * 2.2) and the Generic Event Extension's (version 1.0).
 */

#include "userland/desktop/xserver/internal.h"

#include <stdlib.h>
#include <string.h>

/* The extensions' names. */
#define XINPUT_NAME			"XInputExtension"
#define GE_NAME				"Generic Event Extension"

/* The version the server speaks, and the Generic Event Extension's. */
#define XINPUT_MAJOR_VERSION		2U
#define XINPUT_MINOR_VERSION		2U
#define GE_MAJOR_VERSION		1U
#define GE_MINOR_VERSION		0U

/* The requests (minor opcodes). */
#define XI_GET_EXTENSION_VERSION	1U
#define XI_LIST_INPUT_DEVICES		2U
#define XI_QUERY_POINTER		40U
#define XI_WARP_POINTER			41U
#define XI_CHANGE_CURSOR		42U
#define XI_CHANGE_HIERARCHY		43U
#define XI_SET_CLIENT_POINTER		44U
#define XI_GET_CLIENT_POINTER		45U
#define XI_SELECT_EVENTS		46U
#define XI_QUERY_VERSION		47U
#define XI_QUERY_DEVICE			48U
#define XI_SET_FOCUS			49U
#define XI_GET_FOCUS			50U
#define XI_GRAB_DEVICE			51U
#define XI_UNGRAB_DEVICE		52U
#define XI_ALLOW_EVENTS			53U
#define XI_PASSIVE_GRAB_DEVICE		54U
#define XI_PASSIVE_UNGRAB_DEVICE	55U
#define XI_LIST_PROPERTIES		56U
#define XI_CHANGE_PROPERTY		57U
#define XI_DELETE_PROPERTY		58U
#define XI_GET_PROPERTY			59U
#define XI_GET_SELECTED_EVENTS		60U
#define GE_QUERY_VERSION		0U

/* The core requests the extension's ones are answered through. */
#define CORE_CHANGE_ATTRIBUTES		2U
#define CORE_WARP_POINTER		41U
#define CORE_SET_INPUT_FOCUS		42U
#define CORE_CURSOR_BIT			(1U << 14)
#define CORE_REVERT_TO_PARENT		2U

/* The devices: all of them and all master devices (selections), and each device's id. */
#define XI_ALL_DEVICES			0U
#define XI_ALL_MASTER_DEVICES		1U
#define XI_MASTER_POINTER		2U
#define XI_MASTER_KEYBOARD		3U
#define XI_SLAVE_POINTER		4U
#define XI_SLAVE_KEYBOARD		5U
#define XI_DEVICES			4U

/* What a device is (version 2's uses, and version 1's). */
#define XI_USE_MASTER_POINTER		1U
#define XI_USE_MASTER_KEYBOARD		2U
#define XI_USE_SLAVE_POINTER		3U
#define XI_USE_SLAVE_KEYBOARD		4U
#define XI1_USE_POINTER			0U
#define XI1_USE_KEYBOARD		1U
#define XI1_USE_EXTENSION_KEYBOARD	3U
#define XI1_USE_EXTENSION_POINTER	4U

/* The classes of a device's description, and a valuator's absolute mode. */
#define XI_CLASS_KEY			0U
#define XI_CLASS_BUTTON			1U
#define XI_CLASS_VALUATOR		2U
#define XI_MODE_ABSOLUTE		1U

/* The pointer's buttons and axes. */
#define XI_BUTTONS			9U
#define XI_AXES				2U

/* The event types the server sends, and the last a client may select (version 2.2's). */
#define XI_EVENT_KEY_PRESS		2U
#define XI_EVENT_KEY_RELEASE		3U
#define XI_EVENT_BUTTON_PRESS		4U
#define XI_EVENT_MOTION			6U
#define XI_EVENT_ENTER			7U
#define XI_EVENT_LEAVE			8U
#define XI_EVENT_FOCUS_IN		9U
#define XI_EVENT_FOCUS_OUT		10U
#define XI_EVENT_LAST			24U

/* GenericEvent, the core event code every one of the extension's events goes by. */
#define GENERIC_EVENT			35U

/* The grab and AllowEvents modes. */
#define XI_GRAB_SYNC			0U
#define XI_GRAB_ASYNC			1U
#define XI_ALLOW_ASYNC_DEVICE		0U
#define XI_ALLOW_SYNC_DEVICE		1U
#define XI_ALLOW_REPLAY_DEVICE		2U
#define XI_ALLOW_ASYNC_PAIRED		3U
#define XI_ALLOW_ASYNC_PAIR		4U
#define XI_ALLOW_SYNC_PAIR		5U

/* The core's AllowEvents modes the extension's become. */
#define CORE_ASYNC_POINTER		0U
#define CORE_SYNC_POINTER		1U
#define CORE_REPLAY_POINTER		2U
#define CORE_ASYNC_KEYBOARD		3U
#define CORE_SYNC_KEYBOARD		4U
#define CORE_REPLAY_KEYBOARD		5U
#define CORE_SYNC_BOTH			6U
#define CORE_ASYNC_BOTH			7U

/* A passive grab's status the server answers for each modifier. */
#define XI_ALREADY_GRABBED		1U

/* The crossing events' flags (input.c's): the window has the focus, the pointer is on this screen. */
#define CROSSING_FOCUS			0x01U
#define CROSSING_SAME_SCREEN		0x02U

/* The lengths of the extension's events: a device event, a crossing or focus event (each with one word of buttons). */
#define XI_DEVICE_EVENT_SIZE		84U
#define XI_AXES_SIZE			(4U + XI_AXES * 8U)
#define XI_CROSSING_EVENT_SIZE		76U

/* The longest device description. */
#define XI_DEVICE_INFO_MAX		1280U

/*
 * One device: its id, what it is (version 2's use and version 1's), the
 * device it is paired with or attached to, whether it is a pointer, and
 * its name.
 */
struct xinput_device {
	uint16_t id;
	uint16_t use;
	uint8_t use1;
	uint16_t attachment;
	int pointer;
	const char *name;
	const char *type;
};

/* The devices: the master pointer and keyboard, and the physical one attached to each. */
static const struct xinput_device xinput_devices[XI_DEVICES] = {
	{ XI_MASTER_POINTER, XI_USE_MASTER_POINTER, XI1_USE_POINTER, XI_MASTER_KEYBOARD, 1, "Virtual core pointer", "MOUSE" },
	{ XI_MASTER_KEYBOARD, XI_USE_MASTER_KEYBOARD, XI1_USE_KEYBOARD, XI_MASTER_POINTER, 0, "Virtual core keyboard", "KEYBOARD" },
	{ XI_SLAVE_POINTER, XI_USE_SLAVE_POINTER, XI1_USE_EXTENSION_POINTER, XI_MASTER_POINTER, 1, "System pointer", "MOUSE" },
	{ XI_SLAVE_KEYBOARD, XI_USE_SLAVE_KEYBOARD, XI1_USE_EXTENSION_KEYBOARD, XI_MASTER_KEYBOARD, 0, "System keyboard", "KEYBOARD" }
};

/* The buttons' labels, by their numbers less one. */
static const char *const xinput_button_labels[XI_BUTTONS] = {
	"Button Left", "Button Middle", "Button Right", "Button Wheel Up", "Button Wheel Down",
	"Button Horiz Wheel Left", "Button Horiz Wheel Right", "Button Side", "Button Extra"
};

/* The axes' labels. */
static const char *const xinput_axis_labels[XI_AXES] = {
	"Abs X", "Abs Y"
};

static unsigned xinput_get_extension_version(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xinput_list_input_devices(struct x11server *server, struct x11_client *client, size_t length);
static unsigned xinput_query_pointer(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xinput_warp_pointer(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned xinput_change_cursor(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned xinput_set_client_pointer(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xinput_get_client_pointer(struct x11server *server, struct x11_client *client, size_t length);
static unsigned xinput_select_events(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned xinput_query_version(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xinput_query_device(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xinput_set_focus(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned xinput_get_focus(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xinput_grab_device(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned xinput_ungrab_device(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned xinput_allow_events(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned xinput_passive_grab(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xinput_passive_ungrab(struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xinput_list_properties(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xinput_get_property(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xinput_get_selected_events(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned xinput_device_classes(struct x11server *server, const struct xinput_device *device, uint8_t *info, int msb, size_t *size);
static const struct xinput_device *xinput_device_find(uint16_t id);
static int xinput_device_matches(uint16_t selected, uint16_t device);
static uint32_t xinput_mask_of(const struct x11_window *window, unsigned client, uint16_t device);
static int xinput_select_set(struct x11_window *window, unsigned client, uint16_t device, uint32_t mask);
static int xinput_press_taken(const struct x11_window *window, unsigned client, uint16_t device);
static void xinput_send_device(struct x11server *server, struct x11_client *client, uint8_t type, uint8_t detail, uint32_t time, const struct x11_window *window, uint32_t child, uint16_t state);
static void xinput_send_crossing(struct x11server *server, struct x11_client *client, uint8_t type, const struct x11_window *window, uint8_t detail, uint8_t mode, uint32_t time, uint32_t child, uint8_t flags);
static void xinput_write_state(uint8_t *bytes, const struct x11server *server, uint16_t state, int msb);
static void xinput_send(struct x11server *server, struct x11_client *client, uint8_t *event, size_t length);
static int xinput_window_or_none(struct x11server *server, uint32_t id);

/*
 * Answers QueryExtension's names XInputExtension and Generic Event
 * Extension: the major opcode, first event and first error.  Returns
 * nonzero when the name is one of them.
 */
int
x11_xinput_query(
	const uint8_t *name,
	size_t length,
	uint8_t *major,
	uint8_t *event,
	uint8_t *error)
{
	int differs;

	/* XInputExtension, with its version 1's events and errors. */
	if (length == sizeof(XINPUT_NAME) - 1U) {
		differs = memcmp(name, XINPUT_NAME, length);
		if (differs == 0) {
			*major = (uint8_t)X11_XINPUT_MAJOR;
			*event = (uint8_t)X11_XINPUT_EVENT;
			*error = (uint8_t)X11_XINPUT_ERROR;
			return 1;
		}
	}

	/* Another name. */
	if (length != sizeof(GE_NAME) - 1U)
		return 0;
	differs = memcmp(name, GE_NAME, length);
	if (differs != 0)
		return 0;

	/* Succeeded: the Generic Event Extension, with no events or errors of its own. */
	*major = (uint8_t)X11_GE_MAJOR;
	*event = 0U;
	*error = 0U;
	return 1;
}

/*
 * Answers an XInputExtension request.  Returns 0, or the error to send.
 */
unsigned
x11_xinput_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	unsigned code;

	/* The request's own handler. */
	client = &server->clients[index];
	switch (request[1]) {
	case XI_GET_EXTENSION_VERSION:
		code = xinput_get_extension_version(server, client, request, length);
		break;
	case XI_LIST_INPUT_DEVICES:
		code = xinput_list_input_devices(server, client, length);
		break;
	case XI_QUERY_POINTER:
		code = xinput_query_pointer(server, client, request, length);
		break;
	case XI_WARP_POINTER:
		code = xinput_warp_pointer(server, index, request, length);
		break;
	case XI_CHANGE_CURSOR:
		code = xinput_change_cursor(server, index, request, length);
		break;
	case XI_SET_CLIENT_POINTER:
		code = xinput_set_client_pointer(server, client, request, length);
		break;
	case XI_GET_CLIENT_POINTER:
		code = xinput_get_client_pointer(server, client, length);
		break;
	case XI_SELECT_EVENTS:
		code = xinput_select_events(server, index, request, length);
		break;
	case XI_QUERY_VERSION:
		code = xinput_query_version(server, client, request, length);
		break;
	case XI_QUERY_DEVICE:
		code = xinput_query_device(server, client, request, length);
		break;
	case XI_SET_FOCUS:
		code = xinput_set_focus(server, index, request, length);
		break;
	case XI_GET_FOCUS:
		code = xinput_get_focus(server, client, request, length);
		break;
	case XI_GRAB_DEVICE:
		code = xinput_grab_device(server, index, request, length);
		break;
	case XI_UNGRAB_DEVICE:
		code = xinput_ungrab_device(server, index, request, length);
		break;
	case XI_ALLOW_EVENTS:
		code = xinput_allow_events(server, index, request, length);
		break;
	case XI_PASSIVE_GRAB_DEVICE:
		code = xinput_passive_grab(server, client, request, length);
		break;
	case XI_PASSIVE_UNGRAB_DEVICE:
		code = xinput_passive_ungrab(client, request, length);
		break;
	case XI_LIST_PROPERTIES:
		code = xinput_list_properties(server, client, request, length);
		break;
	case XI_DELETE_PROPERTY:
		/* The devices have no properties: none to delete. */
		code = X11_BAD_LENGTH;
		if (length == 12U)
			code = 0U;
		break;
	case XI_GET_PROPERTY:
		code = xinput_get_property(server, client, request, length);
		break;
	case XI_GET_SELECTED_EVENTS:
		code = xinput_get_selected_events(server, index, request, length);
		break;
	case XI_CHANGE_HIERARCHY:
	case XI_CHANGE_PROPERTY:
		/* The devices are fixed: no master device is made, and no property kept. */
		code = X11_BAD_IMPLEMENTATION;
		break;
	default:
		code = X11_BAD_REQUEST;
		break;
	}

	/* Succeeded: answered, or the error to send. */
	return code;
}

/*
 * Answers a Generic Event Extension request: GEQueryVersion.  Returns 0,
 * or the error to send.
 */
unsigned
x11_ge_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32];

	/* GEQueryVersion alone, with the client's version. */
	client = &server->clients[index];
	if (request[1] != GE_QUERY_VERSION)
		return X11_BAD_REQUEST;
	if (length != 8U)
		return X11_BAD_LENGTH;

	/* Succeeded: the server's version. */
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, (uint16_t)GE_MAJOR_VERSION, client->order);
	x11_write16(reply + 10, (uint16_t)GE_MINOR_VERSION, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * Sends a key, button or motion event on one window of its way up the
 * tree to the clients that selected the extension's form of it there.
 * With a client named (not X11_NO_CLIENT), only that client may hear it.
 * Returns how many clients heard it, 0 when none selected it, or -1 when
 * others did but not the named client (the way ends there).
 */
int
x11_xinput_propagate(
	struct x11server *server,
	const struct x11_window *window,
	uint32_t child,
	uint8_t type,
	uint8_t detail,
	uint32_t time,
	uint16_t state,
	unsigned only)
{
	struct x11_client *client;
	uint32_t mask;
	uint16_t device;
	unsigned index;
	unsigned earlier;
	int heard;
	int selected;
	int repeated;

	/* The device the event comes from. */
	device = XI_MASTER_POINTER;
	if (type == XI_EVENT_KEY_PRESS || type == XI_EVENT_KEY_RELEASE)
		device = XI_MASTER_KEYBOARD;

	/* Each client that selected it, once. */
	heard = 0;
	selected = 0;
	for (index = 0U; index < window->xi_select_count; index++) {
		mask = xinput_mask_of(window, window->xi_selects[index].client, device);
		if ((mask & (1U << type)) == 0U)
			continue;
		selected = 1;

		/* A client met before on the window. */
		repeated = 0;
		for (earlier = 0U; earlier < index; earlier++) {
			if (window->xi_selects[earlier].client == window->xi_selects[index].client)
				repeated = 1;
		}
		if (repeated)
			continue;

		/* The named client alone, when one is named. */
		if (only != X11_NO_CLIENT && window->xi_selects[index].client != only)
			continue;
		client = x11_client_of(server, window->xi_selects[index].client);
		if (client == NULL)
			continue;
		xinput_send_device(server, client, type, detail, time, window, child, state);
		heard++;
	}

	/* Selected by others than the client named. */
	if (heard == 0 && selected)
		return -1;

	/* Succeeded: how many heard it. */
	return heard;
}

/*
 * Sends a key, button or motion event to the client grabbing its device
 * with the extension's grab, as seen from the grab's window, when the
 * grab's mask names it.  Returns nonzero when the client heard it.
 */
int
x11_xinput_to_grab(
	struct x11server *server,
	const struct x11_grab *grab,
	uint8_t type,
	uint8_t detail,
	uint32_t time,
	uint16_t state)
{
	struct x11_window *window;
	struct x11_window *under;
	struct x11_client *client;
	uint32_t child;

	/* The grab must have asked for it. */
	if ((grab->xi2_mask & (1U << type)) == 0U)
		return 0;

	/* The grab's window and client. */
	window = x11_window_find(server, grab->window);
	client = x11_client_of(server, grab->client);
	if (window == NULL || client == NULL)
		return 0;

	/* Succeeded: as seen from the grab's window, the child toward the window under the pointer. */
	under = x11_input_window_under(server);
	child = x11_event_child_toward(window, under);
	xinput_send_device(server, client, type, detail, time, window, child, state);
	return 1;
}

/*
 * Makes the implicit grab a button press starts the extension's, when a
 * client selected the extension's ButtonPress on the window the press
 * reached (it heard the press in that form): the grab is that client's,
 * with the events it selected there.
 */
void
x11_xinput_implicit_grab(
	const struct x11_window *window,
	struct x11_grab *grab)
{
	unsigned index;
	uint32_t mask;

	/* The client that selected the press there. */
	for (index = 0U; index < window->xi_select_count; index++) {
		mask = xinput_mask_of(window, window->xi_selects[index].client, XI_MASTER_POINTER);
		if ((mask & (1U << XI_EVENT_BUTTON_PRESS)) == 0U)
			continue;

		/* Succeeded: the grab is the client's, in the extension's form. */
		grab->client = window->xi_selects[index].client;
		grab->mask = 0U;
		grab->owner_events = 0U;
		grab->xi2 = 1U;
		grab->xi2_mask = mask;
		return;
	}
}

/*
 * Sends a crossing event (Enter, Leave) on a window in the extension's
 * form: to the client grabbing the pointer with the extension's grab
 * when it asked, else (no grab) to each client that selected it there.
 */
void
x11_xinput_crossing(
	struct x11server *server,
	uint8_t type,
	const struct x11_window *window,
	uint8_t detail,
	uint8_t mode,
	uint32_t time,
	uint32_t child,
	uint8_t flags)
{
	const struct x11_grab *grab;
	struct x11_client *client;
	uint32_t mask;
	unsigned index;

	/* A grab: the grabbing client alone, by its grab's mask on its window and its own selection when it asked. */
	grab = &server->pointer_grab;
	if (grab->client != X11_NO_CLIENT) {
		if (!grab->xi2)
			return;
		mask = 0U;
		if (window->id == grab->window)
			mask = grab->xi2_mask;
		if (grab->owner_events)
			mask |= xinput_mask_of(window, grab->client, XI_MASTER_POINTER);
		client = x11_client_of(server, grab->client);
		if ((mask & (1U << type)) == 0U || client == NULL)
			return;
		xinput_send_crossing(server, client, type, window, detail, mode, time, child, flags);
		return;
	}

	/* Succeeded: each client that selected it. */
	for (index = 0U; index < window->xi_select_count; index++) {
		mask = window->xi_selects[index].mask;
		if (!xinput_device_matches(window->xi_selects[index].device, XI_MASTER_POINTER))
			continue;
		if ((mask & (1U << type)) == 0U)
			continue;
		client = x11_client_of(server, window->xi_selects[index].client);
		if (client == NULL)
			continue;
		xinput_send_crossing(server, client, type, window, detail, mode, time, child, flags);
	}
}

/*
 * Sends a focus event (FocusIn, FocusOut) on a window in the extension's
 * form to each client that selected it there.
 */
void
x11_xinput_focus(
	struct x11server *server,
	uint8_t type,
	const struct x11_window *window,
	uint8_t detail,
	uint8_t mode)
{
	struct x11_client *client;
	unsigned index;
	int matches;

	/* Succeeded: each client that selected it. */
	for (index = 0U; index < window->xi_select_count; index++) {
		matches = xinput_device_matches(window->xi_selects[index].device, XI_MASTER_KEYBOARD);
		if (!matches)
			continue;
		if ((window->xi_selects[index].mask & (1U << type)) == 0U)
			continue;
		client = x11_client_of(server, window->xi_selects[index].client);
		if (client == NULL)
			continue;
		xinput_send_crossing(server, client, type, window, detail, mode, server->now, 0U, CROSSING_SAME_SCREEN);
	}
}

/*
 * Forgets what clients selected on a window that is being destroyed.
 */
void
x11_xinput_window_destroyed(
	struct x11_window *window)
{
	/* Succeeded: the selections go. */
	free(window->xi_selects);
	window->xi_selects = NULL;
	window->xi_select_count = 0U;
}

/*
 * Forgets what a client that is going selected on every window.
 */
void
x11_xinput_forget_client(
	struct x11server *server,
	unsigned client)
{
	struct x11_window *window;
	unsigned index;

	/* Each window, each of the client's entries (from the end, the last taking its place). */
	for (window = server->root;
	     window != NULL;
	     window = x11_window_next(window, server->root)) {
		index = window->xi_select_count;
		while (index > 0U) {
			index--;
			if (window->xi_selects[index].client != client)
				continue;
			window->xi_selects[index] = window->xi_selects[window->xi_select_count - 1U];
			window->xi_select_count--;
		}
	}
}

/* GetExtensionVersion (version 1): the extension is there, at the server's version. */
static unsigned
xinput_get_extension_version(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	size_t name_length;

	/* The name, filling the request. */
	if (length < 8U)
		return X11_BAD_LENGTH;
	name_length = x11_read16(request + 4, client->order);
	if (8U + ((name_length + 3U) & ~(size_t)3U) != length)
		return X11_BAD_LENGTH;

	/* Succeeded: the version, present. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XI_GET_EXTENSION_VERSION;
	x11_write16(reply + 8, (uint16_t)XINPUT_MAJOR_VERSION, client->order);
	x11_write16(reply + 10, (uint16_t)XINPUT_MINOR_VERSION, client->order);
	reply[12] = 1U;
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * ListInputDevices (version 1): each device's type, id and use, then its
 * classes (the keyboards' keys; the pointers' buttons and two absolute
 * axes), then the names.
 */
static unsigned
xinput_list_input_devices(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	const struct xinput_device *device;
	uint8_t reply[32U + XI_DEVICES * 8U + XI_DEVICES * 32U + XI_DEVICES * 32U];
	uint8_t *field;
	uint32_t type;
	size_t used;
	size_t name_length;
	unsigned index;
	int msb;

	/* The header alone. */
	msb = client->order;
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* The devices. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XI_LIST_INPUT_DEVICES;
	reply[8] = (uint8_t)XI_DEVICES;
	for (index = 0U; index < XI_DEVICES; index++) {
		device = &xinput_devices[index];
		field = reply + 32U + index * 8U;
		type = x11_atom_intern(server, device->type, strlen(device->type), 0);
		x11_write32(field, type, msb);
		field[4] = (uint8_t)device->id;
		field[5] = 1U;
		if (device->pointer)
			field[5] = 2U;
		field[6] = device->use1;
	}

	/* Each device's classes. */
	used = 32U + XI_DEVICES * 8U;
	for (index = 0U; index < XI_DEVICES; index++) {
		device = &xinput_devices[index];
		field = reply + used;

		/* A keyboard's keys. */
		if (!device->pointer) {
			field[0] = (uint8_t)XI_CLASS_KEY;
			field[1] = 8U;
			field[2] = (uint8_t)X11_KEYMAP_KEYCODE_FIRST;
			field[3] = 255U;
			x11_write16(field + 4, (uint16_t)(256U - X11_KEYMAP_KEYCODE_FIRST), msb);
			used += 8U;
			continue;
		}

		/* A pointer's buttons, and its axes on the screen. */
		field[0] = (uint8_t)XI_CLASS_BUTTON;
		field[1] = 4U;
		x11_write16(field + 2, (uint16_t)XI_BUTTONS, msb);
		field[4] = (uint8_t)XI_CLASS_VALUATOR;
		field[5] = (uint8_t)(8U + XI_AXES * 12U);
		field[6] = (uint8_t)XI_AXES;
		field[7] = (uint8_t)XI_MODE_ABSOLUTE;
		x11_write32(field + 20, (uint32_t)(server->width - 1U), msb);
		x11_write32(field + 32, (uint32_t)(server->height - 1U), msb);
		used += 4U + 8U + XI_AXES * 12U;
	}

	/* The names, each counted. */
	for (index = 0U; index < XI_DEVICES; index++) {
		name_length = strlen(xinput_devices[index].name);
		reply[used] = (uint8_t)name_length;
		memcpy(reply + used + 1U, xinput_devices[index].name, name_length);
		used += 1U + name_length;
	}

	/* Succeeded: the reply, padded to a word. */
	used = (used + 3U) & ~(size_t)3U;
	x11_reply(server, client, reply, used);
	return 0U;
}

/* XIQueryPointer: where the pointer is, on the root window and in a window, its child, the buttons and the keyboard's state. */
static unsigned
xinput_query_pointer(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *device;
	struct x11_window *window;
	struct x11_window *under;
	uint8_t reply[60];
	uint32_t child;
	uint16_t state;
	int x;
	int y;
	int error;
	int msb;

	/* The window and a pointer. */
	msb = client->order;
	if (length != 12U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;
	device = xinput_device_find(x11_read16(request + 8, msb));
	if (device == NULL || !device->pointer)
		return X11_XINPUT_ERROR;

	/* The place: the system's when the backend tells it, else the server's. */
	x = server->pointer_x;
	y = server->pointer_y;
	error = 1;
	if (server->ops->pointer != NULL)
		error = server->ops->pointer(server->backend, &x, &y);
	if (error != 0) {
		x = server->pointer_x;
		y = server->pointer_y;
	}

	/* The window's child the pointer is in (None away from the server's place, or in none). */
	child = 0U;
	if (x == server->pointer_x && y == server->pointer_y) {
		under = x11_input_window_under(server);
		child = x11_event_child_toward(window, under);
	}

	/* Succeeded: the places, on this screen, the state and the buttons. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XI_QUERY_POINTER;
	state = x11_input_state(server);
	x11_write32(reply + 8, X11_ROOT_XID, msb);
	x11_write32(reply + 12, child, msb);
	x11_write32(reply + 16, (uint32_t)x << 16, msb);
	x11_write32(reply + 20, (uint32_t)y << 16, msb);
	x11_write32(reply + 24, (uint32_t)(x - window->x) << 16, msb);
	x11_write32(reply + 28, (uint32_t)(y - window->y) << 16, msb);
	reply[32] = 1U;
	x11_write16(reply + 34, 1U, msb);
	xinput_write_state(reply + 36, server, state, msb);
	x11_write32(reply + 56, (uint32_t)((state >> 8) & 0x1fU) << 1, msb);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* XIWarpPointer: the pointer moves as the core's WarpPointer moves it (places in whole pixels). */
static unsigned
xinput_warp_pointer(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *device;
	uint8_t core[24];
	unsigned code;
	int msb;

	/* A pointer. */
	msb = server->clients[index].order;
	if (length != 36U)
		return X11_BAD_LENGTH;
	device = xinput_device_find(x11_read16(request + 32, msb));
	if (device == NULL || !device->pointer)
		return X11_XINPUT_ERROR;

	/* The core request: the windows, the source's place and size, and the place to go (each in whole pixels). */
	memset(core, 0, sizeof(core));
	core[0] = (uint8_t)CORE_WARP_POINTER;
	x11_write16(core + 2, 6U, msb);
	memcpy(core + 4, request + 4, 8U);
	x11_write16(core + 12, (uint16_t)((int32_t)x11_read32(request + 12, msb) >> 16), msb);
	x11_write16(core + 14, (uint16_t)((int32_t)x11_read32(request + 16, msb) >> 16), msb);
	memcpy(core + 16, request + 20, 4U);
	x11_write16(core + 20, (uint16_t)((int32_t)x11_read32(request + 24, msb) >> 16), msb);
	x11_write16(core + 22, (uint16_t)((int32_t)x11_read32(request + 28, msb) >> 16), msb);

	/* Succeeded: as the core request does, or its error. */
	code = x11_input_request(server, index, core, sizeof(core));
	return code;
}

/* XIChangeCursor: the window's cursor changes, as the core's ChangeWindowAttributes changes it. */
static unsigned
xinput_change_cursor(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *device;
	uint8_t core[16];
	unsigned code;
	int msb;

	/* A pointer. */
	msb = server->clients[index].order;
	if (length != 16U)
		return X11_BAD_LENGTH;
	device = xinput_device_find(x11_read16(request + 12, msb));
	if (device == NULL || !device->pointer)
		return X11_XINPUT_ERROR;

	/* The core request: the window, the cursor bit and the cursor. */
	memset(core, 0, sizeof(core));
	core[0] = (uint8_t)CORE_CHANGE_ATTRIBUTES;
	x11_write16(core + 2, 4U, msb);
	memcpy(core + 4, request + 4, 4U);
	x11_write32(core + 8, CORE_CURSOR_BIT, msb);
	memcpy(core + 12, request + 8, 4U);

	/* Succeeded: as the core request does, or its error. */
	code = x11_request_change_attributes(server, index, core, sizeof(core));
	return code;
}

/* XISetClientPointer: the one pointer is every client's, so only the request's values are checked. */
static unsigned
xinput_set_client_pointer(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *device;
	uint32_t window;
	int found;
	int msb;

	/* The window (None: the client's own) and the master pointer. */
	msb = client->order;
	if (length != 12U)
		return X11_BAD_LENGTH;
	window = x11_read32(request + 4, msb);
	found = xinput_window_or_none(server, window);
	if (!found)
		return X11_BAD_WINDOW;
	device = xinput_device_find(x11_read16(request + 8, msb));
	if (device == NULL || device->id != XI_MASTER_POINTER)
		return X11_XINPUT_ERROR;

	/* Succeeded: the pointer it already is. */
	return 0U;
}

/* XIGetClientPointer: the master pointer, every client's. */
static unsigned
xinput_get_client_pointer(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	uint8_t reply[32];

	/* The window, which changes nothing. */
	if (length != 8U)
		return X11_BAD_LENGTH;

	/* Succeeded: set, the master pointer. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XI_GET_CLIENT_POINTER;
	reply[8] = 1U;
	x11_write16(reply + 10, (uint16_t)XI_MASTER_POINTER, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * XISelectEvents: the events the client hears on a window from each
 * device (or all devices, or all master devices) named, replacing what
 * it selected for that device there.  Only one client may select
 * ButtonPress on a window for a device.
 */
static unsigned
xinput_select_events(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *found;
	struct x11_window *window;
	uint32_t mask;
	uint16_t device;
	size_t offset;
	size_t words;
	size_t word;
	unsigned count;
	unsigned entry;
	int taken;
	int added;
	int msb;

	/* The window and the masks' count. */
	msb = server->clients[index].order;
	if (length < 12U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;
	count = x11_read16(request + 8, msb);
	if (count == 0U)
		return X11_BAD_VALUE;

	/* Each mask must fit, name a device, name no event past the last, and not take another client's ButtonPress. */
	offset = 12U;
	for (entry = 0U; entry < count; entry++) {
		if (offset + 4U > length)
			return X11_BAD_LENGTH;
		device = x11_read16(request + offset, msb);
		words = x11_read16(request + offset + 2U, msb);
		if (offset + 4U + words * 4U > length)
			return X11_BAD_LENGTH;
		found = xinput_device_find(device);
		if (found == NULL && device != XI_ALL_DEVICES && device != XI_ALL_MASTER_DEVICES)
			return X11_XINPUT_ERROR;
		mask = 0U;
		if (words > 0U)
			mask = x11_read32(request + offset + 4U, 0);
		for (word = 0U; word < words; word++) {
			if (word > 0U && x11_read32(request + offset + 4U + word * 4U, 0) != 0U)
				return X11_BAD_VALUE;
		}
		if ((mask >> (XI_EVENT_LAST + 1U)) != 0U)
			return X11_BAD_VALUE;
		taken = xinput_press_taken(window, index, device);
		if (taken && (mask & (1U << XI_EVENT_BUTTON_PRESS)) != 0U)
			return X11_BAD_ACCESS;
		offset += 4U + words * 4U;
	}
	if (offset != length)
		return X11_BAD_LENGTH;

	/* Each mask, kept (the bytes of a mask are its bits in order, whatever the client's byte order). */
	offset = 12U;
	for (entry = 0U; entry < count; entry++) {
		device = x11_read16(request + offset, msb);
		words = x11_read16(request + offset + 2U, msb);
		mask = 0U;
		if (words > 0U)
			mask = x11_read32(request + offset + 4U, 0);
		added = xinput_select_set(window, index, device, mask);
		if (!added)
			return X11_BAD_ALLOC;
		offset += 4U + words * 4U;
	}

	/* Succeeded: selected. */
	return 0U;
}

/*
 * XIQueryVersion: the version the client and the server agree on (the
 * lower of the two); a client must speak version 2.
 */
static unsigned
xinput_query_version(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	uint16_t major;
	uint16_t minor;

	/* The client's version. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	major = x11_read16(request + 4, client->order);
	minor = x11_read16(request + 6, client->order);
	if (major < XINPUT_MAJOR_VERSION)
		return X11_BAD_VALUE;

	/* The lower of the two. */
	if (major > XINPUT_MAJOR_VERSION || minor > XINPUT_MINOR_VERSION) {
		major = XINPUT_MAJOR_VERSION;
		minor = XINPUT_MINOR_VERSION;
	}
	client->xi_major = major;
	client->xi_minor = minor;

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XI_QUERY_VERSION;
	x11_write16(reply + 8, major, client->order);
	x11_write16(reply + 10, minor, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* XIQueryDevice: the devices named (all, all master devices, or one), each with its classes and name. */
static unsigned
xinput_query_device(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *device;
	uint8_t reply[32U + XI_DEVICES * XI_DEVICE_INFO_MAX];
	uint8_t *field;
	uint16_t asked;
	size_t used;
	size_t name_length;
	size_t size;
	unsigned index;
	unsigned devices;
	unsigned classes;
	int msb;

	/* The devices named. */
	msb = client->order;
	if (length != 8U)
		return X11_BAD_LENGTH;
	asked = x11_read16(request + 4, msb);
	if (asked != XI_ALL_DEVICES && asked != XI_ALL_MASTER_DEVICES) {
		device = xinput_device_find(asked);
		if (device == NULL)
			return X11_XINPUT_ERROR;
	}

	/* Each device named: its id, use, attachment, classes, name length and that it is enabled; its name; its classes. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XI_QUERY_DEVICE;
	used = 32U;
	devices = 0U;
	for (index = 0U; index < XI_DEVICES; index++) {
		device = &xinput_devices[index];
		if (asked == XI_ALL_MASTER_DEVICES && device->use > XI_USE_MASTER_KEYBOARD)
			continue;
		if (asked != XI_ALL_DEVICES && asked != XI_ALL_MASTER_DEVICES && asked != device->id)
			continue;
		field = reply + used;
		name_length = strlen(device->name);
		x11_write16(field, device->id, msb);
		x11_write16(field + 2, device->use, msb);
		x11_write16(field + 4, device->attachment, msb);
		x11_write16(field + 8, (uint16_t)name_length, msb);
		field[10] = 1U;
		memcpy(field + 12, device->name, name_length);
		used += 12U + ((name_length + 3U) & ~(size_t)3U);
		classes = xinput_device_classes(server, device, reply + used, msb, &size);
		x11_write16(field + 6, (uint16_t)classes, msb);
		used += size;
		devices++;
	}

	/* Succeeded: the count, and the reply. */
	x11_write16(reply + 8, (uint16_t)devices, msb);
	x11_reply(server, client, reply, used);
	return 0U;
}

/* XISetFocus: the keyboard's focus moves as the core's SetInputFocus moves it (reverting to the parent). */
static unsigned
xinput_set_focus(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *device;
	uint8_t core[12];
	unsigned code;
	int msb;

	/* A keyboard. */
	msb = server->clients[index].order;
	if (length != 16U)
		return X11_BAD_LENGTH;
	device = xinput_device_find(x11_read16(request + 12, msb));
	if (device == NULL || device->pointer)
		return X11_XINPUT_ERROR;

	/* The core request: reverting to the parent, the focus and the time. */
	memset(core, 0, sizeof(core));
	core[0] = (uint8_t)CORE_SET_INPUT_FOCUS;
	core[1] = (uint8_t)CORE_REVERT_TO_PARENT;
	x11_write16(core + 2, 3U, msb);
	memcpy(core + 4, request + 4, 8U);

	/* Succeeded: as the core request does, or its error. */
	code = x11_input_request(server, index, core, sizeof(core));
	return code;
}

/* XIGetFocus: the keyboard's focus. */
static unsigned
xinput_get_focus(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *device;
	uint8_t reply[32];

	/* A keyboard. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	device = xinput_device_find(x11_read16(request + 4, client->order));
	if (device == NULL || device->pointer)
		return X11_XINPUT_ERROR;

	/* Succeeded: the focus window, None or PointerRoot. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XI_GET_FOCUS;
	x11_write32(reply + 8, server->focus, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * XIGrabDevice: the pointer or the keyboard grabbed for the client, which
 * hears the extension's events its mask names (the grab is the core's
 * grab of that device, with its rules and its status).
 */
static unsigned
xinput_grab_device(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *device;
	struct x11_client *client;
	struct x11_window *window;
	struct x11_grab grab;
	uint8_t reply[32];
	uint32_t time;
	size_t words;
	unsigned status;
	int msb;

	/* The window, the time, the cursor, the device, the modes, owner-events and the mask. */
	client = &server->clients[index];
	msb = client->order;
	if (length < 24U)
		return X11_BAD_LENGTH;
	words = x11_read16(request + 22, msb);
	if (length != 24U + words * 4U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;
	device = xinput_device_find(x11_read16(request + 16, msb));
	if (device == NULL)
		return X11_XINPUT_ERROR;
	if (request[18] > XI_GRAB_ASYNC || request[19] > XI_GRAB_ASYNC || request[20] > 1U)
		return X11_BAD_VALUE;
	time = x11_read32(request + 8, msb);

	/* The grab: its modes by the device grabbed and its pair, the extension's events. */
	memset(&grab, 0, sizeof(grab));
	grab.client = index;
	grab.window = window->id;
	grab.cursor = x11_read32(request + 12, msb);
	grab.time = time;
	if (time == 0U)
		grab.time = server->now;
	grab.owner_events = request[20];
	grab.pointer_mode = request[18];
	grab.keyboard_mode = request[19];
	if (!device->pointer) {
		grab.keyboard_mode = request[18];
		grab.pointer_mode = request[19];
	}
	grab.xi2 = 1U;
	if (words > 0U)
		grab.xi2_mask = x11_read32(request + 24, 0);
	status = x11_input_grab_device(server, index, !device->pointer, &grab, time);

	/* Succeeded: the status. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XI_GRAB_DEVICE;
	reply[8] = (uint8_t)status;
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* XIUngrabDevice: the client's grab of the device ends, as the core's ungrab ends it. */
static unsigned
xinput_ungrab_device(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *device;
	uint32_t time;
	int msb;

	/* The time and the device. */
	msb = server->clients[index].order;
	if (length != 12U)
		return X11_BAD_LENGTH;
	time = x11_read32(request + 4, msb);
	device = xinput_device_find(x11_read16(request + 8, msb));
	if (device == NULL)
		return X11_XINPUT_ERROR;

	/* Succeeded: ended. */
	x11_input_ungrab_device(server, index, !device->pointer, time);
	return 0U;
}

/*
 * XIAllowEvents: the devices the client's grabs froze go on, by the core
 * AllowEvents mode the extension's mode for the device names.
 */
static unsigned
xinput_allow_events(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *device;
	uint32_t time;
	unsigned mode;
	int msb;

	/* The time, the device and the mode (version 2.2 adds a touch and a window). */
	msb = server->clients[index].order;
	if (length != 12U && length != 20U)
		return X11_BAD_LENGTH;
	time = x11_read32(request + 4, msb);
	device = xinput_device_find(x11_read16(request + 8, msb));
	if (device == NULL)
		return X11_XINPUT_ERROR;

	/* The core's mode, by the device and its pair. */
	switch (request[10]) {
	case XI_ALLOW_ASYNC_DEVICE:
		mode = CORE_ASYNC_POINTER;
		if (!device->pointer)
			mode = CORE_ASYNC_KEYBOARD;
		break;
	case XI_ALLOW_SYNC_DEVICE:
		mode = CORE_SYNC_POINTER;
		if (!device->pointer)
			mode = CORE_SYNC_KEYBOARD;
		break;
	case XI_ALLOW_REPLAY_DEVICE:
		mode = CORE_REPLAY_POINTER;
		if (!device->pointer)
			mode = CORE_REPLAY_KEYBOARD;
		break;
	case XI_ALLOW_ASYNC_PAIRED:
		mode = CORE_ASYNC_KEYBOARD;
		if (!device->pointer)
			mode = CORE_ASYNC_POINTER;
		break;
	case XI_ALLOW_ASYNC_PAIR:
		mode = CORE_ASYNC_BOTH;
		break;
	case XI_ALLOW_SYNC_PAIR:
		mode = CORE_SYNC_BOTH;
		break;
	default:
		/* The touches' modes: the devices have no touches. */
		return X11_BAD_VALUE;
	}

	/* Succeeded: as the core's AllowEvents. */
	x11_input_allow(server, index, mode, time);
	return 0U;
}

/*
 * XIPassiveGrabDevice: the server makes no passive grabs of the
 * extension's, so each modifier asked for is answered as already
 * grabbed.
 */
static unsigned
xinput_passive_grab(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *device;
	struct x11_window *window;
	uint8_t *reply;
	size_t words;
	size_t modifiers;
	size_t index;
	int msb;

	/* The window, the device, the mask and the modifiers. */
	msb = client->order;
	if (length < 32U)
		return X11_BAD_LENGTH;
	modifiers = x11_read16(request + 22, msb);
	words = x11_read16(request + 24, msb);
	if (length != 32U + words * 4U + modifiers * 4U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 8, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;
	device = xinput_device_find(x11_read16(request + 20, msb));
	if (device == NULL)
		return X11_XINPUT_ERROR;

	/* The reply: each modifier with its status. */
	reply = calloc(1U, 32U + modifiers * 8U);
	if (reply == NULL)
		return X11_BAD_ALLOC;
	reply[1] = (uint8_t)XI_PASSIVE_GRAB_DEVICE;
	x11_write16(reply + 8, (uint16_t)modifiers, msb);
	for (index = 0U; index < modifiers; index++) {
		memcpy(reply + 32U + index * 8U, request + 32U + words * 4U + index * 4U, 4U);
		reply[32U + index * 8U + 4U] = (uint8_t)XI_ALREADY_GRABBED;
	}

	/* Succeeded: sent. */
	x11_reply(server, client, reply, 32U + modifiers * 8U);
	free(reply);
	return 0U;
}

/* XIPassiveUngrabDevice: there are no passive grabs of the extension's to end. */
static unsigned
xinput_passive_ungrab(
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	size_t modifiers;

	/* The fixed part and the modifiers. */
	if (length < 20U)
		return X11_BAD_LENGTH;
	modifiers = x11_read16(request + 16, client->order);
	if (length != 20U + modifiers * 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: nothing to end. */
	return 0U;
}

/* XIListProperties: the devices have no properties. */
static unsigned
xinput_list_properties(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *device;
	uint8_t reply[32];

	/* A device. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	device = xinput_device_find(x11_read16(request + 4, client->order));
	if (device == NULL)
		return X11_XINPUT_ERROR;

	/* Succeeded: none. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XI_LIST_PROPERTIES;
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* XIGetProperty: the devices have no properties, so every one is None. */
static unsigned
xinput_get_property(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	const struct xinput_device *device;
	uint8_t reply[32];

	/* A device. */
	if (length != 24U)
		return X11_BAD_LENGTH;
	device = xinput_device_find(x11_read16(request + 4, client->order));
	if (device == NULL)
		return X11_XINPUT_ERROR;

	/* Succeeded: of type None, with nothing after. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XI_GET_PROPERTY;
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* XIGetSelectedEvents: what the client selected on a window, a mask for each device. */
static unsigned
xinput_get_selected_events(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *window;
	uint8_t *reply;
	size_t used;
	unsigned entry;
	unsigned masks;
	int msb;

	/* The window. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* The reply, room for each of the window's entries. */
	reply = calloc(1U, 32U + window->xi_select_count * 8U);
	if (reply == NULL)
		return X11_BAD_ALLOC;
	reply[1] = (uint8_t)XI_GET_SELECTED_EVENTS;

	/* Each of the client's masks: its device, one word, the mask's bits in order. */
	used = 32U;
	masks = 0U;
	for (entry = 0U; entry < window->xi_select_count; entry++) {
		if (window->xi_selects[entry].client != index)
			continue;
		x11_write16(reply + used, window->xi_selects[entry].device, msb);
		x11_write16(reply + used + 2U, 1U, msb);
		x11_write32(reply + used + 4U, window->xi_selects[entry].mask, 0);
		used += 8U;
		masks++;
	}

	/* Succeeded: sent. */
	x11_write16(reply + 8, (uint16_t)masks, msb);
	x11_reply(server, client, reply, used);
	free(reply);
	return 0U;
}

/*
 * Writes a device's classes for XIQueryDevice: a keyboard's keys (8 to
 * 255); a pointer's buttons (none down, with their labels) and its two
 * absolute axes on the screen.  Returns how many classes, their size
 * through size.
 */
static unsigned
xinput_device_classes(
	struct x11server *server,
	const struct xinput_device *device,
	uint8_t *info,
	int msb,
	size_t *size)
{
	uint32_t label;
	uint32_t keycode;
	size_t used;
	unsigned button;
	unsigned axis;
	unsigned extent;

	/* A keyboard: its keycodes. */
	if (!device->pointer) {
		x11_write16(info, (uint16_t)XI_CLASS_KEY, msb);
		x11_write16(info + 2, (uint16_t)(2U + 256U - X11_KEYMAP_KEYCODE_FIRST), msb);
		x11_write16(info + 4, device->id, msb);
		x11_write16(info + 6, (uint16_t)(256U - X11_KEYMAP_KEYCODE_FIRST), msb);
		used = 8U;
		for (keycode = X11_KEYMAP_KEYCODE_FIRST; keycode < 256U; keycode++) {
			x11_write32(info + used, keycode, msb);
			used += 4U;
		}
		*size = used;
		return 1U;
	}

	/* A pointer's buttons: none down, then their labels. */
	x11_write16(info, (uint16_t)XI_CLASS_BUTTON, msb);
	x11_write16(info + 2, (uint16_t)(2U + 1U + XI_BUTTONS), msb);
	x11_write16(info + 4, device->id, msb);
	x11_write16(info + 6, (uint16_t)XI_BUTTONS, msb);
	x11_write32(info + 8, 0U, msb);
	used = 12U;
	for (button = 0U; button < XI_BUTTONS; button++) {
		label = x11_atom_intern(server, xinput_button_labels[button], strlen(xinput_button_labels[button]), 0);
		x11_write32(info + used, label, msb);
		used += 4U;
	}

	/* Succeeded: its axes, from 0 to the screen's last pixel, where the pointer is. */
	for (axis = 0U; axis < XI_AXES; axis++) {
		label = x11_atom_intern(server, xinput_axis_labels[axis], strlen(xinput_axis_labels[axis]), 0);
		extent = server->width - 1U;
		if (axis == 1U)
			extent = server->height - 1U;
		x11_write16(info + used, (uint16_t)XI_CLASS_VALUATOR, msb);
		x11_write16(info + used + 2U, 11U, msb);
		x11_write16(info + used + 4U, device->id, msb);
		x11_write16(info + used + 6U, (uint16_t)axis, msb);
		x11_write32(info + used + 8U, label, msb);
		x11_write32(info + used + 12U, 0U, msb);
		x11_write32(info + used + 16U, 0U, msb);
		x11_write32(info + used + 20U, extent, msb);
		x11_write32(info + used + 24U, 0U, msb);
		x11_write32(info + used + 28U, (uint32_t)server->pointer_x, msb);
		if (axis == 1U)
			x11_write32(info + used + 28U, (uint32_t)server->pointer_y, msb);
		x11_write32(info + used + 32U, 0U, msb);
		x11_write32(info + used + 36U, 0U, msb);
		info[used + 40U] = (uint8_t)XI_MODE_ABSOLUTE;
		info[used + 41U] = 0U;
		info[used + 42U] = 0U;
		info[used + 43U] = 0U;
		used += 44U;
	}
	*size = used;
	return 1U + XI_AXES;
}

/* Returns the device of an id, or NULL. */
static const struct xinput_device *
xinput_device_find(
	uint16_t id)
{
	unsigned index;

	/* Each device. */
	for (index = 0U; index < XI_DEVICES; index++) {
		if (xinput_devices[index].id == id)
			return &xinput_devices[index];
	}

	/* None. */
	return NULL;
}

/* Tells whether a selection's device (all, all master devices, or one) takes a master device's events. */
static int
xinput_device_matches(
	uint16_t selected,
	uint16_t device)
{
	/* All devices, all master devices, or the device itself. */
	if (selected == XI_ALL_DEVICES)
		return 1;
	if (selected == XI_ALL_MASTER_DEVICES)
		return 1;
	if (selected == device)
		return 1;

	/* Another device. */
	return 0;
}

/* Returns the events a client selected on a window that a master device's events may be. */
static uint32_t
xinput_mask_of(
	const struct x11_window *window,
	unsigned client,
	uint16_t device)
{
	unsigned index;
	uint32_t mask;
	int matches;

	/* Each of the client's entries for the device. */
	mask = 0U;
	for (index = 0U; index < window->xi_select_count; index++) {
		if (window->xi_selects[index].client != client)
			continue;
		matches = xinput_device_matches(window->xi_selects[index].device, device);
		if (matches)
			mask |= window->xi_selects[index].mask;
	}

	/* Succeeded: the events. */
	return mask;
}

/*
 * Keeps what a client selected on a window for a device, in place of
 * what it selected before (an empty mask removes the entry).  Returns
 * zero when the entry could not be added.
 */
static int
xinput_select_set(
	struct x11_window *window,
	unsigned client,
	uint16_t device,
	uint32_t mask)
{
	struct x11_xi_select *grown;
	unsigned index;

	/* The client's entry for the device, changed or removed. */
	for (index = 0U; index < window->xi_select_count; index++) {
		if (window->xi_selects[index].client != client || window->xi_selects[index].device != device)
			continue;
		if (mask != 0U) {
			window->xi_selects[index].mask = mask;
			return 1;
		}
		window->xi_selects[index] = window->xi_selects[window->xi_select_count - 1U];
		window->xi_select_count--;
		return 1;
	}

	/* No entry, and nothing to keep. */
	if (mask == 0U)
		return 1;

	/* A new entry. */
	grown = realloc(window->xi_selects, (window->xi_select_count + 1U) * sizeof(*grown));
	if (grown == NULL)
		return 0;
	window->xi_selects = grown;

	/* Succeeded: kept. */
	grown[window->xi_select_count].client = client;
	grown[window->xi_select_count].device = device;
	grown[window->xi_select_count].mask = mask;
	window->xi_select_count++;
	return 1;
}

/*
 * Tells whether another client selected ButtonPress on a window for a
 * device that overlaps this one (all devices and all master devices
 * overlap every pointer).
 */
static int
xinput_press_taken(
	const struct x11_window *window,
	unsigned client,
	uint16_t device)
{
	const struct x11_xi_select *entry;
	unsigned index;
	int overlaps;

	/* Each other client's entry with ButtonPress. */
	for (index = 0U; index < window->xi_select_count; index++) {
		entry = &window->xi_selects[index];
		if (entry->client == client)
			continue;
		if ((entry->mask & (1U << XI_EVENT_BUTTON_PRESS)) == 0U)
			continue;

		/* Overlapping devices: the same, or either taking all. */
		overlaps = 0;
		if (entry->device == device)
			overlaps = 1;
		if (entry->device <= XI_ALL_MASTER_DEVICES || device <= XI_ALL_MASTER_DEVICES)
			overlaps = 1;
		if (overlaps)
			return 1;
	}

	/* None. */
	return 0;
}

/*
 * Sends a key, button or motion event in the extension's form, as seen
 * from a window: from the master device (the physical one its source),
 * the pointer's place, the buttons and the keyboard's state before the
 * event, and for the pointer's events its two axes.
 */
static void
xinput_send_device(
	struct x11server *server,
	struct x11_client *client,
	uint8_t type,
	uint8_t detail,
	uint32_t time,
	const struct x11_window *window,
	uint32_t child,
	uint16_t state)
{
	uint8_t event[XI_DEVICE_EVENT_SIZE + XI_AXES_SIZE];
	uint16_t device;
	uint16_t source;
	size_t length;
	int msb;

	/* The device the event comes from, and the event's length (a pointer's carries its axes). */
	msb = client->order;
	device = XI_MASTER_POINTER;
	source = XI_SLAVE_POINTER;
	length = XI_DEVICE_EVENT_SIZE + XI_AXES_SIZE;
	if (type == XI_EVENT_KEY_PRESS || type == XI_EVENT_KEY_RELEASE) {
		device = XI_MASTER_KEYBOARD;
		source = XI_SLAVE_KEYBOARD;
		length = XI_DEVICE_EVENT_SIZE;
	}

	/* The type, device, time, detail and windows. */
	memset(event, 0, sizeof(event));
	x11_write16(event + 8, type, msb);
	x11_write16(event + 10, device, msb);
	x11_write32(event + 12, time, msb);
	x11_write32(event + 16, detail, msb);
	x11_write32(event + 20, X11_ROOT_XID, msb);
	x11_write32(event + 24, window->id, msb);
	x11_write32(event + 28, child, msb);

	/* The places on the root window and in the window (16.16 fixed point). */
	x11_write32(event + 32, (uint32_t)server->pointer_x << 16, msb);
	x11_write32(event + 36, (uint32_t)server->pointer_y << 16, msb);
	x11_write32(event + 40, (uint32_t)(server->pointer_x - window->x) << 16, msb);
	x11_write32(event + 44, (uint32_t)(server->pointer_y - window->y) << 16, msb);

	/* One word of buttons, the axes' word for a pointer, the source, and the keyboard's state. */
	x11_write16(event + 48, 1U, msb);
	if (device == XI_MASTER_POINTER)
		x11_write16(event + 50, 1U, msb);
	x11_write16(event + 52, source, msb);
	xinput_write_state(event + 60, server, state, msb);
	x11_write32(event + 80, (uint32_t)((state >> 8) & 0x1fU) << 1, msb);

	/* A pointer's axes: both, at the pointer's place (32.32 fixed point). */
	if (device == XI_MASTER_POINTER) {
		x11_write32(event + 84, 0x3U, msb);
		x11_write32(event + 88, (uint32_t)server->pointer_x, msb);
		x11_write32(event + 96, (uint32_t)server->pointer_y, msb);
	}

	/* Succeeded: sent. */
	xinput_send(server, client, event, length);
}

/*
 * Sends a crossing or focus event in the extension's form: from the
 * master pointer (crossing) or keyboard (focus), the mode and detail, the
 * windows, the pointer's places, the flags, the keyboard's state and the
 * buttons.
 */
static void
xinput_send_crossing(
	struct x11server *server,
	struct x11_client *client,
	uint8_t type,
	const struct x11_window *window,
	uint8_t detail,
	uint8_t mode,
	uint32_t time,
	uint32_t child,
	uint8_t flags)
{
	uint8_t event[XI_CROSSING_EVENT_SIZE];
	uint16_t device;
	uint16_t state;
	int msb;

	/* The device the event comes from. */
	msb = client->order;
	device = XI_MASTER_POINTER;
	if (type == XI_EVENT_FOCUS_IN || type == XI_EVENT_FOCUS_OUT)
		device = XI_MASTER_KEYBOARD;

	/* The type, device, time, source, mode, detail and windows. */
	memset(event, 0, sizeof(event));
	x11_write16(event + 8, type, msb);
	x11_write16(event + 10, device, msb);
	x11_write32(event + 12, time, msb);
	x11_write16(event + 16, device, msb);
	event[18] = mode;
	event[19] = detail;
	x11_write32(event + 20, X11_ROOT_XID, msb);
	x11_write32(event + 24, window->id, msb);
	x11_write32(event + 28, child, msb);

	/* The places on the root window and in the window (16.16 fixed point). */
	x11_write32(event + 32, (uint32_t)server->pointer_x << 16, msb);
	x11_write32(event + 36, (uint32_t)server->pointer_y << 16, msb);
	x11_write32(event + 40, (uint32_t)(server->pointer_x - window->x) << 16, msb);
	x11_write32(event + 44, (uint32_t)(server->pointer_y - window->y) << 16, msb);

	/* On this screen, in the focus, one word of buttons, the keyboard's state and the buttons. */
	if ((flags & CROSSING_SAME_SCREEN) != 0U)
		event[48] = 1U;
	if ((flags & CROSSING_FOCUS) != 0U)
		event[49] = 1U;
	x11_write16(event + 50, 1U, msb);
	state = x11_input_state(server);
	xinput_write_state(event + 52, server, state, msb);
	x11_write32(event + 72, (uint32_t)((state >> 8) & 0x1fU) << 1, msb);

	/* Succeeded: sent. */
	xinput_send(server, client, event, sizeof(event));
}

/*
 * Writes the keyboard's state of a core state as the extension's events
 * carry it: the held, latched, locked and effective modifiers, then the
 * group each way (XKEYBOARD's split).
 */
static void
xinput_write_state(
	uint8_t *bytes,
	const struct x11server *server,
	uint16_t state,
	int msb)
{
	struct x11_xkb_state xkb;

	/* XKEYBOARD's state. */
	x11_xkb_state_of(server, state, &xkb);

	/* Succeeded: the modifiers (none latched), and the group (held, not latched or locked). */
	x11_write32(bytes, xkb.base, msb);
	x11_write32(bytes + 4, 0U, msb);
	x11_write32(bytes + 8, xkb.locked, msb);
	x11_write32(bytes + 12, xkb.mods, msb);
	bytes[16] = xkb.group;
	bytes[17] = 0U;
	bytes[18] = 0U;
	bytes[19] = xkb.group;
}

/*
 * Sends one of the extension's events, its header filled in: GenericEvent,
 * the extension's major opcode, the client's sequence number and the
 * length in words after 32 bytes.
 */
static void
xinput_send(
	struct x11server *server,
	struct x11_client *client,
	uint8_t *event,
	size_t length)
{
	/* The header. */
	event[0] = (uint8_t)GENERIC_EVENT;
	event[1] = (uint8_t)X11_XINPUT_MAJOR;
	x11_write16(event + 2, client->sequence, client->order);
	x11_write32(event + 4, (uint32_t)((length - 32U) / 4U), client->order);

	/* Succeeded: queued. */
	x11_client_send(server, client, event, length);
}

/* Tells whether an id is None or a window. */
static int
xinput_window_or_none(
	struct x11server *server,
	uint32_t id)
{
	struct x11_window *window;

	/* None. */
	if (id == 0U)
		return 1;

	/* A window, or not. */
	window = x11_window_find(server, id);
	if (window == NULL)
		return 0;

	/* Succeeded: a window. */
	return 1;
}
