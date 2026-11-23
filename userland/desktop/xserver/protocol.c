/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X11 core protocol's dispatch, the replies and errors it sends, and
 * the requests of fonts, pixmaps, graphics contexts, drawing, the
 * keyboard and pointer, the server grab and the core's extensions.
 *
 * Wire layouts follow the public X11 core protocol.  The window, atom,
 * property, selection and event requests are answered by window.c,
 * property.c, selection.c and event.c.  Requests the server does not know
 * are refused with BadRequest, and each request is checked for its length
 * and its values, answering the protocol's error when one is wrong.
 */

#include "userland/desktop/xserver/internal.h"

#include "include/kern/os-name.h"

#include <stdlib.h>
#include <string.h>

/* The core requests answered here, or dispatched to another module. */
#define REQUEST_CREATE_WINDOW		1U
#define REQUEST_CHANGE_ATTRIBUTES	2U
#define REQUEST_GET_ATTRIBUTES		3U
#define REQUEST_DESTROY_WINDOW		4U
#define REQUEST_DESTROY_SUBWINDOWS	5U
#define REQUEST_CHANGE_SAVE_SET		6U
#define REQUEST_REPARENT_WINDOW		7U
#define REQUEST_MAP_WINDOW		8U
#define REQUEST_MAP_SUBWINDOWS		9U
#define REQUEST_UNMAP_WINDOW		10U
#define REQUEST_UNMAP_SUBWINDOWS	11U
#define REQUEST_CONFIGURE_WINDOW	12U
#define REQUEST_CIRCULATE_WINDOW	13U
#define REQUEST_GET_GEOMETRY		14U
#define REQUEST_QUERY_TREE		15U
#define REQUEST_INTERN_ATOM		16U
#define REQUEST_GET_ATOM_NAME		17U
#define REQUEST_CHANGE_PROPERTY		18U
#define REQUEST_DELETE_PROPERTY		19U
#define REQUEST_GET_PROPERTY		20U
#define REQUEST_LIST_PROPERTIES		21U
#define REQUEST_SET_SELECTION_OWNER	22U
#define REQUEST_GET_SELECTION_OWNER	23U
#define REQUEST_CONVERT_SELECTION	24U
#define REQUEST_SEND_EVENT		25U
#define REQUEST_GRAB_SERVER		36U
#define REQUEST_UNGRAB_SERVER		37U
#define REQUEST_QUERY_POINTER		38U
#define REQUEST_TRANSLATE_COORDINATES	40U
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
#define REQUEST_CLEAR_AREA		61U
#define REQUEST_COPY_AREA		62U
#define REQUEST_POLY_LINE		65U
#define REQUEST_POLY_FILL_RECTANGLE	70U
#define REQUEST_PUT_IMAGE		72U
#define REQUEST_GET_IMAGE		73U
#define REQUEST_IMAGE_TEXT8		76U
#define REQUEST_IMAGE_TEXT16		77U
#define REQUEST_QUERY_BEST_SIZE		97U
#define REQUEST_QUERY_EXTENSION		98U
#define REQUEST_LIST_EXTENSIONS		99U
#define REQUEST_GET_KEYBOARD_MAPPING	101U
#define REQUEST_ROTATE_PROPERTIES	114U
#define REQUEST_GET_POINTER_MAPPING	117U
#define REQUEST_GET_MODIFIER_MAPPING	119U
#define REQUEST_NO_OPERATION		127U

/* zedBSD's own: an RGB image of a window's or a pixmap's rectangle, three bytes a pixel. */
#define REQUEST_PUT_IMAGE_RGB24		128U

/* QueryBestSize's classes, and the largest cursor (the backend's pointer is the system's). */
#define BEST_SIZE_CURSOR		0U
#define BEST_SIZE_STIPPLE		2U
#define BEST_CURSOR_MAX			64U

/* SetInputFocus's largest revert-to. */
#define REVERT_TO_MAX			2U

/* The keycodes the keyboard has. */
#define KEYCODE_MIN			8U
#define KEYCODE_MAX			255U

/* The one core font's name, and the vendor the setup names. */
#define FONT_NAME			"zed-unicode"
#define SERVER_VENDOR			ZEDBSD_OS_NAME " X11"

/* The core's extensions: BIG-REQUESTS (its one request, BigReqEnable) and XC-MISC. */
#define BIGREQ_NAME			"BIG-REQUESTS"
#define BIGREQ_ENABLE			0U
#define XCMISC_NAME			"XC-MISC"

/* The extensions ListExtensions names. */
#define EXTENSION_NAMES_MAX		3U

/* The most keycodes one GetKeyboardMapping answers. */
#define KEYBOARD_MAPPING_MAX		248U

static unsigned protocol_dispatch(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_grab_server(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_query_pointer(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_set_input_focus(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_get_input_focus(struct x11server *server, unsigned index, size_t length);
static unsigned protocol_open_font(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_close_font(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_query_font(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_list_fonts(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_get_keyboard_mapping(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_get_pointer_mapping(struct x11server *server, unsigned index, size_t length);
static unsigned protocol_get_modifier_mapping(struct x11server *server, unsigned index, size_t length);
static unsigned protocol_query_best_size(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned protocol_query_extension(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned protocol_list_extensions(struct x11server *server, struct x11_client *client, size_t length);
static unsigned protocol_big_requests(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static void protocol_error(struct x11server *server, struct x11_client *client, unsigned code, uint32_t resource, uint8_t opcode, uint16_t minor);

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
	x11_write32(field + 8, X11_ID_MASK, msb);
	x11_write16(field + 16, (uint16_t)(sizeof(SERVER_VENDOR) - 1U), msb);
	x11_write16(field + 18, 65535U, msb);
	field[20] = 1U;
	field[21] = 1U;
	field[22] = (uint8_t)msb;
	field[23] = 1U;
	field[24] = 32U;
	field[25] = 32U;
	field[26] = KEYCODE_MIN;
	field[27] = KEYCODE_MAX;

	/* The vendor, padded to a word. */
	field += 32;
	memcpy(field, SERVER_VENDOR, sizeof(SERVER_VENDOR) - 1U);

	/* The one pixmap format: depth 24, 32 bits a pixel, rows padded to 32 bits. */
	field += 12;
	field[0] = X11_DEPTH;
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
	field[38] = X11_DEPTH;
	field[39] = 1U;

	/* The depth 24 with its one visual. */
	field += 40;
	field[0] = X11_DEPTH;
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
	uint16_t minor;
	unsigned code;
	uint8_t opcode;

	/* The request is the client's next. */
	client = &server->clients[index];
	client->sequence++;
	opcode = request[0];

	/* The request's own handler; a nonzero code is the error to send. */
	code = protocol_dispatch(server, index, request, length);
	if (code == 0U)
		return;

	/* The client may have gone with its request. */
	if (client->id == 0U)
		return;

	/* A failed one is answered with its error, naming the resource the request names first, and an extension's minor opcode. */
	resource = 0U;
	if (length >= 8U && code != X11_BAD_REQUEST && code != X11_BAD_LENGTH && code != X11_BAD_ALLOC)
		resource = x11_read32(request + 4, client->order);
	minor = 0U;
	if (opcode >= REQUEST_PUT_IMAGE_RGB24)
		minor = request[1];
	protocol_error(server, client, code, resource, opcode, minor);
}

/*
 * Refuses a request the core does not dispatch (one too long to hold):
 * its sequence number moves on, and it is answered with the error.
 */
void
x11_request_refuse(
	struct x11server *server,
	unsigned index,
	uint8_t opcode,
	uint8_t minor,
	unsigned code)
{
	struct x11_client *client;

	/* The request is the client's next, answered with its error. */
	client = &server->clients[index];
	client->sequence++;
	if (opcode < REQUEST_PUT_IMAGE_RGB24)
		minor = 0U;
	protocol_error(server, client, code, 0U, opcode, minor);
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
 * Tells whether an id is one a client may choose: inside the range its
 * setup gave (its base under the mask's bits).
 */
int
x11_id_chosen(
	const struct x11_client *client,
	uint32_t id)
{
	/* The bits above the mask must be the client's base. */
	if ((id & ~X11_ID_MASK) != client->base)
		return 0;

	/* Succeeded: the client's own. */
	return 1;
}

/*
 * Tells whether a request is its fixed part and one 32-bit value for each
 * bit of a mask, exactly.
 */
int
x11_values_fit(
	uint32_t mask,
	size_t fixed,
	size_t length)
{
	size_t count;

	/* The bits of the mask. */
	count = 0U;
	while (mask != 0U) {
		count += mask & 1U;
		mask >>= 1;
	}

	/* Another length than the values make. */
	if (length != fixed + count * 4U)
		return 0;

	/* Succeeded: they fit. */
	return 1;
}

/* Calls a request's handler by its opcode.  Returns 0, or the error to send. */
static unsigned
protocol_dispatch(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	unsigned code;

	/* The handler of the opcode. */
	client = &server->clients[index];
	switch (request[0]) {
	case REQUEST_CREATE_WINDOW:
		code = x11_request_create_window(server, index, request, length);
		break;
	case REQUEST_CHANGE_ATTRIBUTES:
		code = x11_request_change_attributes(server, index, request, length);
		break;
	case REQUEST_GET_ATTRIBUTES:
		code = x11_request_get_attributes(server, index, request, length);
		break;
	case REQUEST_DESTROY_WINDOW:
	case REQUEST_DESTROY_SUBWINDOWS:
		code = x11_request_destroy_window(server, index, request, length);
		break;
	case REQUEST_CHANGE_SAVE_SET:
		code = x11_request_change_save_set(server, index, request, length);
		break;
	case REQUEST_REPARENT_WINDOW:
		code = x11_request_reparent_window(server, index, request, length);
		break;
	case REQUEST_MAP_WINDOW:
	case REQUEST_MAP_SUBWINDOWS:
		code = x11_request_map_window(server, index, request, length);
		break;
	case REQUEST_UNMAP_WINDOW:
	case REQUEST_UNMAP_SUBWINDOWS:
		code = x11_request_unmap_window(server, index, request, length);
		break;
	case REQUEST_CONFIGURE_WINDOW:
		code = x11_request_configure_window(server, index, request, length);
		break;
	case REQUEST_CIRCULATE_WINDOW:
		code = x11_request_circulate_window(server, index, request, length);
		break;
	case REQUEST_GET_GEOMETRY:
		code = x11_request_get_geometry(server, index, request, length);
		break;
	case REQUEST_QUERY_TREE:
		code = x11_request_query_tree(server, index, request, length);
		break;
	case REQUEST_INTERN_ATOM:
		code = x11_request_intern_atom(server, index, request, length);
		break;
	case REQUEST_GET_ATOM_NAME:
		code = x11_request_get_atom_name(server, index, request, length);
		break;
	case REQUEST_CHANGE_PROPERTY:
		code = x11_request_change_property(server, index, request, length);
		break;
	case REQUEST_DELETE_PROPERTY:
		code = x11_request_delete_property(server, index, request, length);
		break;
	case REQUEST_GET_PROPERTY:
		code = x11_request_get_property(server, index, request, length);
		break;
	case REQUEST_LIST_PROPERTIES:
		code = x11_request_list_properties(server, index, request, length);
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
	case REQUEST_GRAB_SERVER:
	case REQUEST_UNGRAB_SERVER:
		code = protocol_grab_server(server, index, request, length);
		break;
	case REQUEST_QUERY_POINTER:
		code = protocol_query_pointer(server, index, request, length);
		break;
	case REQUEST_TRANSLATE_COORDINATES:
		code = x11_request_translate_coordinates(server, index, request, length);
		break;
	case REQUEST_SET_INPUT_FOCUS:
		code = protocol_set_input_focus(server, index, request, length);
		break;
	case REQUEST_GET_INPUT_FOCUS:
		code = protocol_get_input_focus(server, index, length);
		break;
	case REQUEST_OPEN_FONT:
		code = protocol_open_font(server, index, request, length);
		break;
	case REQUEST_CLOSE_FONT:
		code = protocol_close_font(server, index, request, length);
		break;
	case REQUEST_QUERY_FONT:
		code = protocol_query_font(server, index, request, length);
		break;
	case REQUEST_LIST_FONTS:
		code = protocol_list_fonts(server, index, request, length);
		break;
	case REQUEST_CREATE_PIXMAP:
		code = x11_request_create_pixmap(server, index, request, length);
		break;
	case REQUEST_FREE_PIXMAP:
		code = x11_request_free_pixmap(server, index, request, length);
		break;
	case REQUEST_CREATE_GC:
		code = x11_request_create_gc(server, index, request, length);
		break;
	case REQUEST_CHANGE_GC:
		code = x11_request_change_gc(server, index, request, length);
		break;
	case REQUEST_FREE_GC:
		code = x11_request_free_gc(server, index, request, length);
		break;
	case REQUEST_CLEAR_AREA:
		code = x11_request_clear_area(server, index, request, length);
		break;
	case REQUEST_COPY_AREA:
		code = x11_request_copy_area(server, index, request, length);
		break;
	case REQUEST_POLY_LINE:
		code = x11_request_poly_line(server, index, request, length);
		break;
	case REQUEST_POLY_FILL_RECTANGLE:
		code = x11_request_poly_fill_rectangle(server, index, request, length);
		break;
	case REQUEST_PUT_IMAGE:
		code = x11_request_put_image(server, index, request, length);
		break;
	case REQUEST_GET_IMAGE:
		code = x11_request_get_image(server, index, request, length);
		break;
	case REQUEST_IMAGE_TEXT8:
	case REQUEST_IMAGE_TEXT16:
		code = x11_request_image_text(server, index, request, length);
		break;
	case REQUEST_QUERY_BEST_SIZE:
		code = protocol_query_best_size(server, index, request, length);
		break;
	case REQUEST_QUERY_EXTENSION:
		code = protocol_query_extension(server, client, request, length);
		break;
	case REQUEST_LIST_EXTENSIONS:
		code = protocol_list_extensions(server, client, length);
		break;
	case REQUEST_GET_KEYBOARD_MAPPING:
		code = protocol_get_keyboard_mapping(server, index, request, length);
		break;
	case REQUEST_ROTATE_PROPERTIES:
		code = x11_request_rotate_properties(server, index, request, length);
		break;
	case REQUEST_GET_POINTER_MAPPING:
		code = protocol_get_pointer_mapping(server, index, length);
		break;
	case REQUEST_GET_MODIFIER_MAPPING:
		code = protocol_get_modifier_mapping(server, index, length);
		break;
	case REQUEST_NO_OPERATION:
		code = 0U;
		break;
	case REQUEST_PUT_IMAGE_RGB24:
		code = x11_request_put_image_rgb24(server, index, request, length);
		break;
	case X11_BIGREQ_MAJOR:
		code = protocol_big_requests(server, client, request, length);
		break;
	case X11_XCMISC_MAJOR:
		code = x11_xcmisc_request(server, index, request, length);
		break;
	case X11_GLX_MAJOR:
		code = X11_BAD_LENGTH;
		if (length >= 4U)
			code = x11_glx_request(server, client, request, length);
		break;
	default:
		code = X11_BAD_REQUEST;
		break;
	}

	/* Succeeded: answered, or the error to send. */
	return code;
}

/*
 * GrabServer and UngrabServer: while a client holds the server, no other
 * client's requests are handled (they wait in their buffers).
 */
static unsigned
protocol_grab_server(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	/* The header alone. */
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* GrabServer: the client holds the server. */
	if (request[0] == REQUEST_GRAB_SERVER) {
		server->grabbed = index;
		return 0U;
	}

	/* Succeeded: UngrabServer lets the others go on (another client's grab is not its to end). */
	if (server->grabbed == index)
		server->grabbed = X11_NO_CLIENT;
	return 0U;
}

/* QueryPointer: the pointer on the root window and in a window, the window's child it is in, and the modifiers and buttons. */
static unsigned
protocol_query_pointer(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *window;
	struct x11_window *under;
	uint8_t reply[32];
	uint32_t child;
	int msb;

	/* The window. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* The window's child the pointer is in (None when it is in none). */
	under = x11_window_at(server, server->pointer_x, server->pointer_y);
	while (under != NULL && under->parent != window)
		under = under->parent;
	child = 0U;
	if (under != NULL)
		child = under->id;

	/* Succeeded: on this screen, the places, and the state. */
	memset(reply, 0, sizeof(reply));
	reply[1] = 1U;
	x11_write32(reply + 8, X11_ROOT_XID, msb);
	x11_write32(reply + 12, child, msb);
	x11_write16(reply + 16, (uint16_t)server->pointer_x, msb);
	x11_write16(reply + 18, (uint16_t)server->pointer_y, msb);
	x11_write16(reply + 20, (uint16_t)(server->pointer_x - window->x), msb);
	x11_write16(reply + 22, (uint16_t)(server->pointer_y - window->y), msb);
	x11_write16(reply + 24, (uint16_t)(server->key_state | ((unsigned)server->buttons << 8)), msb);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* SetInputFocus: the keyboard's focus to a viewable window, PointerRoot or None. */
static unsigned
protocol_set_input_focus(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	uint32_t focus;
	int viewable;

	/* The revert-to and the focus. */
	if (length != 12U)
		return X11_BAD_LENGTH;
	if (request[1] > REVERT_TO_MAX)
		return X11_BAD_VALUE;
	focus = x11_read32(request + 4, server->clients[index].order);

	/* A window must be there and be seen. */
	if (focus != X11_FOCUS_NONE && focus != X11_FOCUS_POINTER_ROOT) {
		window = x11_window_find(server, focus);
		if (window == NULL)
			return X11_BAD_WINDOW;
		viewable = x11_window_viewable(window);
		if (!viewable)
			return X11_BAD_MATCH;
	}

	/* Succeeded: the focus. */
	server->focus = focus;
	return 0U;
}

/* GetInputFocus: the focus (a window, PointerRoot or None), no revert. */
static unsigned
protocol_get_input_focus(
	struct x11server *server,
	unsigned index,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32];

	/* The header alone. */
	client = &server->clients[index];
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, server->focus, client->order);
	x11_reply(server, client, reply, sizeof(reply));
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
	struct x11_client *client;
	struct x11_font *font;
	uint16_t name_length;
	uint32_t id;
	unsigned taken;
	int chosen;
	int error;

	/* The name, filling the request to its last word. */
	client = &server->clients[index];
	if (length < 12U)
		return X11_BAD_LENGTH;
	name_length = x11_read16(request + 8, client->order);
	if (12U + (((size_t)name_length + 3U) & ~(size_t)3U) != length)
		return X11_BAD_LENGTH;

	/* An id of the client's range not in use. */
	id = x11_read32(request + 4, client->order);
	chosen = x11_id_chosen(client, id);
	taken = x11_resource_type(server, id);
	if (!chosen || taken != 0U)
		return X11_BAD_ID_CHOICE;

	/* The font, charged to the client. */
	error = x11_memory_charge(server, index, sizeof(*font));
	if (error != 0)
		return X11_BAD_ALLOC;
	font = calloc(1U, sizeof(*font));
	if (font == NULL) {
		x11_memory_release(server, index, sizeof(*font));
		return X11_BAD_ALLOC;
	}
	font->id = id;
	font->owner = index;

	/* Found by its id. */
	error = x11_resource_add(server, id, X11_RESOURCE_FONT, font);
	if (error != 0) {
		x11_memory_release(server, index, sizeof(*font));
		free(font);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the font is open. */
	return 0U;
}

/* CloseFont: the font goes. */
static unsigned
protocol_close_font(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_font *font;

	/* The font of the id. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	font = x11_resource_find(server, x11_read32(request + 4, server->clients[index].order), X11_RESOURCE_FONT);
	if (font == NULL)
		return X11_BAD_FONT;

	/* Succeeded: its id and its charge go. */
	x11_resource_remove(server, font->id);
	x11_memory_release(server, font->owner, sizeof(*font));
	free(font);
	return 0U;
}

/* QueryFont: the core font's metrics (Unicode, 8x16 cells with 16x16 for wide characters), of a font or a graphics context's. */
static unsigned
protocol_query_font(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[60];
	unsigned type;
	int msb;

	/* A font, or a graphics context. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 8U)
		return X11_BAD_LENGTH;
	type = x11_resource_type(server, x11_read32(request + 4, msb));
	if (type != X11_RESOURCE_FONT && type != X11_RESOURCE_GC)
		return X11_BAD_FONT;

	/* Succeeded: the character range, the ascent and no properties or per-character metrics. */
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, 0U, msb);
	x11_write16(reply + 10, 255U, msb);
	x11_write16(reply + 12, 0U, msb);
	x11_write16(reply + 14, 255U, msb);
	x11_write16(reply + 18, 0U, msb);
	x11_write16(reply + 20, 16U, msb);
	x11_write32(reply + 56, 0U, msb);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* ListFonts: the one core font's name. */
static unsigned
protocol_list_fonts(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[44];
	size_t pattern;

	/* The pattern, filling the request to its last word. */
	client = &server->clients[index];
	if (length < 8U)
		return X11_BAD_LENGTH;
	pattern = x11_read16(request + 6, client->order);
	if (8U + ((pattern + 3U) & ~(size_t)3U) != length)
		return X11_BAD_LENGTH;

	/* Succeeded: one name, as a length and its bytes. */
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, 1U, client->order);
	reply[32] = (uint8_t)(sizeof(FONT_NAME) - 1U);
	memcpy(reply + 33, FONT_NAME, sizeof(FONT_NAME) - 1U);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* GetKeyboardMapping: one keysym for each keycode asked for. */
static unsigned
protocol_get_keyboard_mapping(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32 + 4 * KEYBOARD_MAPPING_MAX];
	struct x11_client *client;
	uint32_t keycode;
	unsigned count;
	unsigned slot;

	/* The first keycode and the count, within the keyboard's keycodes. */
	client = &server->clients[index];
	if (length != 8U)
		return X11_BAD_LENGTH;
	count = request[5];
	if (request[4] < KEYCODE_MIN || (unsigned)request[4] + count - 1U > KEYCODE_MAX || count == 0U)
		return X11_BAD_VALUE;

	/* One keysym a keycode. */
	memset(reply, 0, sizeof(reply));
	reply[1] = 1U;
	for (slot = 0U; slot < count; slot++) {
		keycode = (uint32_t)request[4] + slot;
		x11_write32(reply + 32 + slot * 4U, x11_keymap_keysym(keycode), client->order);
	}

	/* Succeeded: the reply. */
	x11_reply(server, client, reply, 32U + (size_t)count * 4U);
	return 0U;
}

/* GetPointerMapping: three buttons, each itself. */
static unsigned
protocol_get_pointer_mapping(
	struct x11server *server,
	unsigned index,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[36];

	/* The header alone. */
	client = &server->clients[index];
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: the reply with the map 1, 2, 3. */
	memset(reply, 0, sizeof(reply));
	reply[1] = 3U;
	reply[32] = 1U;
	reply[33] = 2U;
	reply[34] = 3U;
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * GetModifierMapping: no key is a modifier's (the backend gives the
 * modifiers held in each event's state; the keyboard's own modifier keys
 * have no keycodes yet).
 */
static unsigned
protocol_get_modifier_mapping(
	struct x11server *server,
	unsigned index,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32];

	/* The header alone. */
	client = &server->clients[index];
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: no keycode a modifier. */
	memset(reply, 0, sizeof(reply));
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * QueryBestSize: the size asked for, for a tile or a stipple (the server
 * draws any), and at most 64 each way for a cursor.
 */
static unsigned
protocol_query_best_size(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *window;
	struct x11_pixmap *pixmap;
	uint8_t reply[32];
	uint16_t width;
	uint16_t height;

	/* The class and a drawable (an InputOnly window only for a cursor). */
	client = &server->clients[index];
	if (length != 12U)
		return X11_BAD_LENGTH;
	if (request[1] > BEST_SIZE_STIPPLE)
		return X11_BAD_VALUE;
	window = x11_window_find(server, x11_read32(request + 4, client->order));
	pixmap = x11_pixmap_find(server, x11_read32(request + 4, client->order));
	if (window == NULL && pixmap == NULL)
		return X11_BAD_DRAWABLE;
	if (window != NULL && window->window_class == X11_INPUT_ONLY && request[1] != BEST_SIZE_CURSOR)
		return X11_BAD_MATCH;

	/* The size asked, a cursor's within the largest. */
	width = x11_read16(request + 8, client->order);
	height = x11_read16(request + 10, client->order);
	if (request[1] == BEST_SIZE_CURSOR) {
		if (width > BEST_CURSOR_MAX)
			width = BEST_CURSOR_MAX;
		if (height > BEST_CURSOR_MAX)
			height = BEST_CURSOR_MAX;
	}

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, width, client->order);
	x11_write16(reply + 10, height, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * QueryExtension: BIG-REQUESTS and XC-MISC are the core's own; any other
 * name is answered by GLX's table (present only for GLX).
 */
static unsigned
protocol_query_extension(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	size_t name_length;
	uint8_t major;
	int differs;

	/* The name, filling the request to its last word. */
	if (length < 8U)
		return X11_BAD_LENGTH;
	name_length = x11_read16(request + 4, client->order);
	if (8U + ((name_length + 3U) & ~(size_t)3U) != length)
		return X11_BAD_LENGTH;

	/* The core's own extensions by name. */
	major = 0U;
	if (name_length == sizeof(BIGREQ_NAME) - 1U) {
		differs = memcmp(request + 8, BIGREQ_NAME, name_length);
		if (differs == 0)
			major = X11_BIGREQ_MAJOR;
	}
	if (name_length == sizeof(XCMISC_NAME) - 1U) {
		differs = memcmp(request + 8, XCMISC_NAME, name_length);
		if (differs == 0)
			major = X11_XCMISC_MAJOR;
	}

	/* Another name: GLX's answer. */
	if (major == 0U) {
		x11_glx_query_extension(server, client, request, length);
		return 0U;
	}

	/* Succeeded: present, with its major opcode and no events or errors. */
	memset(reply, 0, sizeof(reply));
	reply[8] = 1U;
	reply[9] = major;
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* ListExtensions: the names of the extensions present, each a counted string. */
static unsigned
protocol_list_extensions(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	static const char *const names[EXTENSION_NAMES_MAX] = { BIGREQ_NAME, XCMISC_NAME, "GLX" };
	uint8_t reply[32U + 32U];
	size_t used;
	size_t name_length;
	unsigned index;

	/* The header alone. */
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* The names after the header, each its length and its bytes. */
	memset(reply, 0, sizeof(reply));
	used = 32U;
	for (index = 0U; index < EXTENSION_NAMES_MAX; index++) {
		name_length = strlen(names[index]);
		reply[used] = (uint8_t)name_length;
		memcpy(reply + used + 1U, names[index], name_length);
		used += 1U + name_length;
	}

	/* Succeeded: the count, and the list padded to words. */
	reply[1] = (uint8_t)EXTENSION_NAMES_MAX;
	used = (used + 3U) & ~(size_t)3U;
	x11_reply(server, client, reply, used);
	return 0U;
}

/*
 * BIG-REQUESTS: BigReqEnable answers the longest request in words (the
 * input the core holds for a client) and lets the client send requests of
 * length 0 with a 32-bit length.
 */
static unsigned
protocol_big_requests(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];

	/* BigReqEnable is the only request, the header alone. */
	if (request[1] != BIGREQ_ENABLE)
		return X11_BAD_REQUEST;
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: the longest request, and big requests from now on. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, X11_INPUT_CAP / 4U, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	client->big_requests = 1;
	return 0U;
}

/* Sends an error: its code, the request's sequence number, the resource, and the major and minor opcodes. */
static void
protocol_error(
	struct x11server *server,
	struct x11_client *client,
	unsigned code,
	uint32_t resource,
	uint8_t opcode,
	uint16_t minor)
{
	uint8_t error[32];

	/* The error. */
	memset(error, 0, sizeof(error));
	error[1] = (uint8_t)code;
	x11_write16(error + 2, client->sequence, client->order);
	x11_write32(error + 4, resource, client->order);
	x11_write16(error + 8, minor, client->order);
	error[10] = opcode;

	/* Queued for the client. */
	x11_client_send(server, client, error, sizeof(error));
}
