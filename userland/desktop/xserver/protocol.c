/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X11 core protocol requests the server answers, and the replies,
 * events and errors it sends.
 *
 * Wire layouts follow the public X11 core protocol.  A window's position
 * is kept on the root window (absolute); a request and a reply name it
 * relative to its parent, as the protocol does.  Requests the server does
 * not know are refused with BadRequest.
 */

#include "userland/desktop/xserver/internal.h"

#include "include/kern/os-name.h"

#include <stdlib.h>
#include <string.h>

/* The core requests answered. */
#define REQUEST_CREATE_WINDOW		1U
#define REQUEST_CHANGE_ATTRIBUTES	2U
#define REQUEST_DESTROY_WINDOW		4U
#define REQUEST_REPARENT_WINDOW		7U
#define REQUEST_MAP_WINDOW		8U
#define REQUEST_UNMAP_WINDOW		10U
#define REQUEST_CONFIGURE_WINDOW	12U
#define REQUEST_GET_GEOMETRY		14U
#define REQUEST_QUERY_TREE		15U
#define REQUEST_INTERN_ATOM		16U
#define REQUEST_GET_ATOM_NAME		17U
#define REQUEST_CHANGE_PROPERTY		18U
#define REQUEST_DELETE_PROPERTY		19U
#define REQUEST_GET_PROPERTY		20U
#define REQUEST_SET_SELECTION_OWNER	22U
#define REQUEST_GET_SELECTION_OWNER	23U
#define REQUEST_CONVERT_SELECTION	24U
#define REQUEST_SEND_EVENT		25U
#define REQUEST_QUERY_POINTER		38U
#define REQUEST_SET_INPUT_FOCUS		42U
#define REQUEST_GET_INPUT_FOCUS		43U
#define REQUEST_OPEN_FONT		45U
#define REQUEST_CLOSE_FONT		46U
#define REQUEST_QUERY_FONT		47U
#define REQUEST_LIST_FONTS		49U
#define REQUEST_CREATE_PIXMAP		53U
#define REQUEST_FREE_PIXMAP		54U
#define REQUEST_CREATE_GC		55U
#define REQUEST_CHANGE_GC		56U
#define REQUEST_FREE_GC			60U
#define REQUEST_COPY_AREA		62U
#define REQUEST_POLY_LINE		65U
#define REQUEST_POLY_FILL_RECTANGLE	70U
#define REQUEST_IMAGE_TEXT8		76U
#define REQUEST_IMAGE_TEXT16		77U
#define REQUEST_QUERY_EXTENSION		98U
#define REQUEST_GET_KEYBOARD_MAPPING	101U
#define REQUEST_GET_POINTER_MAPPING	117U
#define REQUEST_NO_OPERATION		127U

/* zedBSD's own: an RGB image of a window's or a pixmap's rectangle, three bytes a pixel. */
#define REQUEST_PUT_IMAGE_RGB24		128U

/* The value-mask bits of the window attributes, the configuration and the graphics context kept. */
#define ATTRIBUTE_BACKGROUND_PIXEL	1U
#define ATTRIBUTE_EVENT_MASK		11U
#define CONFIGURE_X			0U
#define CONFIGURE_Y			1U
#define CONFIGURE_WIDTH			2U
#define CONFIGURE_HEIGHT		3U
#define CONFIGURE_BORDER		4U
#define CONFIGURE_STACK_MODE		6U
#define STACK_ABOVE			0U
#define GC_FOREGROUND			2U
#define GC_FONT				14U

/* A new window's colour when its creator gives none, and the default foreground. */
#define WINDOW_BACKGROUND		0x607080U
#define GC_DEFAULT_FOREGROUND		0xffffffU

/* The one core font's name, and the vendor the setup names. */
#define FONT_NAME			"zed-unicode"
#define SERVER_VENDOR			ZEDBSD_OS_NAME " X11"

/* The most keycodes one GetKeyboardMapping answers. */
#define KEYBOARD_MAPPING_MAX		248U

static unsigned protocol_create_window(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_change_attributes(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_reparent_window(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_map_window(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_unmap_window(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_configure_window(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_get_geometry(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_query_tree(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_change_property(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_get_property(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_query_pointer(struct x11server *server, unsigned index);
static unsigned protocol_get_input_focus(struct x11server *server, unsigned index);
static unsigned protocol_open_font(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_close_font(struct x11server *server, unsigned index, const uint8_t *request);
static unsigned protocol_query_font(struct x11server *server, unsigned index);
static unsigned protocol_list_fonts(struct x11server *server, unsigned index);
static unsigned protocol_create_pixmap(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_free_pixmap(struct x11server *server, unsigned index, const uint8_t *request);
static unsigned protocol_create_gc(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_change_gc(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_free_gc(struct x11server *server, unsigned index, const uint8_t *request);
static unsigned protocol_copy_area(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_poly_line(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_poly_fill_rectangle(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_image_text(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_get_keyboard_mapping(struct x11server *server, unsigned index, const uint8_t *request);
static unsigned protocol_get_pointer_mapping(struct x11server *server, unsigned index);
static unsigned protocol_put_image_rgb24(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static void protocol_gc_values(struct x11_gc *gc, const struct x11_client *client, const uint8_t *request, size_t length, size_t offset, uint32_t mask);
static void protocol_line(struct x11_window *window, int from_x, int from_y, int to_x, int to_y, uint32_t color);
static void protocol_move_descendants(struct x11server *server, uint32_t parent, int dx, int dy);
static void protocol_error(struct x11server *server, struct x11_client *client, unsigned code, uint32_t resource, uint8_t opcode);
static void protocol_map_request(struct x11server *server, struct x11_window *parent, struct x11_window *window);
static int protocol_parent_x(struct x11server *server, const struct x11_window *window);
static int protocol_parent_y(struct x11server *server, const struct x11_window *window);

/*
 * Answers a client's connection setup: one screen, 24-bit TrueColor, and
 * the range of resource ids the client may choose.  Returns 0, or -1 when
 * the reply cannot be queued.
 */
int
x11_setup_reply(
	struct x11server *server,
	struct x11_client *client)
{
	uint8_t reply[8 + 32 + 12 + 8 + 40 + 8 + 24];
	uint8_t *field;
	int msb;

	/* A successful setup of protocol 11.0, its length in words after the first eight bytes. */
	msb = client->order;
	memset(reply, 0, sizeof(reply));
	reply[0] = 1U;
	x11_write16(reply + 2, 11U, msb);
	x11_write16(reply + 4, 0U, msb);
	x11_write16(reply + 6, (uint16_t)((sizeof(reply) - 8U) / 4U), msb);

	/* The fixed part: release, the client's ids, motion buffer, vendor length, request length, one screen and one format. */
	field = reply + 8;
	x11_write32(field, 1U, msb);
	x11_write32(field + 4, client->base, msb);
	x11_write32(field + 8, 0x003fffffU, msb);
	x11_write16(field + 16, (uint16_t)(sizeof(SERVER_VENDOR) - 1U), msb);
	x11_write16(field + 18, 65535U, msb);
	field[20] = 1U;
	field[21] = 1U;
	field[22] = (uint8_t)msb;
	field[23] = 1U;
	field[24] = 32U;
	field[25] = 32U;
	field[26] = 8U;
	field[27] = 255U;

	/* The vendor, padded to a word. */
	field += 32;
	memcpy(field, SERVER_VENDOR, sizeof(SERVER_VENDOR) - 1U);

	/* The one pixmap format: depth 24, 32 bits a pixel, rows padded to 32 bits. */
	field += 12;
	field[0] = 24U;
	field[1] = 32U;
	field[2] = 32U;

	/* The screen: the root window, its colormap, white and black, its size in pixels and millimetres. */
	field += 8;
	x11_write32(field, X11_ROOT_XID, msb);
	x11_write32(field + 4, X11_COLORMAP_XID, msb);
	x11_write32(field + 8, 0xffffffU, msb);
	x11_write32(field + 12, 0U, msb);
	x11_write32(field + 16, 0x00ffffffU, msb);
	x11_write16(field + 20, (uint16_t)server->width, msb);
	x11_write16(field + 22, (uint16_t)server->height, msb);
	x11_write16(field + 24, (uint16_t)(server->width * 254U / 96U / 10U), msb);
	x11_write16(field + 26, (uint16_t)(server->height * 254U / 96U / 10U), msb);

	/* One colormap installed at most and at least, the root visual, no backing store, depth 24, one depth. */
	x11_write16(field + 28, 1U, msb);
	x11_write16(field + 30, 1U, msb);
	x11_write32(field + 32, X11_VISUAL_XID, msb);
	field[36] = 0U;
	field[38] = 24U;
	field[39] = 1U;

	/* The depth 24 with its one visual. */
	field += 40;
	field[0] = 24U;
	x11_write16(field + 2, 1U, msb);

	/* The visual: TrueColor, 8 bits a channel, red, green and blue masks. */
	field += 8;
	x11_write32(field, X11_VISUAL_XID, msb);
	field[4] = 4U;
	field[5] = 8U;
	x11_write16(field + 6, 256U, msb);
	x11_write32(field + 8, 0x00ff0000U, msb);
	x11_write32(field + 12, 0x0000ff00U, msb);
	x11_write32(field + 16, 0x000000ffU, msb);

	/* The reply, queued; a client that is already broken cannot take it. */
	x11_client_send(server, client, reply, sizeof(reply));
	if (client->broken)
		return -1;

	/* Succeeded: the client may send requests. */
	return 0;
}

/*
 * Handles one whole request of a client: the request's sequence number
 * moves on, and a request that fails is answered with its error.
 */
void
x11_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint32_t resource;
	unsigned code;
	uint8_t opcode;

	/* The request is the client's next. */
	client = &server->clients[index];
	client->sequence++;
	opcode = request[0];

	/* Every request but the three of one word names something in its second word. */
	if (length < 8U &&
	    opcode != REQUEST_GET_INPUT_FOCUS &&
	    opcode != REQUEST_GET_POINTER_MAPPING &&
	    opcode != REQUEST_NO_OPERATION) {
		protocol_error(server, client, X11_BAD_LENGTH, 0U, opcode);
		return;
	}

	/* The request's own handler; a nonzero code is the error to send. */
	switch (opcode) {
	case REQUEST_CREATE_WINDOW:
		code = protocol_create_window(server, index, request, length);
		break;
	case REQUEST_CHANGE_ATTRIBUTES:
		code = protocol_change_attributes(server, index, request, length);
		break;
	case REQUEST_DESTROY_WINDOW:
		x11_window_destroy(server, x11_read32(request + 4, client->order));
		code = 0U;
		break;
	case REQUEST_REPARENT_WINDOW:
		code = protocol_reparent_window(server, index, request, length);
		break;
	case REQUEST_MAP_WINDOW:
		code = protocol_map_window(server, index, request, length);
		break;
	case REQUEST_UNMAP_WINDOW:
		code = protocol_unmap_window(server, index, request, length);
		break;
	case REQUEST_CONFIGURE_WINDOW:
		code = protocol_configure_window(server, index, request, length);
		break;
	case REQUEST_GET_GEOMETRY:
		code = protocol_get_geometry(server, index, request, length);
		break;
	case REQUEST_QUERY_TREE:
		code = protocol_query_tree(server, index, request, length);
		break;
	case REQUEST_INTERN_ATOM:
		code = x11_request_intern_atom(server, index, request, length);
		break;
	case REQUEST_GET_ATOM_NAME:
		code = x11_request_get_atom_name(server, index, request, length);
		break;
	case REQUEST_CHANGE_PROPERTY:
		code = protocol_change_property(server, index, request, length);
		break;
	case REQUEST_DELETE_PROPERTY:
		code = x11_request_delete_property(server, index, request, length);
		break;
	case REQUEST_GET_PROPERTY:
		code = protocol_get_property(server, index, request, length);
		break;
	case REQUEST_SET_SELECTION_OWNER:
		code = x11_request_set_selection_owner(server, index, request, length);
		break;
	case REQUEST_GET_SELECTION_OWNER:
		code = x11_request_get_selection_owner(server, index, request, length);
		break;
	case REQUEST_CONVERT_SELECTION:
		code = x11_request_convert_selection(server, index, request, length);
		break;
	case REQUEST_SEND_EVENT:
		code = x11_request_send_event(server, index, request, length);
		break;
	case REQUEST_QUERY_POINTER:
		code = protocol_query_pointer(server, index);
		break;
	case REQUEST_SET_INPUT_FOCUS:
		server->focus = x11_read32(request + 4, client->order);
		code = 0U;
		break;
	case REQUEST_GET_INPUT_FOCUS:
		code = protocol_get_input_focus(server, index);
		break;
	case REQUEST_OPEN_FONT:
		code = protocol_open_font(server, index, request, length);
		break;
	case REQUEST_CLOSE_FONT:
		code = protocol_close_font(server, index, request);
		break;
	case REQUEST_QUERY_FONT:
		code = protocol_query_font(server, index);
		break;
	case REQUEST_LIST_FONTS:
		code = protocol_list_fonts(server, index);
		break;
	case REQUEST_CREATE_PIXMAP:
		code = protocol_create_pixmap(server, index, request, length);
		break;
	case REQUEST_FREE_PIXMAP:
		code = protocol_free_pixmap(server, index, request);
		break;
	case REQUEST_CREATE_GC:
		code = protocol_create_gc(server, index, request, length);
		break;
	case REQUEST_CHANGE_GC:
		code = protocol_change_gc(server, index, request, length);
		break;
	case REQUEST_FREE_GC:
		code = protocol_free_gc(server, index, request);
		break;
	case REQUEST_COPY_AREA:
		code = protocol_copy_area(server, index, request, length);
		break;
	case REQUEST_POLY_LINE:
		code = protocol_poly_line(server, index, request, length);
		break;
	case REQUEST_POLY_FILL_RECTANGLE:
		code = protocol_poly_fill_rectangle(server, index, request, length);
		break;
	case REQUEST_IMAGE_TEXT8:
	case REQUEST_IMAGE_TEXT16:
		code = protocol_image_text(server, index, request, length);
		break;
	case REQUEST_QUERY_EXTENSION:
		x11_glx_query_extension(server, client, request, length);
		code = 0U;
		break;
	case REQUEST_GET_KEYBOARD_MAPPING:
		code = protocol_get_keyboard_mapping(server, index, request);
		break;
	case REQUEST_GET_POINTER_MAPPING:
		code = protocol_get_pointer_mapping(server, index);
		break;
	case REQUEST_NO_OPERATION:
		code = 0U;
		break;
	case REQUEST_PUT_IMAGE_RGB24:
		code = protocol_put_image_rgb24(server, index, request, length);
		break;
	case X11_GLX_MAJOR:
		code = x11_glx_request(server, client, request, length);
		break;
	default:
		code = X11_BAD_REQUEST;
		break;
	}

	/* A handled request is done. */
	if (code == 0U)
		return;

	/* A failed one is answered with its error, naming the resource the request names first. */
	resource = 0U;
	if (length >= 8U && code != X11_BAD_REQUEST)
		resource = x11_read32(request + 4, client->order);
	protocol_error(server, client, code, resource, opcode);
}

/*
 * Reads a 16-bit value in a client's byte order.
 */
uint16_t
x11_read16(
	const uint8_t *bytes,
	int msb)
{
	/* Most significant byte first, or last. */
	if (msb)
		return (uint16_t)(((unsigned)bytes[0] << 8) | (unsigned)bytes[1]);

	/* Succeeded: least significant byte first. */
	return (uint16_t)((unsigned)bytes[0] | ((unsigned)bytes[1] << 8));
}

/*
 * Reads a 32-bit value in a client's byte order.
 */
uint32_t
x11_read32(
	const uint8_t *bytes,
	int msb)
{
	/* Most significant byte first, or last. */
	if (msb)
		return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];

	/* Succeeded: least significant byte first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/*
 * Writes a 16-bit value in a client's byte order.
 */
void
x11_write16(
	uint8_t *bytes,
	uint16_t value,
	int msb)
{
	/* Most significant byte first, or last. */
	if (msb) {
		bytes[0] = (uint8_t)(value >> 8);
		bytes[1] = (uint8_t)value;
	} else {
		bytes[0] = (uint8_t)value;
		bytes[1] = (uint8_t)(value >> 8);
	}
}

/*
 * Writes a 32-bit value in a client's byte order.
 */
void
x11_write32(
	uint8_t *bytes,
	uint32_t value,
	int msb)
{
	/* Most significant byte first, or last. */
	if (msb) {
		bytes[0] = (uint8_t)(value >> 24);
		bytes[1] = (uint8_t)(value >> 16);
		bytes[2] = (uint8_t)(value >> 8);
		bytes[3] = (uint8_t)value;
	} else {
		bytes[0] = (uint8_t)value;
		bytes[1] = (uint8_t)(value >> 8);
		bytes[2] = (uint8_t)(value >> 16);
		bytes[3] = (uint8_t)(value >> 24);
	}
}

/*
 * Sends a reply: its type, the request's sequence number and its length
 * in words after the first 32 bytes are filled in.
 */
void
x11_reply(
	struct x11server *server,
	struct x11_client *client,
	uint8_t *reply,
	size_t length)
{
	/* The header. */
	reply[0] = 1U;
	x11_write16(reply + 2, client->sequence, client->order);
	if (length >= 32U)
		x11_write32(reply + 4, (uint32_t)((length - 32U) / 4U), client->order);

	/* Queued for the client. */
	x11_client_send(server, client, reply, length);
}

/*
 * Sends a key, button or motion event to a window's owner: the pointer's
 * place on the root window and in the window, and the state.
 */
void
x11_input_event(
	struct x11server *server,
	unsigned owner,
	uint8_t type,
	uint32_t window,
	uint8_t detail,
	uint32_t time,
	uint16_t state)
{
	struct x11_client *client;
	struct x11_window *target;
	uint8_t event[32];
	int local_x;
	int local_y;
	int msb;

	/* An owner that is gone hears nothing. */
	client = x11_client_of(server, owner);
	if (client == NULL)
		return;

	/* The pointer's place in the window (on the root window when the window is gone). */
	local_x = server->pointer_x;
	local_y = server->pointer_y;
	target = x11_window_find(server, window);
	if (target != NULL) {
		local_x -= target->x;
		local_y -= target->y;
	}

	/* The event: type, detail, time, root, window, no child, both places, the state, on this screen. */
	msb = client->order;
	memset(event, 0, sizeof(event));
	event[0] = type;
	event[1] = detail;
	x11_write16(event + 2, client->sequence, msb);
	x11_write32(event + 4, time, msb);
	x11_write32(event + 8, X11_ROOT_XID, msb);
	x11_write32(event + 12, window, msb);
	x11_write32(event + 16, 0U, msb);
	x11_write16(event + 20, (uint16_t)server->pointer_x, msb);
	x11_write16(event + 22, (uint16_t)server->pointer_y, msb);
	x11_write16(event + 24, (uint16_t)local_x, msb);
	x11_write16(event + 26, (uint16_t)local_y, msb);
	x11_write16(event + 28, state, msb);
	event[30] = 1U;

	/* Queued for the client. */
	x11_client_send(server, client, event, sizeof(event));
}

/*
 * Tells a window's owner that all of the window must be drawn again, and
 * its configuration.
 */
void
x11_expose(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_client *client;
	uint8_t event[32];
	int msb;

	/* The owner asked for exposures. */
	client = x11_client_of(server, window->owner);
	if (client != NULL && (window->event_mask & X11_MASK_EXPOSURE) != 0U) {
		/* The whole window, and no more exposures follow. */
		msb = client->order;
		memset(event, 0, sizeof(event));
		event[0] = X11_EVENT_EXPOSE;
		x11_write16(event + 2, client->sequence, msb);
		x11_write32(event + 4, window->id, msb);
		x11_write16(event + 8, 0U, msb);
		x11_write16(event + 10, 0U, msb);
		x11_write16(event + 12, window->width, msb);
		x11_write16(event + 14, window->height, msb);
		x11_client_send(server, client, event, sizeof(event));
	}

	/* Its configuration too. */
	x11_configure_notify(server, window);
}

/*
 * Tells a window's owner the window's position (relative to its parent),
 * size and border, when it asked for structure events.
 */
void
x11_configure_notify(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_client *client;
	uint8_t event[32];
	int msb;

	/* The owner asked for structure events. */
	client = x11_client_of(server, window->owner);
	if (client == NULL)
		return;
	if ((window->event_mask & X11_MASK_STRUCTURE_NOTIFY) == 0U)
		return;

	/* The event: the window about itself, above no sibling. */
	msb = client->order;
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_CONFIGURE_NOTIFY;
	x11_write16(event + 2, client->sequence, msb);
	x11_write32(event + 4, window->id, msb);
	x11_write32(event + 8, window->id, msb);
	x11_write32(event + 12, 0U, msb);
	x11_write16(event + 16, (uint16_t)(window->x - protocol_parent_x(server, window)), msb);
	x11_write16(event + 18, (uint16_t)(window->y - protocol_parent_y(server, window)), msb);
	x11_write16(event + 20, window->width, msb);
	x11_write16(event + 22, window->height, msb);
	x11_write16(event + 24, window->border, msb);

	/* Queued for the client. */
	x11_client_send(server, client, event, sizeof(event));
}

/*
 * Tells a client that a window is destroyed, as seen from a window it
 * watches (the window itself, or its parent).
 */
void
x11_destroy_notify(
	struct x11server *server,
	unsigned owner,
	uint32_t event_window,
	uint32_t window)
{
	struct x11_client *client;
	uint8_t event[32];
	int msb;

	/* An owner that is gone hears nothing. */
	client = x11_client_of(server, owner);
	if (client == NULL)
		return;

	/* The event. */
	msb = client->order;
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_DESTROY_NOTIFY;
	x11_write16(event + 2, client->sequence, msb);
	x11_write32(event + 4, event_window, msb);
	x11_write32(event + 8, window, msb);

	/* Queued for the client. */
	x11_client_send(server, client, event, sizeof(event));
}

/* CreateWindow: a window under a parent, with its background and events; its position is kept on the root window. */
static unsigned
protocol_create_window(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *parent;
	struct x11_window *window;
	uint32_t mask;
	uint32_t value;
	uint32_t id;
	size_t offset;
	unsigned bit;
	int msb;

	/* The fixed part must be there, and the table must have room. */
	client = &server->clients[index];
	msb = client->order;
	if (length < 32U)
		return X11_BAD_LENGTH;
	if (server->window_count == X11_MAX_WINDOWS)
		return X11_BAD_ALLOC;

	/* An id in use is refused. */
	id = x11_read32(request + 4, msb);
	window = x11_window_find(server, id);
	if (window != NULL)
		return X11_BAD_ID_CHOICE;

	/* The parent must exist. */
	parent = x11_window_find(server, x11_read32(request + 8, msb));
	if (parent == NULL)
		return X11_BAD_WINDOW;

	/* The window, on top of the stack, at its place on the root window. */
	window = &server->windows[server->window_count];
	memset(window, 0, sizeof(*window));
	window->id = id;
	window->owner = index;
	window->parent = parent->id;
	window->x = (int16_t)(parent->x + (int16_t)x11_read16(request + 12, msb));
	window->y = (int16_t)(parent->y + (int16_t)x11_read16(request + 14, msb));
	window->width = x11_read16(request + 16, msb);
	window->height = x11_read16(request + 18, msb);
	window->border = x11_read16(request + 20, msb);
	window->background = WINDOW_BACKGROUND;

	/* The values the mask names, in bit order: the background and the event mask are kept. */
	mask = x11_read32(request + 28, msb);
	offset = 32U;
	for (bit = 0U; bit < 32U && offset + 4U <= length; bit++) {
		if ((mask & (1U << bit)) == 0U)
			continue;

		/* The value of this bit. */
		value = x11_read32(request + offset, msb);
		offset += 4U;
		if (bit == ATTRIBUTE_BACKGROUND_PIXEL)
			window->background = value;
		if (bit == ATTRIBUTE_EVENT_MASK)
			window->event_mask = value;
	}

	/* Its pixels, cleared to its background. */
	window->pixels = x11_pixels_alloc(window->width, window->height, window->background);
	if (window->pixels == NULL) {
		memset(window, 0, sizeof(*window));
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the window is in the table. */
	server->window_count++;
	return 0U;
}

/* ChangeWindowAttributes: the background and the event mask; the root window's events go to whoever selects them. */
static unsigned
protocol_change_attributes(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	uint32_t mask;
	uint32_t value;
	size_t offset;
	unsigned bit;
	int msb;

	/* The window must exist. */
	msb = server->clients[index].order;
	if (length < 12U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* The values the mask names, in bit order. */
	mask = x11_read32(request + 8, msb);
	offset = 12U;
	for (bit = 0U; bit < 32U && offset + 4U <= length; bit++) {
		if ((mask & (1U << bit)) == 0U)
			continue;

		/* The value of this bit. */
		value = x11_read32(request + offset, msb);
		offset += 4U;
		if (bit == ATTRIBUTE_BACKGROUND_PIXEL)
			window->background = value;

		/* The event mask, and the root window's events belong to the client that selects them. */
		if (bit == ATTRIBUTE_EVENT_MASK) {
			window->event_mask = value;
			if (window->id == X11_ROOT_XID)
				window->owner = index;
		}
	}

	/* Succeeded: the attributes are changed. */
	return 0U;
}

/* ReparentWindow: the window under a new parent, at a place relative to it. */
static unsigned
protocol_reparent_window(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_window *parent;
	int16_t x;
	int16_t y;
	int msb;

	/* The window and the new parent must exist. */
	msb = server->clients[index].order;
	if (length < 16U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;
	parent = x11_window_find(server, x11_read32(request + 8, msb));
	if (parent == NULL)
		return X11_BAD_WINDOW;

	/* Its new place on the root window; its descendants move with it. */
	x = (int16_t)(parent->x + (int16_t)x11_read16(request + 12, msb));
	y = (int16_t)(parent->y + (int16_t)x11_read16(request + 14, msb));
	protocol_move_descendants(server, window->id, x - window->x, y - window->y);
	window->parent = parent->id;
	window->x = x;
	window->y = y;
	x11_mark_dirty(server, window->x, window->y, window->width, window->height);

	/* Succeeded: the window has its new parent. */
	return 0U;
}

/* MapWindow: shown (or asked of the client that redirects its parent's children). */
static unsigned
protocol_map_window(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_window *parent;
	int msb;

	/* The window must exist. */
	msb = server->clients[index].order;
	if (length < 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* Another client that redirects the parent's children is asked instead. */
	parent = x11_window_find(server, window->parent);
	if (parent != NULL &&
	    (parent->event_mask & X11_MASK_SUBSTRUCTURE_REDIRECT) != 0U &&
	    parent->owner != index) {
		protocol_map_request(server, parent, window);
		return 0U;
	}

	/* Shown, and its owner draws it. */
	window->mapped = 1;
	x11_mark_dirty(server, window->x, window->y, window->width, window->height);
	x11_expose(server, window);

	/* Succeeded: the window is mapped. */
	return 0U;
}

/* UnmapWindow: hidden. */
static unsigned
protocol_unmap_window(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;

	/* The window must exist. */
	if (length < 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, server->clients[index].order));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* Hidden; what was under it is shown again. */
	window->mapped = 0;
	x11_mark_dirty(server, window->x, window->y, window->width, window->height);

	/* Succeeded: the window is unmapped. */
	return 0U;
}

/* ConfigureWindow: a new place (relative to the parent), size and border, and raised when asked. */
static unsigned
protocol_configure_window(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	uint16_t mask;
	uint32_t value;
	size_t offset;
	unsigned bit;
	int old_x;
	int old_y;
	int old_width;
	int old_height;
	int new_x;
	int new_y;
	uint16_t new_width;
	uint16_t new_height;
	uint16_t new_border;
	int raise;
	int failed;
	int msb;

	/* The window must exist. */
	msb = server->clients[index].order;
	if (length < 12U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* Where it is now, and what it becomes unless the request says otherwise. */
	old_x = window->x;
	old_y = window->y;
	old_width = window->width;
	old_height = window->height;
	new_x = window->x;
	new_y = window->y;
	new_width = window->width;
	new_height = window->height;
	new_border = window->border;
	raise = 0;

	/* The values the mask names, in bit order; the place is relative to the parent. */
	mask = x11_read16(request + 8, msb);
	offset = 12U;
	for (bit = 0U; bit < 7U && offset + 4U <= length; bit++) {
		if ((mask & (1U << bit)) == 0U)
			continue;

		/* The value of this bit. */
		value = x11_read32(request + offset, msb);
		offset += 4U;
		if (bit == CONFIGURE_X) {
			new_x = protocol_parent_x(server, window) + (int16_t)value;
		} else if (bit == CONFIGURE_Y) {
			new_y = protocol_parent_y(server, window) + (int16_t)value;
		} else if (bit == CONFIGURE_WIDTH) {
			new_width = (uint16_t)value;
		} else if (bit == CONFIGURE_HEIGHT) {
			new_height = (uint16_t)value;
		} else if (bit == CONFIGURE_BORDER) {
			new_border = (uint16_t)value;
		} else if (bit == CONFIGURE_STACK_MODE && value == STACK_ABOVE) {
			raise = 1;
		}
	}

	/* Pixels of the new size, keeping what fits. */
	failed = x11_window_resize(window, new_width, new_height);
	if (failed != 0)
		return X11_BAD_ALLOC;

	/* The new configuration; the descendants move with the window. */
	protocol_move_descendants(server, window->id, new_x - old_x, new_y - old_y);
	window->x = (int16_t)new_x;
	window->y = (int16_t)new_y;
	window->width = new_width;
	window->height = new_height;
	window->border = new_border;
	x11_mark_dirty(server, old_x, old_y, old_width, old_height);
	x11_mark_dirty(server, window->x, window->y, window->width, window->height);

	/* A new size is drawn again by the owner. */
	if (new_width != (uint16_t)old_width || new_height != (uint16_t)old_height)
		x11_expose(server, window);

	/* Raised with its top-level window, when asked. */
	if (raise)
		x11_window_raise(server, x11_window_top_level(server, window));

	/* Succeeded: the window is configured. */
	return 0U;
}

/* GetGeometry: the root, the place relative to the parent, the size and the border. */
static unsigned
protocol_get_geometry(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *window;
	uint8_t reply[32];
	int msb;

	/* The window must exist. */
	client = &server->clients[index];
	msb = client->order;
	if (length < 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* The reply: depth 24, the root, and the geometry. */
	memset(reply, 0, sizeof(reply));
	reply[1] = 24U;
	x11_write32(reply + 8, X11_ROOT_XID, msb);
	x11_write16(reply + 12, (uint16_t)(window->x - protocol_parent_x(server, window)), msb);
	x11_write16(reply + 14, (uint16_t)(window->y - protocol_parent_y(server, window)), msb);
	x11_write16(reply + 16, window->width, msb);
	x11_write16(reply + 18, window->height, msb);
	x11_write16(reply + 20, window->border, msb);
	x11_reply(server, client, reply, sizeof(reply));

	/* Succeeded: answered. */
	return 0U;
}

/* QueryTree: the root, the parent and the children in stacking order, bottom first. */
static unsigned
protocol_query_tree(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32 + X11_MAX_WINDOWS * 4];
	struct x11_client *client;
	struct x11_window *window;
	unsigned count;
	unsigned slot;
	int msb;

	/* The window must exist. */
	client = &server->clients[index];
	msb = client->order;
	if (length < 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* The root and the parent. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, X11_ROOT_XID, msb);
	x11_write32(reply + 12, window->parent, msb);

	/* Each child, in the table's (stacking) order. */
	count = 0U;
	for (slot = 1U; slot < server->window_count; slot++) {
		if (server->windows[slot].parent != window->id)
			continue;
		x11_write32(reply + 32 + count * 4U, server->windows[slot].id, msb);
		count++;
	}

	/* The reply with the count. */
	x11_write16(reply + 16, (uint16_t)count, msb);
	x11_reply(server, client, reply, 32U + count * 4U);

	/* Succeeded: answered. */
	return 0U;
}

/* ChangeProperty: WM_NAME and the icon path are kept as strings; other properties are ignored. */
static unsigned
protocol_change_property(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	uint32_t property;
	uint32_t type;
	uint32_t count;
	size_t capacity;
	size_t copied;
	char *target;
	unsigned code;
	int msb;

	/* The window must exist. */
	msb = server->clients[index].order;
	if (length < 24U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* Which string is kept, if any. */
	property = x11_read32(request + 8, msb);
	type = x11_read32(request + 12, msb);
	count = x11_read32(request + 20, msb);
	target = NULL;
	capacity = 0U;
	if (property == X11_ATOM_WM_NAME) {
		target = window->name;
		capacity = sizeof(window->name);
	} else if (property == X11_ATOM_ICON_PATH) {
		target = window->icon_path;
		capacity = sizeof(window->icon_path);
	}

	/* Any other property is kept whole with the window (selection.c). */
	if (target == NULL) {
		code = x11_request_change_property(server, index, request, length);
		return code;
	}

	/* An 8-bit STRING that fits the request is kept, cut to the room there is. */
	if (target != NULL &&
	    type == X11_ATOM_STRING &&
	    request[16] == 8U &&
	    count <= length - 24U) {
		copied = count;
		if (copied > capacity - 1U)
			copied = capacity - 1U;
		memcpy(target, request + 24, copied);
		target[copied] = '\0';
	}

	/* Succeeded: the property is changed (or ignored). */
	return 0U;
}

/* GetProperty: WM_NAME and the icon path as 8-bit STRINGs; anything else has no value. */
static unsigned
protocol_get_property(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32 + 160];
	struct x11_client *client;
	struct x11_window *window;
	const char *value;
	uint32_t property;
	uint32_t type;
	size_t count;
	size_t padded;
	unsigned code;
	int msb;

	/* The window must exist. */
	client = &server->clients[index];
	msb = client->order;
	if (length < 24U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* Which string is asked for. */
	property = x11_read32(request + 8, msb);
	type = x11_read32(request + 12, msb);
	value = NULL;
	if (property == X11_ATOM_WM_NAME)
		value = window->name;
	else if (property == X11_ATOM_ICON_PATH)
		value = window->icon_path;

	/* Any other property is one kept whole with the window (selection.c). */
	if (value == NULL) {
		code = x11_request_get_property(server, index, request, length);
		return code;
	}

	/* A set string of any type or STRING is given whole; otherwise the property has no value. */
	memset(reply, 0, sizeof(reply));
	padded = 0U;
	if (value != NULL &&
	    value[0] != '\0' &&
	    (type == 0U || type == X11_ATOM_STRING)) {
		count = strlen(value);
		padded = (count + 3U) & ~(size_t)3U;
		reply[1] = 8U;
		x11_write32(reply + 8, X11_ATOM_STRING, msb);
		x11_write32(reply + 16, (uint32_t)count, msb);
		memcpy(reply + 32, value, count);
	}

	/* The reply. */
	x11_reply(server, client, reply, 32U + padded);

	/* Succeeded: answered. */
	return 0U;
}

/* QueryPointer: the pointer on the root window, the child it is in, its place there, and the modifiers. */
static unsigned
protocol_query_pointer(
	struct x11server *server,
	unsigned index)
{
	struct x11_client *client;
	struct x11_window *window;
	uint8_t reply[32];
	uint32_t child;
	int msb;

	/* The window under the pointer (none when it is the root). */
	client = &server->clients[index];
	msb = client->order;
	window = x11_window_at(server, server->pointer_x, server->pointer_y);
	child = window->id;
	if (child == X11_ROOT_XID)
		child = 0U;

	/* The reply: on this screen. */
	memset(reply, 0, sizeof(reply));
	reply[1] = 1U;
	x11_write32(reply + 8, X11_ROOT_XID, msb);
	x11_write32(reply + 12, child, msb);
	x11_write16(reply + 16, (uint16_t)server->pointer_x, msb);
	x11_write16(reply + 18, (uint16_t)server->pointer_y, msb);
	x11_write16(reply + 20, (uint16_t)(server->pointer_x - window->x), msb);
	x11_write16(reply + 22, (uint16_t)(server->pointer_y - window->y), msb);
	x11_write16(reply + 24, server->key_state, msb);
	x11_reply(server, client, reply, sizeof(reply));

	/* Succeeded: answered. */
	return 0U;
}

/* GetInputFocus: the focus window (no revert). */
static unsigned
protocol_get_input_focus(
	struct x11server *server,
	unsigned index)
{
	struct x11_client *client;
	uint8_t reply[32];

	/* The reply. */
	client = &server->clients[index];
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, server->focus, client->order);
	x11_reply(server, client, reply, sizeof(reply));

	/* Succeeded: answered. */
	return 0U;
}

/* OpenFont: every name opens the one core font. */
static unsigned
protocol_open_font(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_font *font;
	uint16_t name_length;
	int msb;

	/* The name must fit the request, and the table must have room. */
	msb = server->clients[index].order;
	if (length < 12U)
		return X11_BAD_LENGTH;
	name_length = x11_read16(request + 8, msb);
	if (12U + (size_t)name_length > length)
		return X11_BAD_LENGTH;
	if (server->font_count == X11_MAX_FONTS)
		return X11_BAD_ALLOC;

	/* The font. */
	font = &server->fonts[server->font_count];
	font->id = x11_read32(request + 4, msb);
	font->owner = index;
	server->font_count++;

	/* Succeeded: the font is open. */
	return 0U;
}

/* CloseFont: the font leaves the table. */
static unsigned
protocol_close_font(
	struct x11server *server,
	unsigned index,
	const uint8_t *request)
{
	uint32_t id;
	unsigned slot;

	/* The font of the id. */
	id = x11_read32(request + 4, server->clients[index].order);
	for (slot = 0U; slot < server->font_count; slot++) {
		if (server->fonts[slot].id == id)
			break;
	}

	/* An unknown font is refused. */
	if (slot == server->font_count)
		return X11_BAD_WINDOW;

	/* The later fonts move down over it. */
	memmove(&server->fonts[slot], &server->fonts[slot + 1U], (server->font_count - slot - 1U) * sizeof(server->fonts[0]));
	server->font_count--;
	memset(&server->fonts[server->font_count], 0, sizeof(server->fonts[0]));

	/* Succeeded: the font is closed. */
	return 0U;
}

/* QueryFont: the core font's metrics (Unicode, 8x16 cells with 16x16 for wide characters). */
static unsigned
protocol_query_font(
	struct x11server *server,
	unsigned index)
{
	struct x11_client *client;
	uint8_t reply[60];
	int msb;

	/* The character range, the ascent and no properties or per-character metrics. */
	client = &server->clients[index];
	msb = client->order;
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, 0U, msb);
	x11_write16(reply + 10, 255U, msb);
	x11_write16(reply + 12, 0U, msb);
	x11_write16(reply + 14, 255U, msb);
	x11_write16(reply + 18, 0U, msb);
	x11_write16(reply + 20, 16U, msb);
	x11_write32(reply + 56, 0U, msb);
	x11_reply(server, client, reply, sizeof(reply));

	/* Succeeded: answered. */
	return 0U;
}

/* ListFonts: the one core font's name. */
static unsigned
protocol_list_fonts(
	struct x11server *server,
	unsigned index)
{
	struct x11_client *client;
	uint8_t reply[44];

	/* One name, as a length and its bytes. */
	client = &server->clients[index];
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, 1U, client->order);
	reply[32] = (uint8_t)(sizeof(FONT_NAME) - 1U);
	memcpy(reply + 33, FONT_NAME, sizeof(FONT_NAME) - 1U);
	x11_reply(server, client, reply, sizeof(reply));

	/* Succeeded: answered. */
	return 0U;
}

/* CreatePixmap: black pixels of a size. */
static unsigned
protocol_create_pixmap(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_pixmap *pixmap;
	int msb;

	/* The fixed part must be there, and the table must have room. */
	msb = server->clients[index].order;
	if (length < 16U)
		return X11_BAD_LENGTH;
	if (server->pixmap_count == X11_MAX_PIXMAPS)
		return X11_BAD_ALLOC;

	/* The pixmap. */
	pixmap = &server->pixmaps[server->pixmap_count];
	memset(pixmap, 0, sizeof(*pixmap));
	pixmap->id = x11_read32(request + 4, msb);
	pixmap->owner = index;
	pixmap->width = x11_read16(request + 12, msb);
	pixmap->height = x11_read16(request + 14, msb);

	/* Its pixels. */
	pixmap->pixels = x11_pixels_alloc(pixmap->width, pixmap->height, 0U);
	if (pixmap->pixels == NULL) {
		memset(pixmap, 0, sizeof(*pixmap));
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the pixmap is in the table. */
	server->pixmap_count++;
	return 0U;
}

/* FreePixmap: the pixmap and its pixels go. */
static unsigned
protocol_free_pixmap(
	struct x11server *server,
	unsigned index,
	const uint8_t *request)
{
	struct x11_pixmap *pixmap;
	size_t slot;

	/* The pixmap of the id. */
	pixmap = x11_pixmap_find(server, x11_read32(request + 4, server->clients[index].order));
	if (pixmap == NULL)
		return X11_BAD_WINDOW;

	/* Its pixels, and the later pixmaps move down over it. */
	free(pixmap->pixels);
	slot = (size_t)(pixmap - server->pixmaps);
	memmove(&server->pixmaps[slot], &server->pixmaps[slot + 1U], (server->pixmap_count - slot - 1U) * sizeof(server->pixmaps[0]));
	server->pixmap_count--;
	memset(&server->pixmaps[server->pixmap_count], 0, sizeof(server->pixmaps[0]));

	/* Succeeded: the pixmap is freed. */
	return 0U;
}

/* CreateGC: a graphics context with a white foreground unless its values say otherwise. */
static unsigned
protocol_create_gc(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_gc *gc;

	/* The fixed part must be there, and the table must have room. */
	client = &server->clients[index];
	if (length < 16U)
		return X11_BAD_LENGTH;
	if (server->gc_count == X11_MAX_GCS)
		return X11_BAD_ALLOC;

	/* The context and its values. */
	gc = &server->gcs[server->gc_count];
	memset(gc, 0, sizeof(*gc));
	gc->id = x11_read32(request + 4, client->order);
	gc->owner = index;
	gc->foreground = GC_DEFAULT_FOREGROUND;
	protocol_gc_values(gc, client, request, length, 16U, x11_read32(request + 12, client->order));

	/* Succeeded: the context is in the table. */
	server->gc_count++;
	return 0U;
}

/* ChangeGC: new values of a graphics context. */
static unsigned
protocol_change_gc(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_gc *gc;

	/* The context must exist. */
	client = &server->clients[index];
	if (length < 12U)
		return X11_BAD_LENGTH;
	gc = x11_gc_find(server, x11_read32(request + 4, client->order));
	if (gc == NULL)
		return X11_BAD_WINDOW;

	/* Its values. */
	protocol_gc_values(gc, client, request, length, 12U, x11_read32(request + 8, client->order));

	/* Succeeded: the context is changed. */
	return 0U;
}

/* FreeGC: the context leaves the table. */
static unsigned
protocol_free_gc(
	struct x11server *server,
	unsigned index,
	const uint8_t *request)
{
	struct x11_gc *gc;
	size_t slot;

	/* The context of the id. */
	gc = x11_gc_find(server, x11_read32(request + 4, server->clients[index].order));
	if (gc == NULL)
		return X11_BAD_WINDOW;

	/* The later contexts move down over it. */
	slot = (size_t)(gc - server->gcs);
	memmove(&server->gcs[slot], &server->gcs[slot + 1U], (server->gc_count - slot - 1U) * sizeof(server->gcs[0]));
	server->gc_count--;
	memset(&server->gcs[server->gc_count], 0, sizeof(server->gcs[0]));

	/* Succeeded: the context is freed. */
	return 0U;
}

/* CopyArea: a rectangle of a pixmap onto a window (the one direction clients use here). */
static unsigned
protocol_copy_area(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_pixmap *source;
	struct x11_window *target;
	int source_x;
	int source_y;
	int target_x;
	int target_y;
	int width;
	int height;
	int row;
	int column;
	int from_x;
	int from_y;
	int to_x;
	int to_y;
	int msb;

	/* The source pixmap and the target window must exist. */
	msb = server->clients[index].order;
	if (length < 28U)
		return X11_BAD_LENGTH;
	source = x11_pixmap_find(server, x11_read32(request + 4, msb));
	target = x11_window_find(server, x11_read32(request + 8, msb));
	if (source == NULL ||
	    target == NULL ||
	    target->pixels == NULL)
		return X11_BAD_WINDOW;

	/* The rectangle. */
	source_x = (int16_t)x11_read16(request + 16, msb);
	source_y = (int16_t)x11_read16(request + 18, msb);
	target_x = (int16_t)x11_read16(request + 20, msb);
	target_y = (int16_t)x11_read16(request + 22, msb);
	width = x11_read16(request + 24, msb);
	height = x11_read16(request + 26, msb);

	/* Each pixel inside both. */
	for (row = 0; row < height; row++) {
		for (column = 0; column < width; column++) {
			from_x = source_x + column;
			from_y = source_y + row;
			to_x = target_x + column;
			to_y = target_y + row;
			if (from_x < 0 ||
			    from_y < 0 ||
			    from_x >= source->width ||
			    from_y >= source->height)
				continue;
			if (to_x < 0 ||
			    to_y < 0 ||
			    to_x >= target->width ||
			    to_y >= target->height)
				continue;
			target->pixels[(size_t)to_y * target->width + (size_t)to_x] = source->pixels[(size_t)from_y * source->width + (size_t)from_x];
		}
	}

	/* The rectangle is shown again. */
	x11_mark_dirty(server, target->x + target_x, target->y + target_y, width, height);

	/* Succeeded: copied. */
	return 0U;
}

/* PolyLine: connected one-pixel lines in the context's colour. */
static unsigned
protocol_poly_line(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_gc *gc;
	uint32_t color;
	size_t offset;
	int last_x;
	int last_y;
	int x;
	int y;
	int msb;

	/* The window must exist. */
	msb = server->clients[index].order;
	if (length < 16U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL || window->pixels == NULL)
		return X11_BAD_WINDOW;

	/* The colour. */
	gc = x11_gc_find(server, x11_read32(request + 8, msb));
	color = GC_DEFAULT_FOREGROUND;
	if (gc != NULL)
		color = gc->foreground;

	/* Each point after the first ends a line from the one before. */
	last_x = 0;
	last_y = 0;
	for (offset = 12U; offset + 4U <= length; offset += 4U) {
		x = (int16_t)x11_read16(request + offset, msb);
		y = (int16_t)x11_read16(request + offset + 2U, msb);
		if (offset != 12U)
			protocol_line(window, last_x, last_y, x, y, color);
		last_x = x;
		last_y = y;
	}

	/* The whole window is shown again (lines are small and rare). */
	x11_mark_dirty(server, window->x, window->y, window->width, window->height);

	/* Succeeded: drawn. */
	return 0U;
}

/* PolyFillRectangle: rectangles of a window or a pixmap in the context's colour. */
static unsigned
protocol_poly_fill_rectangle(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_pixmap *pixmap;
	struct x11_gc *gc;
	uint32_t drawable;
	uint32_t color;
	size_t offset;
	int x;
	int y;
	int width;
	int height;
	int msb;

	/* The drawable: a window with pixels, or a pixmap. */
	msb = server->clients[index].order;
	if (length < 12U)
		return X11_BAD_LENGTH;
	drawable = x11_read32(request + 4, msb);
	window = x11_window_find(server, drawable);
	pixmap = x11_pixmap_find(server, drawable);
	if (window != NULL && window->pixels == NULL)
		window = NULL;
	if (window == NULL && pixmap == NULL)
		return X11_BAD_WINDOW;

	/* The colour. */
	gc = x11_gc_find(server, x11_read32(request + 8, msb));
	color = GC_DEFAULT_FOREGROUND;
	if (gc != NULL)
		color = gc->foreground;

	/* Each rectangle. */
	for (offset = 12U; offset + 8U <= length; offset += 8U) {
		x = (int16_t)x11_read16(request + offset, msb);
		y = (int16_t)x11_read16(request + offset + 2U, msb);
		width = x11_read16(request + offset + 4U, msb);
		height = x11_read16(request + offset + 6U, msb);
		if (window != NULL) {
			x11_window_fill(server, window, x, y, width, height, color);
		} else {
			x11_pixmap_fill(pixmap, x, y, width, height, color);
		}
	}

	/* Succeeded: filled. */
	return 0U;
}

/* ImageText8 and ImageText16: characters in the core font, from a baseline. */
static unsigned
protocol_image_text(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_gc *gc;
	size_t count;
	size_t bytes;
	int wide;
	int msb;

	/* The window must exist. */
	msb = server->clients[index].order;
	if (length < 16U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL || window->pixels == NULL)
		return X11_BAD_WINDOW;

	/* The characters must fit the request (two bytes each for ImageText16). */
	gc = x11_gc_find(server, x11_read32(request + 8, msb));
	count = request[1];
	wide = 0;
	if (request[0] == REQUEST_IMAGE_TEXT16)
		wide = 1;
	bytes = count;
	if (wide)
		bytes = count * 2U;
	if (16U + bytes > length)
		return X11_BAD_LENGTH;

	/* Drawn. */
	x11_draw_text(server, window, gc, (int16_t)x11_read16(request + 12, msb), (int16_t)x11_read16(request + 14, msb), request + 16, count, wide);

	/* Succeeded: drawn. */
	return 0U;
}

/* GetKeyboardMapping: one keysym for each keycode asked for. */
static unsigned
protocol_get_keyboard_mapping(
	struct x11server *server,
	unsigned index,
	const uint8_t *request)
{
	uint8_t reply[32 + 4 * KEYBOARD_MAPPING_MAX];
	struct x11_client *client;
	uint32_t keycode;
	unsigned count;
	unsigned slot;

	/* How many keycodes, at most what one reply holds. */
	client = &server->clients[index];
	count = request[5];
	if (count > KEYBOARD_MAPPING_MAX)
		count = KEYBOARD_MAPPING_MAX;

	/* One keysym a keycode. */
	memset(reply, 0, sizeof(reply));
	reply[1] = 1U;
	for (slot = 0U; slot < count; slot++) {
		keycode = (uint32_t)request[4] + slot;
		x11_write32(reply + 32 + slot * 4U, x11_keymap_keysym(keycode), client->order);
	}

	/* The reply. */
	x11_reply(server, client, reply, 32U + (size_t)count * 4U);

	/* Succeeded: answered. */
	return 0U;
}

/* GetPointerMapping: three buttons, each itself. */
static unsigned
protocol_get_pointer_mapping(
	struct x11server *server,
	unsigned index)
{
	struct x11_client *client;
	uint8_t reply[36];

	/* The reply with the map 1, 2, 3. */
	client = &server->clients[index];
	memset(reply, 0, sizeof(reply));
	reply[1] = 3U;
	reply[32] = 1U;
	reply[33] = 2U;
	reply[34] = 3U;
	x11_reply(server, client, reply, sizeof(reply));

	/* Succeeded: answered. */
	return 0U;
}

/* zedBSD's PutImageRGB24: three bytes a pixel into a window or a pixmap (what GLX shows its frames with). */
static unsigned
protocol_put_image_rgb24(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_pixmap *pixmap;
	const uint8_t *rgb;
	uint32_t *pixels;
	uint32_t drawable;
	size_t count;
	int target_width;
	int target_height;
	int x;
	int y;
	int width;
	int height;
	int row;
	int column;
	int to_x;
	int to_y;
	int msb;

	/* The drawable: a window with pixels, or a pixmap. */
	msb = server->clients[index].order;
	if (length < 16U)
		return X11_BAD_LENGTH;
	drawable = x11_read32(request + 4, msb);
	window = x11_window_find(server, drawable);
	pixmap = x11_pixmap_find(server, drawable);
	if (window != NULL && window->pixels == NULL)
		window = NULL;
	if (window == NULL && pixmap == NULL)
		return X11_BAD_WINDOW;

	/* The rectangle, whose pixels must be in the request. */
	x = (int16_t)x11_read16(request + 8, msb);
	y = (int16_t)x11_read16(request + 10, msb);
	width = x11_read16(request + 12, msb);
	height = x11_read16(request + 14, msb);
	count = (size_t)width * (size_t)height;
	if (count == 0U || count > (length - 16U) / 3U)
		return X11_BAD_LENGTH;

	/* The target's pixels and size. */
	if (window != NULL) {
		pixels = window->pixels;
		target_width = window->width;
		target_height = window->height;
	} else {
		pixels = pixmap->pixels;
		target_width = pixmap->width;
		target_height = pixmap->height;
	}

	/* Each pixel inside the target. */
	for (row = 0; row < height; row++) {
		for (column = 0; column < width; column++) {
			to_x = x + column;
			to_y = y + row;
			if (to_x < 0 ||
			    to_y < 0 ||
			    to_x >= target_width ||
			    to_y >= target_height)
				continue;
			rgb = request + 16U + ((size_t)row * (size_t)width + (size_t)column) * 3U;
			pixels[(size_t)to_y * (size_t)target_width + (size_t)to_x] = ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | (uint32_t)rgb[2];
		}
	}

	/* A window's rectangle is shown again. */
	if (window != NULL)
		x11_mark_dirty(server, window->x + x, window->y + y, width, height);

	/* Succeeded: the pixels are in. */
	return 0U;
}

/* Keeps the values of a graphics context its mask names: the foreground and the font. */
static void
protocol_gc_values(
	struct x11_gc *gc,
	const struct x11_client *client,
	const uint8_t *request,
	size_t length,
	size_t offset,
	uint32_t mask)
{
	uint32_t value;
	unsigned bit;

	/* The values, in bit order. */
	for (bit = 0U; bit < 32U && offset + 4U <= length; bit++) {
		if ((mask & (1U << bit)) == 0U)
			continue;

		/* The value of this bit. */
		value = x11_read32(request + offset, client->order);
		offset += 4U;
		if (bit == GC_FOREGROUND)
			gc->foreground = value;
		if (bit == GC_FONT)
			gc->font = value;
	}
}

/* Draws a one-pixel line into a window's pixels (Bresenham), clipped to the window. */
static void
protocol_line(
	struct x11_window *window,
	int from_x,
	int from_y,
	int to_x,
	int to_y,
	uint32_t color)
{
	int dx;
	int dy;
	int step_x;
	int step_y;
	int error;
	int twice;

	/* The distances and directions. */
	dx = abs(to_x - from_x);
	dy = -abs(to_y - from_y);
	step_x = 1;
	if (from_x > to_x)
		step_x = -1;
	step_y = 1;
	if (from_y > to_y)
		step_y = -1;
	error = dx + dy;

	/* Each pixel from the start to the end. */
	for (;;) {
		if (from_x >= 0 &&
		    from_y >= 0 &&
		    from_x < window->width &&
		    from_y < window->height)
			window->pixels[(size_t)from_y * window->width + (size_t)from_x] = color;

		/* The end is drawn last. */
		if (from_x == to_x && from_y == to_y)
			break;

		/* A step across, down, or both. */
		twice = 2 * error;
		if (twice >= dy) {
			error += dy;
			from_x += step_x;
		}

		/* And a step down. */
		if (twice <= dx) {
			error += dx;
			from_y += step_y;
		}
	}
}

/* Moves every descendant of a window by a distance (positions are on the root window). */
static void
protocol_move_descendants(
	struct x11server *server,
	uint32_t parent,
	int dx,
	int dy)
{
	struct x11_window *child;
	unsigned slot;

	/* Nothing moves when the window does not. */
	if (dx == 0 && dy == 0)
		return;

	/* Each child, and its own descendants. */
	for (slot = 1U; slot < server->window_count; slot++) {
		child = &server->windows[slot];
		if (child->parent != parent)
			continue;
		child->x = (int16_t)(child->x + dx);
		child->y = (int16_t)(child->y + dy);
		protocol_move_descendants(server, child->id, dx, dy);
	}
}

/* Sends an error: its code, the request's sequence number, the resource and the opcode. */
static void
protocol_error(
	struct x11server *server,
	struct x11_client *client,
	unsigned code,
	uint32_t resource,
	uint8_t opcode)
{
	uint8_t error[32];

	/* The error. */
	memset(error, 0, sizeof(error));
	error[1] = (uint8_t)code;
	x11_write16(error + 2, client->sequence, client->order);
	x11_write32(error + 4, resource, client->order);
	error[10] = opcode;

	/* Queued for the client. */
	x11_client_send(server, client, error, sizeof(error));
}

/* Asks the client that redirects a parent's children to map one of them. */
static void
protocol_map_request(
	struct x11server *server,
	struct x11_window *parent,
	struct x11_window *window)
{
	struct x11_client *client;
	uint8_t event[32];

	/* The redirecting client must still be there. */
	client = x11_client_of(server, parent->owner);
	if (client == NULL)
		return;

	/* The event: the parent and the window. */
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_MAP_REQUEST;
	x11_write16(event + 2, client->sequence, client->order);
	x11_write32(event + 4, parent->id, client->order);
	x11_write32(event + 8, window->id, client->order);

	/* Queued for the client. */
	x11_client_send(server, client, event, sizeof(event));
}

/* Returns the x of a window's parent on the root window (0 for the root window and a lost parent). */
static int
protocol_parent_x(
	struct x11server *server,
	const struct x11_window *window)
{
	struct x11_window *parent;

	/* The parent, if there is one. */
	parent = x11_window_find(server, window->parent);
	if (parent == NULL)
		return 0;

	/* Succeeded: its x. */
	return parent->x;
}

/* Returns the y of a window's parent on the root window (0 for the root window and a lost parent). */
static int
protocol_parent_y(
	struct x11server *server,
	const struct x11_window *window)
{
	struct x11_window *parent;

	/* The parent, if there is one. */
	parent = x11_window_find(server, window->parent);
	if (parent == NULL)
		return 0;

	/* Succeeded: its y. */
	return parent->y;
}
