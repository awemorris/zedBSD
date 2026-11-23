/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The XFIXES extension (version 6.0).
 *
 * Clients use it for three things the core protocol lacks.  A clipboard
 * manager and a toolkit hear when a selection's owner changes
 * (SelectSelectionInput and XFixesSelectionNotify, which selection.c
 * reports here).  Toolkits keep regions as server resources and give them
 * to a window's shape (SetWindowShapeRegion, through shape-ext.c) or a
 * graphics context's clip.  And a client may ask about the cursor, hide
 * it and name it.
 *
 * The cursor's half works on cursor.c's cursors: GetCursorImage gives the
 * cursor shown under the pointer (a clear one of one pixel where the
 * system's own pointer, or a shape of the cursor font, which has no image
 * here, is shown), CursorNotify tells of a new cursor shown, cursors are
 * named and changed, and a client's HideCursor hides the cursor until it
 * shows it again or goes.  Pointer
 * barriers are kept as resources only: a rootless server cannot hold the
 * system's pointer back.  A client's disconnect mode is kept for its
 * answer; the server does not end when its last client goes.
 *
 * Every request after QueryVersion is answered only up to the version the
 * client asked for, as the X.Org server does.  Wire layouts follow the
 * public XFIXES extension protocol.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The requests' minor opcodes. */
#define XFIXES_QUERY_VERSION			0U
#define XFIXES_CHANGE_SAVE_SET			1U
#define XFIXES_SELECT_SELECTION_INPUT		2U
#define XFIXES_SELECT_CURSOR_INPUT		3U
#define XFIXES_GET_CURSOR_IMAGE			4U
#define XFIXES_CREATE_REGION			5U
#define XFIXES_CREATE_REGION_FROM_BITMAP	6U
#define XFIXES_CREATE_REGION_FROM_WINDOW	7U
#define XFIXES_CREATE_REGION_FROM_GC		8U
#define XFIXES_CREATE_REGION_FROM_PICTURE	9U
#define XFIXES_DESTROY_REGION			10U
#define XFIXES_SET_REGION			11U
#define XFIXES_COPY_REGION			12U
#define XFIXES_UNION_REGION			13U
#define XFIXES_INTERSECT_REGION			14U
#define XFIXES_SUBTRACT_REGION			15U
#define XFIXES_INVERT_REGION			16U
#define XFIXES_TRANSLATE_REGION			17U
#define XFIXES_REGION_EXTENTS			18U
#define XFIXES_FETCH_REGION			19U
#define XFIXES_SET_GC_CLIP_REGION		20U
#define XFIXES_SET_WINDOW_SHAPE_REGION		21U
#define XFIXES_SET_PICTURE_CLIP_REGION		22U
#define XFIXES_SET_CURSOR_NAME			23U
#define XFIXES_GET_CURSOR_NAME			24U
#define XFIXES_GET_CURSOR_IMAGE_AND_NAME	25U
#define XFIXES_CHANGE_CURSOR			26U
#define XFIXES_CHANGE_CURSOR_BY_NAME		27U
#define XFIXES_EXPAND_REGION			28U
#define XFIXES_HIDE_CURSOR			29U
#define XFIXES_SHOW_CURSOR			30U
#define XFIXES_CREATE_POINTER_BARRIER		31U
#define XFIXES_DESTROY_POINTER_BARRIER		32U
#define XFIXES_SET_CLIENT_DISCONNECT_MODE	33U
#define XFIXES_GET_CLIENT_DISCONNECT_MODE	34U

/* The version the extension speaks. */
#define XFIXES_VERSION_MAJOR		6U
#define XFIXES_VERSION_MINOR		0U

/* The events' offsets from the first: a selection's new owner, and a new cursor. */
#define XFIXES_SELECTION_NOTIFY		0U
#define XFIXES_CURSOR_NOTIFY		1U

/* The masks a client selects: the three selection events, and the cursor's. */
#define XFIXES_SELECTION_MASKS		0x7U
#define XFIXES_CURSOR_MASK		0x1U

/* CreateRegionFromWindow's kinds: the bounding and the clip region. */
#define XFIXES_WINDOW_BOUNDING		0U
#define XFIXES_WINDOW_CLIP		1U

/* The disconnect mode a client may set: the server ends with its last client (kept, not acted on). */
#define XFIXES_DISCONNECT_TERMINATE	1U

/* RENDER's BadPicture (its first error plus one), for the picture requests while the core has no pictures. */
#define XFIXES_BAD_PICTURE		135U

/* The core requests XFIXES's GC clip borrows: ChangeGC and SetClipRectangles, and ChangeGC's clip-mask bit. */
#define XFIXES_CORE_CHANGE_GC		56U
#define XFIXES_CORE_SET_CLIP		59U
#define XFIXES_CORE_CHANGE_SAVE_SET	6U
#define XFIXES_GC_CLIP_MASK		(1U << 19)

/* The most boxes a region may have, and the longest name a cursor is given. */
#define XFIXES_BOXES_MAX		((size_t)1U << 20)
#define XFIXES_NAME_MAX			256U

/*
 * The last request of each version (version n answers the minor opcodes
 * up to entry n), as the X.Org server has them.
 */
static const uint8_t xfixes_version_last[XFIXES_VERSION_MAJOR + 1U] = {
	XFIXES_QUERY_VERSION,
	XFIXES_GET_CURSOR_IMAGE,
	XFIXES_CHANGE_CURSOR_BY_NAME,
	XFIXES_EXPAND_REGION,
	XFIXES_SHOW_CURSOR,
	XFIXES_DESTROY_POINTER_BARRIER,
	XFIXES_GET_CLIENT_DISCONNECT_MODE
};

/*
 * A region a client made: its boxes (in X's banded form, on the root
 * window's coordinates or wherever the client uses it), its owner, and
 * the bytes the owner is charged for them.
 */
struct xfixes_region {
	struct x11_region region;
	unsigned owner;
	size_t charged;
};

/*
 * A pointer barrier a client made: kept so that its id is taken and
 * DestroyPointerBarrier finds it; the system's pointer is not held by it.
 */
struct xfixes_barrier {
	uint32_t id;
	unsigned owner;
	uint32_t window;
};

/*
 * One wish to hear of something: the client, the window the events name,
 * the selection (0 for the cursor's), and the mask.
 */
struct xfixes_select {
	unsigned client;
	uint32_t window;
	uint32_t selection;
	uint32_t mask;
};

/*
 * A name a client gave a cursor (SetCursorName): the cursor's id and the
 * name's atom.
 */
struct xfixes_name {
	uint32_t cursor;
	uint32_t atom;
};

/*
 * XFIXES's state of a server: for each client slot the version it asked
 * for (0: none yet), how many times it hid the cursor, and its disconnect
 * mode; the wishes to hear of selections and of the cursor; the names
 * given to cursors; and the serial of the cursor last shown, as
 * CursorNotify told it (0: the system's pointer).
 */
struct x11_xfixes {
	uint8_t *versions;
	unsigned *hidden;
	uint8_t *disconnect;
	struct xfixes_select *selects;
	unsigned select_count;
	unsigned select_capacity;
	struct xfixes_name *names;
	unsigned name_count;
	unsigned name_capacity;
	uint32_t cursor_shown;
};

static struct x11_xfixes *xfixes_state(struct x11server *server);
static unsigned xfixes_query_version(struct x11_ext_request *request);
static unsigned xfixes_change_save_set(struct x11_ext_request *request);
static unsigned xfixes_select_selection_input(struct x11_ext_request *request);
static unsigned xfixes_select_cursor_input(struct x11_ext_request *request);
static unsigned xfixes_get_cursor_image(struct x11_ext_request *request, int named);
static unsigned xfixes_create_region(struct x11_ext_request *request);
static unsigned xfixes_create_region_from_bitmap(struct x11_ext_request *request);
static unsigned xfixes_create_region_from_window(struct x11_ext_request *request);
static unsigned xfixes_create_region_from_gc(struct x11_ext_request *request);
static unsigned xfixes_destroy_region(struct x11_ext_request *request);
static unsigned xfixes_set_region(struct x11_ext_request *request);
static unsigned xfixes_copy_region(struct x11_ext_request *request);
static unsigned xfixes_combine_region(struct x11_ext_request *request, unsigned operation);
static unsigned xfixes_invert_region(struct x11_ext_request *request);
static unsigned xfixes_translate_region(struct x11_ext_request *request);
static unsigned xfixes_region_extents(struct x11_ext_request *request);
static unsigned xfixes_fetch_region(struct x11_ext_request *request);
static unsigned xfixes_set_gc_clip_region(struct x11_ext_request *request);
static unsigned xfixes_set_window_shape_region(struct x11_ext_request *request);
static unsigned xfixes_picture_region(struct x11_ext_request *request);
static unsigned xfixes_set_cursor_name(struct x11_ext_request *request);
static unsigned xfixes_get_cursor_name(struct x11_ext_request *request);
static unsigned xfixes_change_cursor(struct x11_ext_request *request);
static unsigned xfixes_change_cursor_by_name(struct x11_ext_request *request);
static unsigned xfixes_expand_region(struct x11_ext_request *request);
static unsigned xfixes_hide_cursor(struct x11_ext_request *request, int hide);
static unsigned xfixes_create_pointer_barrier(struct x11_ext_request *request);
static unsigned xfixes_destroy_pointer_barrier(struct x11_ext_request *request);
static unsigned xfixes_set_client_disconnect_mode(struct x11_ext_request *request);
static unsigned xfixes_get_client_disconnect_mode(struct x11_ext_request *request);
static unsigned xfixes_new_id(struct x11_ext_request *request, uint32_t id);
static unsigned xfixes_find_region(struct x11_ext_request *request, uint32_t id, struct xfixes_region **found);
static unsigned xfixes_add_region(struct x11_ext_request *request, uint32_t id, struct x11_region *region);
static unsigned xfixes_replace(struct x11server *server, struct xfixes_region *target, struct x11_region *region);
static unsigned xfixes_rects(struct x11_ext_request *request, size_t offset, struct x11_region *region);
static unsigned xfixes_find_cursor(struct x11_ext_request *request, uint32_t id, struct x11_cursor **cursor);
static uint32_t xfixes_cursor_serial(const struct x11_cursor *cursor);
static uint32_t xfixes_cursor_name(const struct x11_xfixes *state, const struct x11_cursor *cursor);
static size_t xfixes_region_bytes(const struct x11_region *region);
static void xfixes_region_free(struct x11server *server, struct xfixes_region *region);
static unsigned xfixes_select(struct x11_xfixes *state, unsigned client, uint32_t window, uint32_t selection, uint32_t mask);
static void xfixes_release(struct x11server *server, unsigned owner, unsigned type);

/*
 * Answers an XFIXES request by its minor opcode, up to the version the
 * client asked for.  Returns 0, or the error to send (request->bad names
 * the value that was wrong).
 */
unsigned
x11_xfixes_request(
	struct x11_ext_request *request)
{
	struct x11_xfixes *state;
	unsigned version;
	unsigned minor;
	unsigned code;

	/* The state, made on the first request. */
	state = xfixes_state(request->server);
	if (state == NULL)
		return X11_BAD_ALLOC;

	/* Only the requests of the version the client asked for (QueryVersion first). */
	minor = request->bytes[1];
	version = state->versions[request->index];
	if (minor > xfixes_version_last[version])
		return X11_BAD_REQUEST;

	/* The request of the minor opcode. */
	switch (minor) {
	case XFIXES_QUERY_VERSION:
		code = xfixes_query_version(request);
		break;
	case XFIXES_CHANGE_SAVE_SET:
		code = xfixes_change_save_set(request);
		break;
	case XFIXES_SELECT_SELECTION_INPUT:
		code = xfixes_select_selection_input(request);
		break;
	case XFIXES_SELECT_CURSOR_INPUT:
		code = xfixes_select_cursor_input(request);
		break;
	case XFIXES_GET_CURSOR_IMAGE:
		code = xfixes_get_cursor_image(request, 0);
		break;
	case XFIXES_CREATE_REGION:
		code = xfixes_create_region(request);
		break;
	case XFIXES_CREATE_REGION_FROM_BITMAP:
		code = xfixes_create_region_from_bitmap(request);
		break;
	case XFIXES_CREATE_REGION_FROM_WINDOW:
		code = xfixes_create_region_from_window(request);
		break;
	case XFIXES_CREATE_REGION_FROM_GC:
		code = xfixes_create_region_from_gc(request);
		break;
	case XFIXES_CREATE_REGION_FROM_PICTURE:
	case XFIXES_SET_PICTURE_CLIP_REGION:
		code = xfixes_picture_region(request);
		break;
	case XFIXES_DESTROY_REGION:
		code = xfixes_destroy_region(request);
		break;
	case XFIXES_SET_REGION:
		code = xfixes_set_region(request);
		break;
	case XFIXES_COPY_REGION:
		code = xfixes_copy_region(request);
		break;
	case XFIXES_UNION_REGION:
	case XFIXES_INTERSECT_REGION:
	case XFIXES_SUBTRACT_REGION:
		code = xfixes_combine_region(request, minor);
		break;
	case XFIXES_INVERT_REGION:
		code = xfixes_invert_region(request);
		break;
	case XFIXES_TRANSLATE_REGION:
		code = xfixes_translate_region(request);
		break;
	case XFIXES_REGION_EXTENTS:
		code = xfixes_region_extents(request);
		break;
	case XFIXES_FETCH_REGION:
		code = xfixes_fetch_region(request);
		break;
	case XFIXES_SET_GC_CLIP_REGION:
		code = xfixes_set_gc_clip_region(request);
		break;
	case XFIXES_SET_WINDOW_SHAPE_REGION:
		code = xfixes_set_window_shape_region(request);
		break;
	case XFIXES_SET_CURSOR_NAME:
		code = xfixes_set_cursor_name(request);
		break;
	case XFIXES_GET_CURSOR_NAME:
		code = xfixes_get_cursor_name(request);
		break;
	case XFIXES_GET_CURSOR_IMAGE_AND_NAME:
		code = xfixes_get_cursor_image(request, 1);
		break;
	case XFIXES_CHANGE_CURSOR:
		code = xfixes_change_cursor(request);
		break;
	case XFIXES_CHANGE_CURSOR_BY_NAME:
		code = xfixes_change_cursor_by_name(request);
		break;
	case XFIXES_EXPAND_REGION:
		code = xfixes_expand_region(request);
		break;
	case XFIXES_HIDE_CURSOR:
		code = xfixes_hide_cursor(request, 1);
		break;
	case XFIXES_SHOW_CURSOR:
		code = xfixes_hide_cursor(request, 0);
		break;
	case XFIXES_CREATE_POINTER_BARRIER:
		code = xfixes_create_pointer_barrier(request);
		break;
	case XFIXES_DESTROY_POINTER_BARRIER:
		code = xfixes_destroy_pointer_barrier(request);
		break;
	case XFIXES_SET_CLIENT_DISCONNECT_MODE:
		code = xfixes_set_client_disconnect_mode(request);
		break;
	case XFIXES_GET_CLIENT_DISCONNECT_MODE:
		code = xfixes_get_client_disconnect_mode(request);
		break;
	default:
		code = X11_BAD_REQUEST;
		break;
	}

	/* Succeeded: answered, or the error to send. */
	return code;
}

/*
 * Tells the clients that asked that a selection's owner changed: a new
 * owner (or None), the owner's window destroyed, or the owner's client
 * closed (X11_XFIXES_*), with the time the selection last changed.
 */
void
x11_xfixes_selection(
	struct x11server *server,
	uint32_t selection,
	uint32_t owner,
	unsigned subtype,
	uint32_t selection_time)
{
	struct x11_xfixes *state;
	struct x11_client *client;
	uint8_t event[32];
	unsigned slot;
	int msb;

	/* Nobody asked yet. */
	state = server->xfixes;
	if (state == NULL)
		return;

	/* Each wish for this selection and this kind of change. */
	for (slot = 0U; slot < state->select_count; slot++) {
		if (state->selects[slot].selection != selection)
			continue;
		if ((state->selects[slot].mask & (1U << subtype)) == 0U)
			continue;
		client = x11_client_of(server, state->selects[slot].client);
		if (client == NULL)
			continue;

		/* The event, in the client's byte order. */
		msb = client->order;
		memset(event, 0, sizeof(event));
		event[0] = (uint8_t)(X11_XFIXES_EVENT + XFIXES_SELECTION_NOTIFY);
		event[1] = (uint8_t)subtype;
		x11_write32(event + 4, state->selects[slot].window, msb);
		x11_write32(event + 8, owner, msb);
		x11_write32(event + 12, selection, msb);
		x11_write32(event + 16, server->now, msb);
		x11_write32(event + 20, selection_time, msb);
		x11_extension_event(server, client, event);
	}
}

/*
 * Forgets a closed client: its regions and barriers go, its wishes to
 * hear of selections and the cursor, its hiding of the cursor and the
 * version it asked for.
 */
void
x11_xfixes_forget_client(
	struct x11server *server,
	unsigned index)
{
	struct x11_xfixes *state;
	unsigned slot;

	/* Its regions and barriers. */
	xfixes_release(server, index, X11_RESOURCE_REGION);
	xfixes_release(server, index, X11_RESOURCE_BARRIER);

	/* Nothing else kept yet. */
	state = server->xfixes;
	if (state == NULL)
		return;

	/* Its wishes, the last taking each one's place. */
	slot = 0U;
	while (slot < state->select_count) {
		if (state->selects[slot].client != index) {
			slot++;
			continue;
		}
		state->selects[slot] = state->selects[state->select_count - 1U];
		state->select_count--;
	}

	/* Succeeded: its slot's state starts again. */
	state->versions[index] = 0U;
	state->hidden[index] = 0U;
	state->disconnect[index] = 0U;
}

/*
 * Forgets the wishes to hear of selections and the cursor that name a
 * window going away.
 */
void
x11_xfixes_forget_window(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_xfixes *state;
	unsigned slot;

	/* Nothing kept yet. */
	state = server->xfixes;
	if (state == NULL)
		return;

	/* Succeeded: each wish naming it, the last taking its place. */
	slot = 0U;
	while (slot < state->select_count) {
		if (state->selects[slot].window != window->id) {
			slot++;
			continue;
		}
		state->selects[slot] = state->selects[state->select_count - 1U];
		state->select_count--;
	}
}

/*
 * Frees XFIXES's state when the server ends.
 */
void
x11_xfixes_fini(
	struct x11server *server)
{
	struct x11_xfixes *state;

	/* Nothing made. */
	state = server->xfixes;
	if (state == NULL)
		return;

	/* Succeeded: freed. */
	free(state->versions);
	free(state->hidden);
	free(state->disconnect);
	free(state->selects);
	free(state->names);
	free(state);
	server->xfixes = NULL;
}

/*
 * Returns the region a client made with an id, or NULL (for a module that
 * takes regions, such as RENDER's clip).
 */
struct x11_region *
x11_xfixes_region(
	struct x11server *server,
	uint32_t id)
{
	struct xfixes_region *found;

	/* The resource of the kind. */
	found = x11_resource_find(server, id, X11_RESOURCE_REGION);
	if (found == NULL)
		return NULL;

	/* Succeeded: its boxes. */
	return &found->region;
}

/*
 * Tells whether a client hid the cursor (HideCursor more times than
 * ShowCursor): the cursor is then not shown.
 */
int
x11_xfixes_cursor_hidden(
	struct x11server *server)
{
	struct x11_xfixes *state;
	unsigned slot;

	/* Nothing kept yet. */
	state = server->xfixes;
	if (state == NULL)
		return 0;

	/* Any client's count. */
	for (slot = 0U; slot < server->clients_max; slot++) {
		if (state->hidden[slot] != 0U)
			return 1;
	}

	/* Succeeded: shown. */
	return 0;
}

/*
 * Tells the clients that asked (SelectCursorInput) of a new cursor shown:
 * CursorNotify with its serial and name, once for each change.
 */
void
x11_xfixes_cursor_tick(
	struct x11server *server)
{
	const struct x11_cursor *cursor;
	struct x11_xfixes *state;
	struct x11_client *client;
	uint8_t event[32];
	uint32_t serial;
	uint32_t atom;
	unsigned slot;
	int msb;

	/* Nothing kept yet. */
	state = server->xfixes;
	if (state == NULL)
		return;

	/* The cursor shown now, the same as last told. */
	cursor = x11_cursor_current(server);
	serial = xfixes_cursor_serial(cursor);
	if (serial == state->cursor_shown)
		return;
	state->cursor_shown = serial;
	atom = xfixes_cursor_name(state, cursor);

	/* Succeeded: each wish for the cursor's events told, in its client's byte order. */
	for (slot = 0U; slot < state->select_count; slot++) {
		if (state->selects[slot].selection != 0U)
			continue;
		if ((state->selects[slot].mask & XFIXES_CURSOR_MASK) == 0U)
			continue;
		client = x11_client_of(server, state->selects[slot].client);
		if (client == NULL)
			continue;
		msb = client->order;
		memset(event, 0, sizeof(event));
		event[0] = (uint8_t)(X11_XFIXES_EVENT + XFIXES_CURSOR_NOTIFY);
		event[1] = 0U;
		x11_write32(event + 4, state->selects[slot].window, msb);
		x11_write32(event + 8, serial, msb);
		x11_write32(event + 12, server->now, msb);
		x11_write32(event + 16, atom, msb);
		x11_extension_event(server, client, event);
	}
}

/* Returns XFIXES's state, made when first asked for (NULL without memory). */
static struct x11_xfixes *
xfixes_state(
	struct x11server *server)
{
	struct x11_xfixes *state;

	/* Already made. */
	if (server->xfixes != NULL)
		return server->xfixes;

	/* The state, and a slot of each kind for each client. */
	state = calloc(1U, sizeof(*state));
	if (state == NULL)
		return NULL;
	state->versions = calloc(server->clients_max, sizeof(*state->versions));
	state->hidden = calloc(server->clients_max, sizeof(*state->hidden));
	state->disconnect = calloc(server->clients_max, sizeof(*state->disconnect));
	if (state->versions == NULL || state->hidden == NULL || state->disconnect == NULL) {
		free(state->versions);
		free(state->hidden);
		free(state->disconnect);
		free(state);
		return NULL;
	}

	/* Succeeded: kept for the server. */
	server->xfixes = state;
	return state;
}

/*
 * QueryVersion: the version the client asked for, or the server's when it
 * asked for a later one; the client is answered up to that version from
 * now on.
 */
static unsigned
xfixes_query_version(
	struct x11_ext_request *request)
{
	struct x11_xfixes *state;
	uint8_t reply[32];
	uint32_t major;
	uint32_t minor;

	/* The client's version. */
	if (request->length != 12U)
		return X11_BAD_LENGTH;
	major = x11_read32(request->bytes + 4, request->msb);
	minor = x11_read32(request->bytes + 8, request->msb);

	/* The lower of the two. */
	if (major > XFIXES_VERSION_MAJOR || (major == XFIXES_VERSION_MAJOR && minor > XFIXES_VERSION_MINOR)) {
		major = XFIXES_VERSION_MAJOR;
		minor = XFIXES_VERSION_MINOR;
	}

	/* The client is answered up to it. */
	state = request->server->xfixes;
	state->versions[request->index] = (uint8_t)major;

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, major, request->msb);
	x11_write32(reply + 12, minor, request->msb);
	x11_reply(request->server, request->client, reply, sizeof(reply));
	return 0U;
}

/*
 * ChangeSaveSet: the core's ChangeSaveSet (the target and the map, which
 * only a reparenting window manager acts on, are not kept).
 */
static unsigned
xfixes_change_save_set(
	struct x11_ext_request *request)
{
	uint8_t core[8];
	unsigned code;

	/* The mode, the target, the map, and the window. */
	if (request->length != 12U)
		return X11_BAD_LENGTH;
	if (request->bytes[5] > 1U) {
		request->bad = request->bytes[5];
		return X11_BAD_VALUE;
	}
	if (request->bytes[6] > 1U) {
		request->bad = request->bytes[6];
		return X11_BAD_VALUE;
	}

	/* The core's request of the same mode and window. */
	memset(core, 0, sizeof(core));
	core[0] = XFIXES_CORE_CHANGE_SAVE_SET;
	core[1] = request->bytes[4];
	x11_write16(core + 2, 2U, request->msb);
	memcpy(core + 4, request->bytes + 8, 4U);
	code = x11_request_change_save_set(request->server, request->index, core, sizeof(core));

	/* Succeeded: the core's answer, naming the window or the mode when wrong. */
	request->bad = x11_read32(request->bytes + 8, request->msb);
	if (code == X11_BAD_VALUE)
		request->bad = request->bytes[4];
	return code;
}

/*
 * SelectSelectionInput: the client hears of a selection's new owners on a
 * window (a mask of 0: no longer).
 */
static unsigned
xfixes_select_selection_input(
	struct x11_ext_request *request)
{
	struct x11_window *window;
	uint32_t window_id;
	uint32_t selection;
	uint32_t mask;
	unsigned code;
	int valid;

	/* The window, the selection and the mask. */
	if (request->length != 16U)
		return X11_BAD_LENGTH;
	window_id = x11_read32(request->bytes + 4, request->msb);
	selection = x11_read32(request->bytes + 8, request->msb);
	mask = x11_read32(request->bytes + 12, request->msb);
	window = x11_window_find(request->server, window_id);
	if (window == NULL) {
		request->bad = window_id;
		return X11_BAD_WINDOW;
	}
	valid = x11_atom_valid(request->server, selection);
	if (!valid) {
		request->bad = selection;
		return X11_BAD_ATOM;
	}
	if ((mask & ~XFIXES_SELECTION_MASKS) != 0U) {
		request->bad = mask;
		return X11_BAD_VALUE;
	}

	/* Succeeded: kept, replaced or forgotten. */
	code = xfixes_select(request->server->xfixes, request->index, window_id, selection, mask);
	return code;
}

/*
 * SelectCursorInput: the client hears of a new cursor shown, with the
 * window it named.
 */
static unsigned
xfixes_select_cursor_input(
	struct x11_ext_request *request)
{
	struct x11_window *window;
	uint32_t window_id;
	uint32_t mask;
	unsigned code;

	/* The window and the mask. */
	if (request->length != 12U)
		return X11_BAD_LENGTH;
	window_id = x11_read32(request->bytes + 4, request->msb);
	mask = x11_read32(request->bytes + 8, request->msb);
	window = x11_window_find(request->server, window_id);
	if (window == NULL) {
		request->bad = window_id;
		return X11_BAD_WINDOW;
	}
	if ((mask & ~XFIXES_CURSOR_MASK) != 0U) {
		request->bad = mask;
		return X11_BAD_VALUE;
	}

	/* Kept, replaced or forgotten; a change is told from the cursor shown now on. */
	code = xfixes_select(request->server->xfixes, request->index, window_id, 0U, mask);
	if (code != 0U)
		return code;

	/* Succeeded: the cursor shown now is the one known. */
	request->server->xfixes->cursor_shown = xfixes_cursor_serial(x11_cursor_current(request->server));
	return 0U;
}

/*
 * GetCursorImage and GetCursorImageAndName: where the pointer is, and the
 * cursor shown: its image, hot spot and serial (a clear pixel with the
 * system's pointer or a shape of the cursor font), and for the second its
 * name (none: 0 and no bytes).
 */
static unsigned
xfixes_get_cursor_image(
	struct x11_ext_request *request,
	int named)
{
	const struct x11_cursor *cursor;
	const char *name;
	uint32_t *pixels;
	uint32_t clear;
	uint8_t *reply;
	uint32_t atom;
	size_t count;
	size_t index;
	size_t name_length;
	size_t size;
	size_t at;
	int width;
	int height;
	int hot_x;
	int hot_y;

	/* The header alone. */
	if (request->length != 4U)
		return X11_BAD_LENGTH;

	/* The cursor shown, and its image (one clear pixel without one). */
	cursor = x11_cursor_current(request->server);
	pixels = NULL;
	if (cursor != NULL)
		pixels = x11_cursor_pixels(cursor);
	clear = 0U;
	width = 1;
	height = 1;
	hot_x = 0;
	hot_y = 0;
	if (pixels != NULL) {
		width = cursor->width;
		height = cursor->height;
		hot_x = cursor->hot_x;
		hot_y = cursor->hot_y;
	}

	/* Its name, for the second request. */
	atom = 0U;
	name = NULL;
	name_length = 0U;
	if (named)
		atom = xfixes_cursor_name(request->server->xfixes, cursor);
	if (atom != 0U)
		name = x11_atom_name(request->server, atom, &name_length);
	if (name == NULL)
		name_length = 0U;
	if (name_length > XFIXES_NAME_MAX)
		name_length = XFIXES_NAME_MAX;

	/* The reply: the fixed part, the pixels, and the name padded to a word. */
	count = (size_t)width * (size_t)height;
	size = 32U + count * 4U + ((name_length + 3U) & ~(size_t)3U);
	reply = calloc(1U, size);
	if (reply == NULL) {
		free(pixels);
		return X11_BAD_ALLOC;
	}
	x11_write16(reply + 8, (uint16_t)request->server->pointer_x, request->msb);
	x11_write16(reply + 10, (uint16_t)request->server->pointer_y, request->msb);
	x11_write16(reply + 12, (uint16_t)width, request->msb);
	x11_write16(reply + 14, (uint16_t)height, request->msb);
	x11_write16(reply + 16, (uint16_t)hot_x, request->msb);
	x11_write16(reply + 18, (uint16_t)hot_y, request->msb);
	x11_write32(reply + 20, xfixes_cursor_serial(cursor), request->msb);
	if (named) {
		x11_write32(reply + 24, atom, request->msb);
		x11_write16(reply + 28, (uint16_t)name_length, request->msb);
	}
	for (index = 0U; index < count; index++) {
		at = 32U + index * 4U;
		if (pixels != NULL) {
			x11_write32(reply + at, pixels[index], request->msb);
		} else {
			x11_write32(reply + at, clear, request->msb);
		}
	}
	if (name_length != 0U)
		memcpy(reply + 32U + count * 4U, name, name_length);

	/* Succeeded: sent. */
	x11_reply(request->server, request->client, reply, size);
	free(reply);
	free(pixels);
	return 0U;
}

/* CreateRegion: a region of a list of rectangles. */
static unsigned
xfixes_create_region(
	struct x11_ext_request *request)
{
	struct x11_region region;
	uint32_t id;
	unsigned code;

	/* The id and whole rectangles. */
	if (request->length < 8U || (request->length - 8U) % 8U != 0U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	code = xfixes_new_id(request, id);
	if (code != 0U)
		return code;

	/* The rectangles' region. */
	x11_region_init(&region);
	code = xfixes_rects(request, 8U, &region);
	if (code != 0U) {
		x11_region_fini(&region);
		return code;
	}

	/* Succeeded: the resource. */
	code = xfixes_add_region(request, id, &region);
	return code;
}

/* CreateRegionFromBitmap: a region of a bitmap's set pixels. */
static unsigned
xfixes_create_region_from_bitmap(
	struct x11_ext_request *request)
{
	struct x11_region region;
	struct x11_pixmap *pixmap;
	struct x11_box box;
	const uint32_t *row;
	uint32_t id;
	uint32_t pixmap_id;
	unsigned code;
	int start;
	int x;
	int y;
	int error;

	/* The id and the bitmap. */
	if (request->length != 12U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	code = xfixes_new_id(request, id);
	if (code != 0U)
		return code;
	pixmap_id = x11_read32(request->bytes + 8, request->msb);
	pixmap = x11_pixmap_find(request->server, pixmap_id);
	if (pixmap == NULL) {
		request->bad = pixmap_id;
		return X11_BAD_PIXMAP;
	}
	if (pixmap->depth != 1U)
		return X11_BAD_MATCH;

	/* Each row's runs of set pixels. */
	x11_region_init(&region);
	for (y = 0; y < pixmap->image.height; y++) {
		row = pixmap->image.pixels + (size_t)y * pixmap->image.stride;
		x = 0;
		while (x < pixmap->image.width) {
			if (row[x] == 0U) {
				x++;
				continue;
			}
			start = x;
			while (x < pixmap->image.width && row[x] != 0U)
				x++;
			box.x0 = start;
			box.y0 = y;
			box.x1 = x;
			box.y1 = y + 1;
			error = x11_region_append_box(&region, &box);
			if (error != 0 || region.count > XFIXES_BOXES_MAX) {
				x11_region_fini(&region);
				return X11_BAD_ALLOC;
			}
		}
	}

	/* Succeeded: the resource. */
	code = xfixes_add_region(request, id, &region);
	return code;
}

/* CreateRegionFromWindow: a window's bounding or clip region (from its origin), as SHAPE keeps it. */
static unsigned
xfixes_create_region_from_window(
	struct x11_ext_request *request)
{
	struct x11_region region;
	struct x11_window *window;
	uint32_t id;
	uint32_t window_id;
	unsigned kind;
	unsigned code;
	int error;

	/* The id, the window and the kind. */
	if (request->length != 16U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	code = xfixes_new_id(request, id);
	if (code != 0U)
		return code;
	window_id = x11_read32(request->bytes + 8, request->msb);
	window = x11_window_find(request->server, window_id);
	if (window == NULL) {
		request->bad = window_id;
		return X11_BAD_WINDOW;
	}
	kind = request->bytes[12];
	if (kind > XFIXES_WINDOW_CLIP) {
		request->bad = kind;
		return X11_BAD_VALUE;
	}

	/* The window's region. */
	x11_region_init(&region);
	error = x11_shape_region(window, kind, &region);
	if (error != 0) {
		x11_region_fini(&region);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the resource. */
	code = xfixes_add_region(request, id, &region);
	return code;
}

/* CreateRegionFromGC: a graphics context's clip (BadMatch when it has none). */
static unsigned
xfixes_create_region_from_gc(
	struct x11_ext_request *request)
{
	struct x11_region region;
	struct x11_gc *gc;
	uint32_t id;
	uint32_t gc_id;
	unsigned code;
	int error;

	/* The id and the context. */
	if (request->length != 12U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	code = xfixes_new_id(request, id);
	if (code != 0U)
		return code;
	gc_id = x11_read32(request->bytes + 8, request->msb);
	gc = x11_gc_find(request->server, gc_id);
	if (gc == NULL) {
		request->bad = gc_id;
		return X11_BAD_GC;
	}
	if (!gc->clip_set)
		return X11_BAD_MATCH;

	/* Its clip region (on its clip origin, as the client gave it). */
	x11_region_init(&region);
	error = x11_region_copy(&region, &gc->clip);
	if (error != 0) {
		x11_region_fini(&region);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the resource. */
	code = xfixes_add_region(request, id, &region);
	return code;
}

/* DestroyRegion: the region goes. */
static unsigned
xfixes_destroy_region(
	struct x11_ext_request *request)
{
	struct xfixes_region *region;
	uint32_t id;
	unsigned code;

	/* The region. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	code = xfixes_find_region(request, id, &region);
	if (code != 0U)
		return code;

	/* Succeeded: gone. */
	x11_resource_remove(request->server, id);
	xfixes_region_free(request->server, region);
	return 0U;
}

/* SetRegion: a region becomes a list of rectangles. */
static unsigned
xfixes_set_region(
	struct x11_ext_request *request)
{
	struct xfixes_region *target;
	struct x11_region region;
	unsigned code;

	/* The region and whole rectangles. */
	if (request->length < 8U || (request->length - 8U) % 8U != 0U)
		return X11_BAD_LENGTH;
	code = xfixes_find_region(request, x11_read32(request->bytes + 4, request->msb), &target);
	if (code != 0U)
		return code;

	/* The rectangles' region. */
	x11_region_init(&region);
	code = xfixes_rects(request, 8U, &region);
	if (code != 0U) {
		x11_region_fini(&region);
		return code;
	}

	/* Succeeded: it replaces the region. */
	code = xfixes_replace(request->server, target, &region);
	return code;
}

/* CopyRegion: the destination becomes the source. */
static unsigned
xfixes_copy_region(
	struct x11_ext_request *request)
{
	struct xfixes_region *source;
	struct xfixes_region *target;
	struct x11_region region;
	unsigned code;
	int error;

	/* The two regions. */
	if (request->length != 12U)
		return X11_BAD_LENGTH;
	code = xfixes_find_region(request, x11_read32(request->bytes + 4, request->msb), &source);
	if (code != 0U)
		return code;
	code = xfixes_find_region(request, x11_read32(request->bytes + 8, request->msb), &target);
	if (code != 0U)
		return code;

	/* The copy. */
	x11_region_init(&region);
	error = x11_region_copy(&region, &source->region);
	if (error != 0) {
		x11_region_fini(&region);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: it replaces the destination. */
	code = xfixes_replace(request->server, target, &region);
	return code;
}

/* UnionRegion, IntersectRegion, SubtractRegion: the destination becomes the two sources joined, met or cut. */
static unsigned
xfixes_combine_region(
	struct x11_ext_request *request,
	unsigned operation)
{
	struct xfixes_region *first;
	struct xfixes_region *second;
	struct xfixes_region *target;
	struct x11_region region;
	unsigned code;
	int error;

	/* The three regions. */
	if (request->length != 16U)
		return X11_BAD_LENGTH;
	code = xfixes_find_region(request, x11_read32(request->bytes + 4, request->msb), &first);
	if (code != 0U)
		return code;
	code = xfixes_find_region(request, x11_read32(request->bytes + 8, request->msb), &second);
	if (code != 0U)
		return code;
	code = xfixes_find_region(request, x11_read32(request->bytes + 12, request->msb), &target);
	if (code != 0U)
		return code;

	/* The first source with the second. */
	x11_region_init(&region);
	error = x11_region_copy(&region, &first->region);
	if (error == 0) {
		if (operation == XFIXES_UNION_REGION) {
			error = x11_region_union(&region, &second->region);
		} else if (operation == XFIXES_INTERSECT_REGION) {
			error = x11_region_intersect(&region, &second->region);
		} else {
			error = x11_region_subtract(&region, &second->region);
		}
	}
	if (error != 0) {
		x11_region_fini(&region);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: it replaces the destination. */
	code = xfixes_replace(request->server, target, &region);
	return code;
}

/* InvertRegion: the destination becomes a rectangle less the source. */
static unsigned
xfixes_invert_region(
	struct x11_ext_request *request)
{
	struct xfixes_region *source;
	struct xfixes_region *target;
	struct x11_region region;
	struct x11_box box;
	unsigned code;
	int error;

	/* The source, the rectangle and the destination. */
	if (request->length != 20U)
		return X11_BAD_LENGTH;
	code = xfixes_find_region(request, x11_read32(request->bytes + 4, request->msb), &source);
	if (code != 0U)
		return code;
	code = xfixes_find_region(request, x11_read32(request->bytes + 16, request->msb), &target);
	if (code != 0U)
		return code;
	box.x0 = (int16_t)x11_read16(request->bytes + 8, request->msb);
	box.y0 = (int16_t)x11_read16(request->bytes + 10, request->msb);
	box.x1 = box.x0 + (int)x11_read16(request->bytes + 12, request->msb);
	box.y1 = box.y0 + (int)x11_read16(request->bytes + 14, request->msb);

	/* The rectangle less the source (an empty rectangle is nothing). */
	x11_region_init(&region);
	error = 0;
	if (box.x0 < box.x1 && box.y0 < box.y1) {
		error = x11_region_set(&region, &box);
		if (error == 0)
			error = x11_region_subtract(&region, &source->region);
	}
	if (error != 0) {
		x11_region_fini(&region);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: it replaces the destination. */
	code = xfixes_replace(request->server, target, &region);
	return code;
}

/* TranslateRegion: a region moves. */
static unsigned
xfixes_translate_region(
	struct x11_ext_request *request)
{
	struct xfixes_region *target;
	unsigned code;

	/* The region and the move. */
	if (request->length != 12U)
		return X11_BAD_LENGTH;
	code = xfixes_find_region(request, x11_read32(request->bytes + 4, request->msb), &target);
	if (code != 0U)
		return code;

	/* Succeeded: moved. */
	x11_region_translate(&target->region, (int16_t)x11_read16(request->bytes + 8, request->msb), (int16_t)x11_read16(request->bytes + 10, request->msb));
	return 0U;
}

/* RegionExtents: the destination becomes the box around the source. */
static unsigned
xfixes_region_extents(
	struct x11_ext_request *request)
{
	struct xfixes_region *source;
	struct xfixes_region *target;
	struct x11_region region;
	struct x11_box extents;
	unsigned code;
	int error;

	/* The two regions. */
	if (request->length != 12U)
		return X11_BAD_LENGTH;
	code = xfixes_find_region(request, x11_read32(request->bytes + 4, request->msb), &source);
	if (code != 0U)
		return code;
	code = xfixes_find_region(request, x11_read32(request->bytes + 8, request->msb), &target);
	if (code != 0U)
		return code;

	/* The box (nothing for an empty source). */
	x11_region_init(&region);
	x11_region_extents(&source->region, &extents);
	if (source->region.count != 0U) {
		error = x11_region_set(&region, &extents);
		if (error != 0) {
			x11_region_fini(&region);
			return X11_BAD_ALLOC;
		}
	}

	/* Succeeded: it replaces the destination. */
	code = xfixes_replace(request->server, target, &region);
	return code;
}

/* FetchRegion: the box around a region and its rectangles in X's banded order. */
static unsigned
xfixes_fetch_region(
	struct x11_ext_request *request)
{
	struct xfixes_region *source;
	struct x11_box extents;
	uint8_t *reply;
	uint8_t *rect;
	size_t size;
	size_t index;
	unsigned code;

	/* The region. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	code = xfixes_find_region(request, x11_read32(request->bytes + 4, request->msb), &source);
	if (code != 0U)
		return code;

	/* The reply, room for every rectangle. */
	size = 32U + source->region.count * 8U;
	reply = calloc(1U, size);
	if (reply == NULL)
		return X11_BAD_ALLOC;

	/* The box, and each rectangle. */
	x11_region_extents(&source->region, &extents);
	x11_write16(reply + 8, (uint16_t)extents.x0, request->msb);
	x11_write16(reply + 10, (uint16_t)extents.y0, request->msb);
	x11_write16(reply + 12, (uint16_t)(extents.x1 - extents.x0), request->msb);
	x11_write16(reply + 14, (uint16_t)(extents.y1 - extents.y0), request->msb);
	for (index = 0U; index < source->region.count; index++) {
		rect = reply + 32U + index * 8U;
		x11_write16(rect, (uint16_t)source->region.boxes[index].x0, request->msb);
		x11_write16(rect + 2, (uint16_t)source->region.boxes[index].y0, request->msb);
		x11_write16(rect + 4, (uint16_t)(source->region.boxes[index].x1 - source->region.boxes[index].x0), request->msb);
		x11_write16(rect + 6, (uint16_t)(source->region.boxes[index].y1 - source->region.boxes[index].y0), request->msb);
	}

	/* Succeeded: sent. */
	x11_reply(request->server, request->client, reply, size);
	free(reply);
	return 0U;
}

/*
 * SetGCClipRegion: a graphics context's clip becomes a region at an
 * origin (None: no clip), through the core's SetClipRectangles and
 * ChangeGC.
 */
static unsigned
xfixes_set_gc_clip_region(
	struct x11_ext_request *request)
{
	struct xfixes_region *source;
	struct x11_gc *gc;
	uint8_t *core;
	uint8_t *rect;
	uint32_t gc_id;
	uint32_t region_id;
	size_t size;
	size_t index;
	unsigned code;

	/* The context and the region. */
	if (request->length != 16U)
		return X11_BAD_LENGTH;
	gc_id = x11_read32(request->bytes + 4, request->msb);
	gc = x11_gc_find(request->server, gc_id);
	if (gc == NULL) {
		request->bad = gc_id;
		return X11_BAD_GC;
	}
	region_id = x11_read32(request->bytes + 8, request->msb);
	source = NULL;
	if (region_id != 0U) {
		code = xfixes_find_region(request, region_id, &source);
		if (code != 0U)
			return code;
	}

	/* None: the core's ChangeGC of the clip mask to None. */
	if (source == NULL) {
		size = 16U;
		core = calloc(1U, size);
		if (core == NULL)
			return X11_BAD_ALLOC;
		core[0] = XFIXES_CORE_CHANGE_GC;
		x11_write16(core + 2, (uint16_t)(size / 4U), request->msb);
		x11_write32(core + 4, gc_id, request->msb);
		x11_write32(core + 8, XFIXES_GC_CLIP_MASK, request->msb);
		code = x11_request_change_gc(request->server, request->index, core, size);
		free(core);
		request->bad = gc_id;
		return code;
	}

	/* The core's SetClipRectangles of the region's rectangles at the origin (YXBanded). */
	size = 12U + source->region.count * 8U;
	core = calloc(1U, size);
	if (core == NULL)
		return X11_BAD_ALLOC;
	core[0] = XFIXES_CORE_SET_CLIP;
	core[1] = 3U;
	x11_write32(core + 4, gc_id, request->msb);
	memcpy(core + 8, request->bytes + 12, 4U);
	for (index = 0U; index < source->region.count; index++) {
		rect = core + 12U + index * 8U;
		x11_write16(rect, (uint16_t)source->region.boxes[index].x0, request->msb);
		x11_write16(rect + 2, (uint16_t)source->region.boxes[index].y0, request->msb);
		x11_write16(rect + 4, (uint16_t)(source->region.boxes[index].x1 - source->region.boxes[index].x0), request->msb);
		x11_write16(rect + 6, (uint16_t)(source->region.boxes[index].y1 - source->region.boxes[index].y0), request->msb);
	}
	code = x11_request_set_clip_rectangles(request->server, request->index, core, size);
	free(core);

	/* Succeeded: the core's answer. */
	request->bad = gc_id;
	return code;
}

/*
 * SetWindowShapeRegion: a window's region of a SHAPE kind becomes a
 * region moved by an offset (None: the window is unshaped of the kind).
 */
static unsigned
xfixes_set_window_shape_region(
	struct x11_ext_request *request)
{
	struct xfixes_region *source;
	struct x11_region moved;
	struct x11_window *window;
	uint32_t window_id;
	uint32_t region_id;
	unsigned kind;
	unsigned code;
	int error;

	/* The window, the kind and the region. */
	if (request->length != 20U)
		return X11_BAD_LENGTH;
	window_id = x11_read32(request->bytes + 4, request->msb);
	window = x11_window_find(request->server, window_id);
	if (window == NULL) {
		request->bad = window_id;
		return X11_BAD_WINDOW;
	}
	kind = request->bytes[8];
	if (kind > X11_SHAPE_INPUT) {
		request->bad = kind;
		return X11_BAD_VALUE;
	}
	region_id = x11_read32(request->bytes + 16, request->msb);

	/* None: unshaped. */
	if (region_id == 0U) {
		code = x11_shape_set(request->server, window, kind, NULL);
		return code;
	}

	/* The region at the offset. */
	code = xfixes_find_region(request, region_id, &source);
	if (code != 0U)
		return code;
	x11_region_init(&moved);
	error = x11_region_copy(&moved, &source->region);
	if (error != 0) {
		x11_region_fini(&moved);
		return X11_BAD_ALLOC;
	}
	x11_region_translate(&moved, (int16_t)x11_read16(request->bytes + 12, request->msb), (int16_t)x11_read16(request->bytes + 14, request->msb));

	/* Succeeded: SHAPE takes it. */
	code = x11_shape_set(request->server, window, kind, &moved);
	x11_region_fini(&moved);
	return code;
}

/*
 * CreateRegionFromPicture and SetPictureClipRegion: RENDER's pictures,
 * which the core does not make yet; every picture id is refused.
 */
static unsigned
xfixes_picture_region(
	struct x11_ext_request *request)
{
	uint32_t id;
	unsigned code;

	/* The fixed size: the region and the picture, or the picture, the region and the origin. */
	if (request->bytes[1] == XFIXES_CREATE_REGION_FROM_PICTURE && request->length != 12U)
		return X11_BAD_LENGTH;
	if (request->bytes[1] == XFIXES_SET_PICTURE_CLIP_REGION && request->length != 16U)
		return X11_BAD_LENGTH;

	/* A new region's id is checked first. */
	if (request->bytes[1] == XFIXES_CREATE_REGION_FROM_PICTURE) {
		id = x11_read32(request->bytes + 4, request->msb);
		code = xfixes_new_id(request, id);
		if (code != 0U)
			return code;
		request->bad = x11_read32(request->bytes + 8, request->msb);
		return XFIXES_BAD_PICTURE;
	}

	/* Succeeded: no such picture. */
	request->bad = x11_read32(request->bytes + 4, request->msb);
	return XFIXES_BAD_PICTURE;
}

/* SetCursorName: a cursor's name (an atom of it), kept for GetCursorName. */
static unsigned
xfixes_set_cursor_name(
	struct x11_ext_request *request)
{
	struct x11_xfixes *state;
	struct xfixes_name *grown;
	struct x11_cursor *found;
	uint32_t cursor;
	uint32_t atom;
	size_t length;
	unsigned capacity;
	unsigned slot;
	unsigned code;

	/* The cursor and the name filling the request. */
	if (request->length < 12U)
		return X11_BAD_LENGTH;
	cursor = x11_read32(request->bytes + 4, request->msb);
	length = x11_read16(request->bytes + 8, request->msb);
	if (12U + ((length + 3U) & ~(size_t)3U) != request->length)
		return X11_BAD_LENGTH;
	code = xfixes_find_cursor(request, cursor, &found);
	if (code != 0U)
		return code;

	/* The name's atom. */
	atom = x11_atom_intern(request->server, (const char *)request->bytes + 12, length, 0);
	if (atom == 0U)
		return X11_BAD_ALLOC;

	/* The cursor's entry, replaced or added. */
	state = request->server->xfixes;
	for (slot = 0U; slot < state->name_count; slot++) {
		if (state->names[slot].cursor == cursor) {
			state->names[slot].atom = atom;
			return 0U;
		}
	}
	if (state->name_count == state->name_capacity) {
		capacity = state->name_capacity * 2U;
		if (capacity == 0U)
			capacity = 8U;
		grown = realloc(state->names, capacity * sizeof(*grown));
		if (grown == NULL)
			return X11_BAD_ALLOC;
		state->names = grown;
		state->name_capacity = capacity;
	}

	/* Succeeded: named. */
	state->names[state->name_count].cursor = cursor;
	state->names[state->name_count].atom = atom;
	state->name_count++;
	return 0U;
}

/* GetCursorName: a cursor's name and its atom (none: 0 and no bytes). */
static unsigned
xfixes_get_cursor_name(
	struct x11_ext_request *request)
{
	struct x11_xfixes *state;
	struct x11_cursor *found;
	const char *name;
	uint8_t *reply;
	uint32_t cursor;
	uint32_t atom;
	size_t length;
	size_t size;
	unsigned slot;
	unsigned code;

	/* The cursor. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	cursor = x11_read32(request->bytes + 4, request->msb);
	code = xfixes_find_cursor(request, cursor, &found);
	if (code != 0U)
		return code;

	/* Its name, if it was given one. */
	state = request->server->xfixes;
	atom = 0U;
	for (slot = 0U; slot < state->name_count; slot++) {
		if (state->names[slot].cursor == cursor)
			atom = state->names[slot].atom;
	}
	name = NULL;
	length = 0U;
	if (atom != 0U)
		name = x11_atom_name(request->server, atom, &length);
	if (name == NULL)
		length = 0U;
	if (length > XFIXES_NAME_MAX)
		length = XFIXES_NAME_MAX;

	/* The reply, the name padded to a word. */
	size = 32U + ((length + 3U) & ~(size_t)3U);
	reply = calloc(1U, size);
	if (reply == NULL)
		return X11_BAD_ALLOC;
	x11_write32(reply + 8, atom, request->msb);
	x11_write16(reply + 12, (uint16_t)length, request->msb);
	if (length != 0U)
		memcpy(reply + 32, name, length);

	/* Succeeded: sent. */
	x11_reply(request->server, request->client, reply, size);
	free(reply);
	return 0U;
}

/* ChangeCursor: the destination cursor looks like the source from now on, wherever it is used. */
static unsigned
xfixes_change_cursor(
	struct x11_ext_request *request)
{
	struct x11_cursor *source;
	struct x11_cursor *target;
	unsigned code;

	/* The two cursors. */
	if (request->length != 12U)
		return X11_BAD_LENGTH;
	code = xfixes_find_cursor(request, x11_read32(request->bytes + 4, request->msb), &source);
	if (code != 0U)
		return code;
	code = xfixes_find_cursor(request, x11_read32(request->bytes + 8, request->msb), &target);
	if (code != 0U)
		return code;

	/* Succeeded: changed. */
	code = x11_cursor_change(request->server, target, source);
	return code;
}

/* ChangeCursorByName: every cursor of a name looks like the source from now on. */
static unsigned
xfixes_change_cursor_by_name(
	struct x11_ext_request *request)
{
	struct x11_xfixes *state;
	struct x11_cursor *source;
	struct x11_cursor *target;
	uint32_t atom;
	size_t length;
	unsigned slot;
	unsigned code;

	/* The cursor and the name filling the request. */
	if (request->length < 12U)
		return X11_BAD_LENGTH;
	length = x11_read16(request->bytes + 8, request->msb);
	if (12U + ((length + 3U) & ~(size_t)3U) != request->length)
		return X11_BAD_LENGTH;
	code = xfixes_find_cursor(request, x11_read32(request->bytes + 4, request->msb), &source);
	if (code != 0U)
		return code;

	/* The name's atom; a name no atom has names no cursor. */
	atom = x11_atom_intern(request->server, (const char *)request->bytes + 12, length, 1);
	if (atom == 0U)
		return 0U;

	/* Succeeded: each cursor of the name changed. */
	state = request->server->xfixes;
	for (slot = 0U; slot < state->name_count; slot++) {
		if (state->names[slot].atom != atom)
			continue;
		target = x11_cursor_find(request->server, state->names[slot].cursor);
		if (target == NULL)
			continue;
		code = x11_cursor_change(request->server, target, source);
		if (code != 0U)
			return code;
	}
	return 0U;
}

/* ExpandRegion: the destination becomes each rectangle of the source grown on its four sides. */
static unsigned
xfixes_expand_region(
	struct x11_ext_request *request)
{
	struct xfixes_region *source;
	struct xfixes_region *target;
	struct x11_region region;
	struct x11_box box;
	size_t index;
	unsigned left;
	unsigned right;
	unsigned top;
	unsigned bottom;
	unsigned code;
	int error;

	/* The two regions and the four widths. */
	if (request->length != 20U)
		return X11_BAD_LENGTH;
	code = xfixes_find_region(request, x11_read32(request->bytes + 4, request->msb), &source);
	if (code != 0U)
		return code;
	code = xfixes_find_region(request, x11_read32(request->bytes + 8, request->msb), &target);
	if (code != 0U)
		return code;
	left = x11_read16(request->bytes + 12, request->msb);
	right = x11_read16(request->bytes + 14, request->msb);
	top = x11_read16(request->bytes + 16, request->msb);
	bottom = x11_read16(request->bytes + 18, request->msb);

	/* Each rectangle grown, joined. */
	x11_region_init(&region);
	error = 0;
	for (index = 0U; index < source->region.count && error == 0; index++) {
		box.x0 = source->region.boxes[index].x0 - (int)left;
		box.y0 = source->region.boxes[index].y0 - (int)top;
		box.x1 = source->region.boxes[index].x1 + (int)right;
		box.y1 = source->region.boxes[index].y1 + (int)bottom;
		error = x11_region_union_box(&region, &box);
	}
	if (error != 0) {
		x11_region_fini(&region);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: it replaces the destination. */
	code = xfixes_replace(request->server, target, &region);
	return code;
}

/*
 * HideCursor and ShowCursor: the client hides the cursor once more, or
 * once less (BadMatch when it has not hidden it); while any client's count
 * is above 0 the cursor is not shown (cursor.c).
 */
static unsigned
xfixes_hide_cursor(
	struct x11_ext_request *request,
	int hide)
{
	struct x11_xfixes *state;
	struct x11_window *window;
	uint32_t window_id;

	/* The window (its screen's cursor). */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	window_id = x11_read32(request->bytes + 4, request->msb);
	window = x11_window_find(request->server, window_id);
	if (window == NULL) {
		request->bad = window_id;
		return X11_BAD_WINDOW;
	}

	/* Once more. */
	state = request->server->xfixes;
	if (hide) {
		state->hidden[request->index]++;
		return 0U;
	}

	/* Once less, after a hide. */
	if (state->hidden[request->index] == 0U)
		return X11_BAD_MATCH;

	/* Succeeded: shown once more. */
	state->hidden[request->index]--;
	return 0U;
}

/*
 * CreatePointerBarrier: a barrier on a window's screen, kept as a
 * resource (the system's pointer is not held back).  The line must be
 * horizontal or vertical, and the devices fill the request.
 */
static unsigned
xfixes_create_pointer_barrier(
	struct x11_ext_request *request)
{
	struct xfixes_barrier *barrier;
	struct x11_window *window;
	uint32_t id;
	uint32_t window_id;
	unsigned devices;
	unsigned code;
	int x0;
	int y0;
	int x1;
	int y1;
	int error;

	/* The fixed part and the devices after it. */
	if (request->length < 28U)
		return X11_BAD_LENGTH;
	devices = x11_read16(request->bytes + 26, request->msb);
	if (request->length != 28U + (((size_t)devices * 2U + 3U) & ~(size_t)3U))
		return X11_BAD_LENGTH;

	/* The id and the window. */
	id = x11_read32(request->bytes + 4, request->msb);
	code = xfixes_new_id(request, id);
	if (code != 0U)
		return code;
	window_id = x11_read32(request->bytes + 8, request->msb);
	window = x11_window_find(request->server, window_id);
	if (window == NULL) {
		request->bad = window_id;
		return X11_BAD_WINDOW;
	}

	/* A line along an axis, not a point. */
	x0 = (int16_t)x11_read16(request->bytes + 12, request->msb);
	y0 = (int16_t)x11_read16(request->bytes + 14, request->msb);
	x1 = (int16_t)x11_read16(request->bytes + 16, request->msb);
	y1 = (int16_t)x11_read16(request->bytes + 18, request->msb);
	if (x0 != x1 && y0 != y1)
		return X11_BAD_VALUE;
	if (x0 == x1 && y0 == y1)
		return X11_BAD_VALUE;

	/* The barrier, charged to the client. */
	error = x11_memory_charge(request->server, request->index, sizeof(*barrier));
	if (error != 0)
		return X11_BAD_ALLOC;
	barrier = calloc(1U, sizeof(*barrier));
	if (barrier == NULL) {
		x11_memory_release(request->server, request->index, sizeof(*barrier));
		return X11_BAD_ALLOC;
	}
	barrier->id = id;
	barrier->owner = request->index;
	barrier->window = window_id;
	error = x11_resource_add(request->server, id, X11_RESOURCE_BARRIER, barrier);
	if (error != 0) {
		x11_memory_release(request->server, request->index, sizeof(*barrier));
		free(barrier);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: kept. */
	return 0U;
}

/* DestroyPointerBarrier: the barrier goes. */
static unsigned
xfixes_destroy_pointer_barrier(
	struct x11_ext_request *request)
{
	struct xfixes_barrier *barrier;
	uint32_t id;

	/* The barrier. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	id = x11_read32(request->bytes + 4, request->msb);
	barrier = x11_resource_find(request->server, id, X11_RESOURCE_BARRIER);
	if (barrier == NULL) {
		request->bad = id;
		return X11_BAD_BARRIER;
	}

	/* Succeeded: gone. */
	x11_resource_remove(request->server, id);
	x11_memory_release(request->server, barrier->owner, sizeof(*barrier));
	free(barrier);
	return 0U;
}

/* SetClientDisconnectMode: kept for the client's answer. */
static unsigned
xfixes_set_client_disconnect_mode(
	struct x11_ext_request *request)
{
	uint32_t mode;

	/* The mode: the default, or terminate. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	mode = x11_read32(request->bytes + 4, request->msb);
	if ((mode & ~XFIXES_DISCONNECT_TERMINATE) != 0U) {
		request->bad = mode;
		return X11_BAD_VALUE;
	}

	/* Succeeded: kept. */
	request->server->xfixes->disconnect[request->index] = (uint8_t)mode;
	return 0U;
}

/* GetClientDisconnectMode: the mode the client set. */
static unsigned
xfixes_get_client_disconnect_mode(
	struct x11_ext_request *request)
{
	uint8_t reply[32];

	/* The header alone. */
	if (request->length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, request->server->xfixes->disconnect[request->index], request->msb);
	x11_reply(request->server, request->client, reply, sizeof(reply));
	return 0U;
}

/* Checks a new resource's id: the client's range, not in use.  Returns 0, or BadIDChoice. */
static unsigned
xfixes_new_id(
	struct x11_ext_request *request,
	uint32_t id)
{
	unsigned taken;
	int chosen;

	/* The client's own, and free. */
	chosen = x11_id_chosen(request->client, id);
	taken = x11_resource_type(request->server, id);
	if (!chosen || taken != 0U) {
		request->bad = id;
		return X11_BAD_ID_CHOICE;
	}

	/* Succeeded: free. */
	return 0U;
}

/* Finds a region a request names.  Returns 0, or BadRegion (naming the id). */
static unsigned
xfixes_find_region(
	struct x11_ext_request *request,
	uint32_t id,
	struct xfixes_region **found)
{
	/* The resource of the kind. */
	*found = x11_resource_find(request->server, id, X11_RESOURCE_REGION);
	if (*found == NULL) {
		request->bad = id;
		return X11_BAD_REGION;
	}

	/* Succeeded: found. */
	return 0U;
}

/*
 * Makes a region resource of boxes (taken over: the caller's region is
 * empty afterwards either way), in X's form and charged to the client.
 * Returns 0, or BadAlloc.
 */
static unsigned
xfixes_add_region(
	struct x11_ext_request *request,
	uint32_t id,
	struct x11_region *region)
{
	struct xfixes_region *made;
	size_t bytes;
	int error;

	/* In X's form. */
	error = x11_region_canonical(region);
	if (error != 0) {
		x11_region_fini(region);
		return X11_BAD_ALLOC;
	}

	/* The resource, charged to the client. */
	bytes = xfixes_region_bytes(region);
	error = x11_memory_charge(request->server, request->index, bytes);
	if (error != 0) {
		x11_region_fini(region);
		return X11_BAD_ALLOC;
	}
	made = calloc(1U, sizeof(*made));
	if (made == NULL) {
		x11_memory_release(request->server, request->index, bytes);
		x11_region_fini(region);
		return X11_BAD_ALLOC;
	}
	made->region = *region;
	made->owner = request->index;
	made->charged = bytes;
	x11_region_init(region);

	/* Known by its id. */
	error = x11_resource_add(request->server, id, X11_RESOURCE_REGION, made);
	if (error != 0) {
		xfixes_region_free(request->server, made);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: made. */
	return 0U;
}

/*
 * Puts new boxes in a region resource (taken over: the caller's region is
 * empty afterwards either way), in X's form, its charge following them.
 * Returns 0, or BadAlloc (the resource keeps what it had).
 */
static unsigned
xfixes_replace(
	struct x11server *server,
	struct xfixes_region *target,
	struct x11_region *region)
{
	size_t bytes;
	int error;

	/* In X's form. */
	error = x11_region_canonical(region);
	if (error != 0) {
		x11_region_fini(region);
		return X11_BAD_ALLOC;
	}

	/* The new charge in place of the old. */
	bytes = xfixes_region_bytes(region);
	if (bytes > target->charged) {
		error = x11_memory_charge(server, target->owner, bytes - target->charged);
		if (error != 0) {
			x11_region_fini(region);
			return X11_BAD_ALLOC;
		}
	} else {
		x11_memory_release(server, target->owner, target->charged - bytes);
	}
	target->charged = bytes;

	/* Succeeded: the boxes replace the old. */
	x11_region_fini(&target->region);
	target->region = *region;
	x11_region_init(region);
	return 0U;
}

/*
 * Reads the rectangles a request carries from an offset to its end into a
 * region (empty rectangles are nothing).  Returns 0, or BadAlloc.
 */
static unsigned
xfixes_rects(
	struct x11_ext_request *request,
	size_t offset,
	struct x11_region *region)
{
	struct x11_box box;
	const uint8_t *rect;
	unsigned width;
	unsigned height;
	int error;

	/* Each rectangle. */
	for (; offset + 8U <= request->length; offset += 8U) {
		rect = request->bytes + offset;
		width = x11_read16(rect + 4, request->msb);
		height = x11_read16(rect + 6, request->msb);
		if (width == 0U || height == 0U)
			continue;
		box.x0 = (int16_t)x11_read16(rect, request->msb);
		box.y0 = (int16_t)x11_read16(rect + 2, request->msb);
		box.x1 = box.x0 + (int)width;
		box.y1 = box.y0 + (int)height;
		error = x11_region_union_box(region, &box);
		if (error != 0)
			return X11_BAD_ALLOC;
	}

	/* Succeeded: read. */
	return 0U;
}

/* Finds a cursor a request names (one a client made).  Returns 0, or BadCursor (naming the id). */
static unsigned
xfixes_find_cursor(
	struct x11_ext_request *request,
	uint32_t id,
	struct x11_cursor **cursor)
{
	/* The cursor of the id. */
	*cursor = x11_cursor_find(request->server, id);
	if (*cursor == NULL) {
		request->bad = id;
		return X11_BAD_CURSOR;
	}

	/* Succeeded: a cursor. */
	return 0U;
}

/* Gives a cursor's serial as XFIXES reports it: its version (0: the system's pointer). */
static uint32_t
xfixes_cursor_serial(
	const struct x11_cursor *cursor)
{
	/* The system's pointer. */
	if (cursor == NULL)
		return 0U;

	/* Succeeded: the version cursor.c gave it. */
	return cursor->version;
}

/* Gives the atom of a cursor's name (0: none, or no cursor). */
static uint32_t
xfixes_cursor_name(
	const struct x11_xfixes *state,
	const struct x11_cursor *cursor)
{
	unsigned slot;

	/* No cursor, or one whose id went. */
	if (cursor == NULL || cursor->id == 0U)
		return 0U;

	/* Its entry. */
	for (slot = 0U; slot < state->name_count; slot++) {
		if (state->names[slot].cursor == cursor->id)
			return state->names[slot].atom;
	}

	/* None given. */
	return 0U;
}

/* Gives the bytes a region resource is charged for. */
static size_t
xfixes_region_bytes(
	const struct x11_region *region)
{
	/* Succeeded: the resource and its boxes. */
	return sizeof(struct xfixes_region) + region->count * sizeof(struct x11_box);
}

/* Frees a region resource and its charge. */
static void
xfixes_region_free(
	struct x11server *server,
	struct xfixes_region *region)
{
	/* Succeeded: the boxes, the charge and the resource. */
	x11_memory_release(server, region->owner, region->charged);
	x11_region_fini(&region->region);
	free(region);
}

/*
 * Keeps, replaces or forgets (a mask of 0) a client's wish to hear of a
 * selection (0: the cursor) on a window.  Returns 0, or BadAlloc.
 */
static unsigned
xfixes_select(
	struct x11_xfixes *state,
	unsigned client,
	uint32_t window,
	uint32_t selection,
	uint32_t mask)
{
	struct xfixes_select *grown;
	unsigned capacity;
	unsigned slot;

	/* The wish already kept. */
	for (slot = 0U; slot < state->select_count; slot++) {
		if (state->selects[slot].client != client)
			continue;
		if (state->selects[slot].window != window)
			continue;
		if (state->selects[slot].selection != selection)
			continue;
		break;
	}

	/* Forgotten. */
	if (mask == 0U) {
		if (slot < state->select_count) {
			state->selects[slot] = state->selects[state->select_count - 1U];
			state->select_count--;
		}
		return 0U;
	}

	/* Replaced. */
	if (slot < state->select_count) {
		state->selects[slot].mask = mask;
		return 0U;
	}

	/* A new one, the list grown when full. */
	if (state->select_count == state->select_capacity) {
		capacity = state->select_capacity * 2U;
		if (capacity == 0U)
			capacity = 8U;
		grown = realloc(state->selects, capacity * sizeof(*grown));
		if (grown == NULL)
			return X11_BAD_ALLOC;
		state->selects = grown;
		state->select_capacity = capacity;
	}

	/* Succeeded: kept. */
	state->selects[state->select_count].client = client;
	state->selects[state->select_count].window = window;
	state->selects[state->select_count].selection = selection;
	state->selects[state->select_count].mask = mask;
	state->select_count++;
	return 0U;
}

/* Frees a closed client's resources of a kind (regions or barriers). */
static void
xfixes_release(
	struct x11server *server,
	unsigned owner,
	unsigned type)
{
	struct xfixes_barrier *barrier;
	void *object;
	uint32_t *ids;
	size_t count;
	size_t index;
	int error;

	/* The client's resources of the kind, listed. */
	error = x11_resource_owned(server, owner, type, &ids, &count);
	if (error != 0)
		return;

	/* Each one, and its charge. */
	for (index = 0U; index < count; index++) {
		object = x11_resource_find(server, ids[index], type);
		x11_resource_remove(server, ids[index]);
		if (type == X11_RESOURCE_REGION) {
			xfixes_region_free(server, object);
		} else {
			barrier = object;
			x11_memory_release(server, barrier->owner, sizeof(*barrier));
			free(barrier);
		}
	}

	/* Succeeded: none is left. */
	free(ids);
}
