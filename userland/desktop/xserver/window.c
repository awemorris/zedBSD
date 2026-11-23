/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window tree and its requests, and drawing into windows and pixmaps.
 *
 * Each window keeps its children in stacking order, bottom first, and its
 * own pixels, so it can be shown again without its client; places are
 * kept on the root window (a window's origin, inside its border) and
 * given to clients relative to the parent, as the protocol does.  The
 * requests that would change a window another client manages (one that
 * selected SubstructureRedirect on its parent, or ResizeRedirect on it)
 * are passed to that client as MapRequest, ConfigureRequest,
 * CirculateRequest or ResizeRequest instead.  The rootless server is
 * itself the window manager of the root window's children, so no client
 * may redirect them.  What changes is gathered into one box
 * (x11_mark_dirty) that rootless.c shows at the end of a pass.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The colour text is drawn in without a graphics context, and a new window's colour when its creator gives none. */
#define WINDOW_TEXT_COLOR	0xffffffU
#define WINDOW_BACKGROUND	0x607080U

/* The window attributes' value bits, the last of them, and those an InputOnly window may have. */
#define ATTRIBUTE_BACKGROUND_PIXMAP	0U
#define ATTRIBUTE_BACKGROUND_PIXEL	1U
#define ATTRIBUTE_BORDER_PIXMAP		2U
#define ATTRIBUTE_BORDER_PIXEL		3U
#define ATTRIBUTE_BIT_GRAVITY		4U
#define ATTRIBUTE_WIN_GRAVITY		5U
#define ATTRIBUTE_BACKING_STORE		6U
#define ATTRIBUTE_BACKING_PLANES	7U
#define ATTRIBUTE_BACKING_PIXEL		8U
#define ATTRIBUTE_OVERRIDE_REDIRECT	9U
#define ATTRIBUTE_SAVE_UNDER		10U
#define ATTRIBUTE_EVENT_MASK		11U
#define ATTRIBUTE_DO_NOT_PROPAGATE	12U
#define ATTRIBUTE_COLORMAP		13U
#define ATTRIBUTE_CURSOR		14U
#define ATTRIBUTE_COUNT			15U
#define ATTRIBUTE_INPUT_ONLY		((1U << ATTRIBUTE_WIN_GRAVITY) | (1U << ATTRIBUTE_OVERRIDE_REDIRECT) | (1U << ATTRIBUTE_EVENT_MASK) | \
					 (1U << ATTRIBUTE_DO_NOT_PROPAGATE) | (1U << ATTRIBUTE_CURSOR))

/* The background pixmap's special values. */
#define BACKGROUND_NONE			0U
#define BACKGROUND_PARENT_RELATIVE	1U

/* The gravities: unmap, the nine places, and static; and the largest backing-store value. */
#define GRAVITY_UNMAP			0U
#define GRAVITY_NORTH_WEST		1U
#define GRAVITY_STATIC			10U
#define BACKING_STORE_MAX		2U

/* ConfigureWindow's value bits, the last of them, and the stacking modes. */
#define CONFIGURE_X			0U
#define CONFIGURE_Y			1U
#define CONFIGURE_WIDTH			2U
#define CONFIGURE_HEIGHT		3U
#define CONFIGURE_BORDER		4U
#define CONFIGURE_SIBLING		5U
#define CONFIGURE_STACK_MODE		6U
#define CONFIGURE_COUNT			7U
#define STACK_ABOVE			0U
#define STACK_BELOW			1U
#define STACK_TOP_IF			2U
#define STACK_BOTTOM_IF			3U
#define STACK_OPPOSITE			4U

/* CirculateWindow's directions, and the places CirculateNotify names. */
#define CIRCULATE_RAISE_LOWEST		0U
#define CIRCULATE_LOWER_HIGHEST		1U
#define PLACE_ON_TOP			0U
#define PLACE_ON_BOTTOM			1U

/* A window's map state as GetWindowAttributes gives it. */
#define MAP_STATE_UNMAPPED		0U
#define MAP_STATE_UNVIEWABLE		1U
#define MAP_STATE_VIEWABLE		2U

/* ChangeSaveSet's modes. */
#define SAVE_SET_DELETE			1U

/* The requests of which one handler answers two: DestroyWindow, MapWindow and UnmapWindow (the others are their Subwindows forms). */
#define REQUEST_DESTROY_WINDOW		4U
#define REQUEST_MAP_WINDOW		8U
#define REQUEST_UNMAP_WINDOW		10U

/*
 * The values of the window attributes a request gives: which bits it
 * names, and each value.
 */
struct window_values {
	uint32_t mask;
	uint32_t value[ATTRIBUTE_COUNT];
};

/*
 * A ConfigureWindow's new configuration: the bits named, the place
 * relative to the parent, the size, the border, the sibling and the
 * stacking mode.
 */
struct window_configure {
	uint32_t mask;
	int x;
	int y;
	uint16_t width;
	uint16_t height;
	uint16_t border;
	struct x11_window *sibling;
	unsigned stack_mode;
};

static unsigned window_values_read(struct x11server *server, unsigned index, const struct x11_window *window, uint32_t mask, const uint8_t *values, struct window_values *read);
static unsigned window_values_apply(struct x11server *server, unsigned index, struct x11_window *window, const struct window_values *values);
static unsigned window_pixels(struct x11server *server, struct x11_window *window, uint16_t width, uint16_t height);
static void window_link(struct x11_window *parent, struct x11_window *window, struct x11_window *under);
static void window_unlink(struct x11_window *window);
static struct x11_window *window_next_over(struct x11_window *window, const struct x11_window *top);
static void window_move(struct x11_window *window, int dx, int dy);
static int window_relative_x(const struct x11_window *window);
static int window_relative_y(const struct x11_window *window);
static void window_map(struct x11server *server, unsigned requester, struct x11_window *window);
static void window_unmap(struct x11server *server, struct x11_window *window, int from_configure);
static void window_expose_tree(struct x11server *server, struct x11_window *window);
static void window_configure_notify(struct x11server *server, struct x11_window *window);
static void window_configure_synthetic(struct x11server *server, struct x11_window *window, unsigned client);
static unsigned window_configure_read(struct x11server *server, struct x11_window *window, uint32_t mask, const uint8_t *values, int msb, struct window_configure *configure);
static void window_configure_request(struct x11server *server, unsigned redirect, struct x11_window *window, const struct window_configure *configure);
static void window_gravity(struct x11server *server, struct x11_window *window, int old_width, int old_height, int dx, int dy);
static int window_occludes(const struct x11_window *upper, const struct x11_window *lower);
static int window_occluded(const struct x11_window *window, const struct x11_window *sibling);
static int window_occluding(const struct x11_window *window, const struct x11_window *sibling);
static int window_restack(struct x11server *server, struct x11_window *window, struct x11_window *sibling, unsigned mode);
static void window_free(struct x11server *server, struct x11_window *window);
static void window_destroy_tree(struct x11server *server, struct x11_window *top);
static struct x11_window *window_first_leaf(struct x11_window *window);
static void window_release_windows(struct x11server *server, unsigned owner);
static void window_release_kind(struct x11server *server, unsigned owner, unsigned type);

/*
 * Makes the root window: the server's, mapped, the size of the screen,
 * with no pixels (nothing shows it).  Returns 0, or an errno value.
 */
int
x11_window_root(
	struct x11server *server,
	uint32_t background)
{
	struct x11_window *root;
	int error;

	/* The window. */
	root = calloc(1U, sizeof(*root));
	if (root == NULL)
		return ENOMEM;
	root->id = X11_ROOT_XID;
	root->owner = X11_NO_CLIENT;
	root->window_class = X11_INPUT_OUTPUT;
	root->background = background;
	root->win_gravity = GRAVITY_NORTH_WEST;
	root->colormap = X11_COLORMAP_XID;
	root->width = (uint16_t)server->width;
	root->height = (uint16_t)server->height;
	root->mapped = 1;

	/* Found by its id. */
	error = x11_resource_add(server, X11_ROOT_XID, X11_RESOURCE_WINDOW, root);
	if (error != 0) {
		free(root);
		return error;
	}

	/* Succeeded: the tree's root. */
	server->root = root;
	return 0;
}

/*
 * Makes a window of the server's own (owned by no client): an unmapped
 * 1x1 InputOnly child of the root window, under its other children, at an
 * id no client chooses.  The window manager names it in EWMH's
 * _NET_SUPPORTING_WM_CHECK.  Returns 0, or an errno value.
 */
int
x11_window_server_child(
	struct x11server *server,
	uint32_t id)
{
	struct x11_window *window;
	int error;

	/* The server holds the window's memory. */
	error = x11_memory_charge(server, X11_NO_CLIENT, sizeof(*window));
	if (error != 0)
		return error;

	/* The window, off the screen. */
	window = calloc(1U, sizeof(*window));
	if (window == NULL) {
		x11_memory_release(server, X11_NO_CLIENT, sizeof(*window));
		return ENOMEM;
	}
	window->id = id;
	window->owner = X11_NO_CLIENT;
	window->window_class = X11_INPUT_ONLY;
	window->win_gravity = GRAVITY_NORTH_WEST;
	window->colormap = X11_COLORMAP_XID;
	window->x = -100;
	window->y = -100;
	window->width = 1U;
	window->height = 1U;

	/* Found by its id. */
	error = x11_resource_add(server, id, X11_RESOURCE_WINDOW, window);
	if (error != 0) {
		free(window);
		x11_memory_release(server, X11_NO_CLIENT, sizeof(*window));
		return error;
	}

	/* Succeeded: at the bottom of the root window's children. */
	window_link(server->root, window, server->root->first_child);
	return 0;
}

/*
 * Maps a window for the window manager (a minimized window brought back),
 * as its owner's MapWindow would.
 */
void
x11_window_map(
	struct x11server *server,
	struct x11_window *window)
{
	/* Succeeded: mapped, its clients told. */
	window_map(server, window->owner, window);
}

/*
 * Unmaps a window for the window manager (a window minimized), as
 * UnmapWindow would.
 */
void
x11_window_unmap(
	struct x11server *server,
	struct x11_window *window)
{
	/* Succeeded: unmapped, its clients told. */
	window_unmap(server, window, 0);
}

/*
 * Returns the window of an id, or NULL.
 */
struct x11_window *
x11_window_find(
	struct x11server *server,
	uint32_t id)
{
	struct x11_window *window;

	/* The id's resource, when it is a window. */
	window = x11_resource_find(server, id, X11_RESOURCE_WINDOW);
	if (window == NULL)
		return NULL;

	/* Succeeded: the window. */
	return window;
}

/*
 * Returns the pixmap of an id, or NULL.
 */
struct x11_pixmap *
x11_pixmap_find(
	struct x11server *server,
	uint32_t id)
{
	struct x11_pixmap *pixmap;

	/* The id's resource, when it is a pixmap. */
	pixmap = x11_resource_find(server, id, X11_RESOURCE_PIXMAP);
	if (pixmap == NULL)
		return NULL;

	/* Succeeded: the pixmap. */
	return pixmap;
}

/*
 * Returns the graphics context of an id, or NULL.
 */
struct x11_gc *
x11_gc_find(
	struct x11server *server,
	uint32_t id)
{
	struct x11_gc *gc;

	/* The id's resource, when it is a graphics context. */
	gc = x11_resource_find(server, id, X11_RESOURCE_GC);
	if (gc == NULL)
		return NULL;

	/* Succeeded: the context. */
	return gc;
}

/*
 * CreateWindow: a window on top of its parent's children, of the class,
 * depth and visual asked (24 bits TrueColor, or InputOnly), with the
 * attributes its values give.  The parent's SubstructureNotify clients
 * are told.
 */
unsigned
x11_request_create_window(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct window_values values;
	struct x11_client *client;
	struct x11_window *parent;
	struct x11_window *window;
	uint8_t event[32];
	uint32_t mask;
	uint32_t visual;
	uint32_t id;
	uint16_t width;
	uint16_t height;
	uint16_t border;
	unsigned window_class;
	unsigned taken;
	unsigned code;
	int fits;
	int chosen;
	int error;
	int msb;

	/* The fixed part and one value for each bit of the mask. */
	client = &server->clients[index];
	msb = client->order;
	if (length < 32U)
		return X11_BAD_LENGTH;
	mask = x11_read32(request + 28, msb);
	if ((mask >> ATTRIBUTE_COUNT) != 0U)
		return X11_BAD_VALUE;
	fits = x11_values_fit(mask, 32U, length);
	if (!fits)
		return X11_BAD_LENGTH;

	/* An id of the client's range not in use. */
	id = x11_read32(request + 4, msb);
	chosen = x11_id_chosen(client, id);
	if (!chosen)
		return X11_BAD_ID_CHOICE;
	taken = x11_resource_type(server, id);
	if (taken != 0U)
		return X11_BAD_ID_CHOICE;

	/* The parent. */
	parent = x11_window_find(server, x11_read32(request + 8, msb));
	if (parent == NULL)
		return X11_BAD_WINDOW;

	/* The class (CopyFromParent takes the parent's), and a size. */
	window_class = x11_read16(request + 22, msb);
	if (window_class > X11_INPUT_ONLY)
		return X11_BAD_VALUE;
	if (window_class == 0U)
		window_class = parent->window_class;
	width = x11_read16(request + 16, msb);
	height = x11_read16(request + 18, msb);
	border = x11_read16(request + 20, msb);
	if (width == 0U || height == 0U)
		return X11_BAD_VALUE;

	/* The depth and visual the class allows: the screen's for InputOutput, none for InputOnly. */
	visual = x11_read32(request + 24, msb);
	if (visual != 0U && visual != X11_VISUAL_XID)
		return X11_BAD_MATCH;
	if (window_class == X11_INPUT_OUTPUT) {
		if (parent->window_class == X11_INPUT_ONLY)
			return X11_BAD_MATCH;
		if (request[1] != 0U && request[1] != X11_DEPTH)
			return X11_BAD_MATCH;
	} else {
		if (request[1] != 0U || border != 0U)
			return X11_BAD_MATCH;
		if ((mask & ~ATTRIBUTE_INPUT_ONLY) != 0U)
			return X11_BAD_MATCH;
	}

	/* The window, empty for its values to be checked against. */
	window = calloc(1U, sizeof(*window));
	if (window == NULL)
		return X11_BAD_ALLOC;
	window->id = id;
	window->owner = index;
	window->parent = parent;
	window->window_class = window_class;
	window->background = WINDOW_BACKGROUND;
	window->win_gravity = GRAVITY_NORTH_WEST;
	window->colormap = X11_COLORMAP_XID;
	if (window_class == X11_INPUT_ONLY)
		window->colormap = 0U;
	window->width = width;
	window->height = height;
	window->border = border;
	window->x = parent->x + (int16_t)x11_read16(request + 12, msb) + window->border;
	window->y = parent->y + (int16_t)x11_read16(request + 14, msb) + window->border;

	/* Its values, every one checked before any is kept. */
	code = window_values_read(server, index, window, mask, request + 32, &values);
	if (code != 0U) {
		free(window);
		return code;
	}

	/* The background it is cleared to. */
	if ((values.mask & (1U << ATTRIBUTE_BACKGROUND_PIXEL)) != 0U)
		window->background = values.value[ATTRIBUTE_BACKGROUND_PIXEL];

	/* The client holds the window and its pixels. */
	error = x11_memory_charge(server, index, sizeof(*window));
	if (error != 0) {
		free(window);
		return X11_BAD_ALLOC;
	}
	code = window_pixels(server, window, window->width, window->height);
	if (code != 0U) {
		x11_memory_release(server, index, sizeof(*window));
		free(window);
		return code;
	}

	/* Found by its id. */
	error = x11_resource_add(server, id, X11_RESOURCE_WINDOW, window);
	if (error != 0) {
		window_free(server, window);
		return X11_BAD_ALLOC;
	}

	/* Its values, on top of its siblings. */
	window_link(parent, window, NULL);
	code = window_values_apply(server, index, window, &values);
	if (code != 0U) {
		window_free(server, window);
		return code;
	}

	/* The parent's clients are told. */
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_CREATE_NOTIFY;
	x11_write32(event + 4, parent->id, 0);
	x11_write32(event + 8, window->id, 0);
	x11_write16(event + 12, (uint16_t)window_relative_x(window), 0);
	x11_write16(event + 14, (uint16_t)window_relative_y(window), 0);
	x11_write16(event + 16, window->width, 0);
	x11_write16(event + 18, window->height, 0);
	x11_write16(event + 20, window->border, 0);
	event[22] = window->override_redirect;
	(void)x11_event_deliver(server, parent, X11_MASK_SUBSTRUCTURE_NOTIFY, event);

	/* Succeeded: the window is in the tree, unmapped. */
	return 0U;
}

/* ChangeWindowAttributes: the attributes the values give, every one checked before any is kept. */
unsigned
x11_request_change_attributes(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct window_values values;
	struct x11_window *window;
	uint32_t mask;
	unsigned code;
	int fits;
	int msb;

	/* The window, and one value for each bit of the mask. */
	msb = server->clients[index].order;
	if (length < 12U)
		return X11_BAD_LENGTH;
	mask = x11_read32(request + 8, msb);
	if ((mask >> ATTRIBUTE_COUNT) != 0U)
		return X11_BAD_VALUE;
	fits = x11_values_fit(mask, 12U, length);
	if (!fits)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;
	if (window->window_class == X11_INPUT_ONLY && (mask & ~ATTRIBUTE_INPUT_ONLY) != 0U)
		return X11_BAD_MATCH;

	/* The values, checked. */
	code = window_values_read(server, index, window, mask, request + 12, &values);
	if (code != 0U)
		return code;

	/* Kept. */
	code = window_values_apply(server, index, window, &values);
	if (code != 0U)
		return code;

	/* Succeeded: the attributes are changed. */
	return 0U;
}

/* GetWindowAttributes: the window's attributes, map state, and the events asked for on it. */
unsigned
x11_request_get_attributes(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *window;
	uint8_t reply[44];
	unsigned state;
	int viewable;
	int msb;

	/* The window. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* Its map state. */
	state = MAP_STATE_UNMAPPED;
	viewable = x11_window_viewable(window);
	if (viewable) {
		state = MAP_STATE_VIEWABLE;
	} else if (window->mapped) {
		state = MAP_STATE_UNVIEWABLE;
	}

	/* The reply. */
	memset(reply, 0, sizeof(reply));
	reply[1] = window->backing_store;
	x11_write32(reply + 8, X11_VISUAL_XID, msb);
	x11_write16(reply + 12, (uint16_t)window->window_class, msb);
	reply[14] = window->bit_gravity;
	reply[15] = window->win_gravity;
	x11_write32(reply + 16, window->backing_planes, msb);
	x11_write32(reply + 20, window->backing_pixel, msb);
	reply[24] = window->save_under;
	reply[25] = 1U;
	reply[26] = (uint8_t)state;
	reply[27] = window->override_redirect;
	x11_write32(reply + 28, window->colormap, msb);
	x11_write32(reply + 32, x11_event_masks(window), msb);
	x11_write32(reply + 36, x11_event_mask_of(window, index), msb);
	x11_write16(reply + 40, (uint16_t)window->do_not_propagate, msb);
	x11_reply(server, client, reply, sizeof(reply));

	/* Succeeded: answered. */
	return 0U;
}

/* DestroyWindow (the window and its inferiors) and DestroySubwindows (its children, bottom to top). */
unsigned
x11_request_destroy_window(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;

	/* The window. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, server->clients[index].order));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* DestroyWindow: the window itself (the root window stays). */
	if (request[0] == REQUEST_DESTROY_WINDOW) {
		x11_window_destroy(server, window);
		return 0U;
	}

	/* Succeeded: DestroySubwindows takes each child from the bottom. */
	while (window->first_child != NULL)
		x11_window_destroy(server, window->first_child);
	return 0U;
}

/*
 * ChangeSaveSet: a window of another client into the save-set or out of
 * it.  The rootless server reparents no client's window, so nothing is
 * kept for the client's close.
 */
unsigned
x11_request_change_save_set(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;

	/* The mode and a window of another client. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	if (request[1] > SAVE_SET_DELETE)
		return X11_BAD_VALUE;
	window = x11_window_find(server, x11_read32(request + 4, server->clients[index].order));
	if (window == NULL)
		return X11_BAD_WINDOW;
	if (window->owner == index || window == server->root)
		return X11_BAD_MATCH;

	/* Succeeded: accepted. */
	return 0U;
}

/*
 * ReparentWindow: the window, unmapped first when it is mapped, on top of
 * a new parent's children at a place relative to it; told to the window's
 * and both parents' clients, then mapped again.
 */
unsigned
x11_request_reparent_window(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_window *parent;
	struct x11_window *old_parent;
	uint8_t event[32];
	int was_mapped;
	int inside;
	int x;
	int y;
	int msb;

	/* The window and the new parent, which must not be the window or one of its inferiors. */
	msb = server->clients[index].order;
	if (length != 16U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;
	parent = x11_window_find(server, x11_read32(request + 8, msb));
	if (parent == NULL)
		return X11_BAD_WINDOW;
	if (window == server->root)
		return X11_BAD_MATCH;
	inside = x11_window_inferior(parent, window);
	if (inside)
		return X11_BAD_MATCH;
	if (window->window_class == X11_INPUT_OUTPUT && parent->window_class == X11_INPUT_ONLY)
		return X11_BAD_MATCH;

	/* A mapped window is unmapped first. */
	was_mapped = window->mapped;
	if (was_mapped)
		window_unmap(server, window, 0);

	/* A top-level window that leaves the root window leaves its surface. */
	old_parent = window->parent;
	if (old_parent == server->root)
		x11_rootless_forget(server, window);

	/* On top of the new parent's children, at its place there; the inferiors move with it. */
	x11_mark_dirty(server, window->x, window->y, window->width, window->height);
	window_unlink(window);
	window_link(parent, window, NULL);
	x = parent->x + (int16_t)x11_read16(request + 12, msb) + window->border;
	y = parent->y + (int16_t)x11_read16(request + 14, msb) + window->border;
	window_move(window, x - window->x, y - window->y);

	/* The window's clients, then the old and the new parent's, are told. */
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_REPARENT_NOTIFY;
	x11_write32(event + 8, window->id, 0);
	x11_write32(event + 12, parent->id, 0);
	x11_write16(event + 16, (uint16_t)window_relative_x(window), 0);
	x11_write16(event + 18, (uint16_t)window_relative_y(window), 0);
	event[20] = window->override_redirect;
	x11_write32(event + 4, window->id, 0);
	(void)x11_event_deliver(server, window, X11_MASK_STRUCTURE_NOTIFY, event);
	x11_write32(event + 4, old_parent->id, 0);
	(void)x11_event_deliver(server, old_parent, X11_MASK_SUBSTRUCTURE_NOTIFY, event);
	x11_write32(event + 4, parent->id, 0);
	(void)x11_event_deliver(server, parent, X11_MASK_SUBSTRUCTURE_NOTIFY, event);

	/* Succeeded: mapped again when it was mapped. */
	if (was_mapped)
		window_map(server, index, window);
	return 0U;
}

/* MapWindow (the window) and MapSubwindows (its unmapped children, top to bottom). */
unsigned
x11_request_map_window(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_window *child;
	struct x11_window *next;

	/* The window. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, server->clients[index].order));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* MapWindow: the window itself. */
	if (request[0] == REQUEST_MAP_WINDOW) {
		window_map(server, index, window);
		return 0U;
	}

	/* Succeeded: MapSubwindows maps each child from the top. */
	for (child = window->last_child; child != NULL; child = next) {
		next = child->below;
		window_map(server, index, child);
	}
	return 0U;
}

/* UnmapWindow (the window) and UnmapSubwindows (its mapped children, bottom to top). */
unsigned
x11_request_unmap_window(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_window *child;

	/* The window. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, server->clients[index].order));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* UnmapWindow: the window itself (never the root window). */
	if (request[0] == REQUEST_UNMAP_WINDOW) {
		if (window != server->root)
			window_unmap(server, window, 0);
		return 0U;
	}

	/* Succeeded: UnmapSubwindows unmaps each child from the bottom. */
	for (child = window->first_child; child != NULL; child = child->above)
		window_unmap(server, child, 0);
	return 0U;
}

/*
 * ConfigureWindow: a new place (relative to the parent), size, border and
 * place in the stack; asked of the client that redirects the parent's
 * children instead, and a new size of the one that redirects the window's
 * resizes.  The window's clients are told, its children follow their
 * gravity, and a new size is drawn again.
 */
unsigned
x11_request_configure_window(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct window_configure configure;
	struct x11_client *asked;
	struct x11_window *window;
	uint8_t event[32];
	uint32_t mask;
	unsigned redirect;
	unsigned code;
	int old_x;
	int old_y;
	int old_width;
	int old_height;
	int new_x;
	int new_y;
	int resized;
	int border_refused;
	int fits;
	int msb;

	/* The window, and one value for each bit of the mask. */
	msb = server->clients[index].order;
	if (length < 12U)
		return X11_BAD_LENGTH;
	mask = x11_read16(request + 8, msb);
	if ((mask >> CONFIGURE_COUNT) != 0U)
		return X11_BAD_VALUE;
	fits = x11_values_fit(mask, 12U, length);
	if (!fits)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* The new configuration, checked. */
	code = window_configure_read(server, window, mask, request + 12, msb, &configure);
	if (code != 0U)
		return code;

	/* The root window does not move. */
	if (window == server->root)
		return 0U;

	/* Another client that redirects the parent's children decides. */
	redirect = x11_event_selector(window->parent, X11_MASK_SUBSTRUCTURE_REDIRECT);
	if (!window->override_redirect && redirect != X11_NO_CLIENT && redirect != index) {
		window_configure_request(server, redirect, window, &configure);
		return 0U;
	}

	/*
	 * The rootless server, as the top-level windows' window manager, keeps
	 * them without a border: a border asked for is 0, and a request that
	 * then changes nothing is answered with a synthetic ConfigureNotify
	 * (ICCCM 4.1.5), so the client hears what it has.
	 */
	border_refused = 0;
	if (window->parent == server->root && !window->override_redirect) {
		if ((configure.mask & (1U << CONFIGURE_BORDER)) != 0U && configure.border != 0U)
			border_refused = 1;
		configure.border = 0U;
	}

	/* Another client that redirects the window's resizes is asked for the size; the rest goes on. */
	resized = 0;
	if (configure.width != window->width || configure.height != window->height)
		resized = 1;
	redirect = x11_event_selector(window, X11_MASK_RESIZE_REDIRECT);
	asked = x11_client_of(server, redirect);
	if (resized && asked != NULL && redirect != index) {
		memset(event, 0, sizeof(event));
		event[0] = X11_EVENT_RESIZE_REQUEST;
		x11_write32(event + 4, window->id, 0);
		x11_write16(event + 8, configure.width, 0);
		x11_write16(event + 10, configure.height, 0);
		x11_event_send(server, asked, event);
		configure.width = window->width;
		configure.height = window->height;
		resized = 0;
	}

	/* Pixels of the new size, keeping what fits. */
	old_x = window->x;
	old_y = window->y;
	old_width = window->width;
	old_height = window->height;
	if (resized) {
		code = window_pixels(server, window, configure.width, configure.height);
		if (code != 0U)
			return code;
	}

	/* The new configuration; the inferiors move with the window. */
	x11_mark_dirty(server, old_x - window->border, old_y - window->border, old_width + 2 * window->border, old_height + 2 * window->border);
	new_x = window->parent->x + configure.x + configure.border;
	new_y = window->parent->y + configure.y + configure.border;
	window_move(window, new_x - old_x, new_y - old_y);
	window->width = configure.width;
	window->height = configure.height;
	window->border = configure.border;
	if ((configure.mask & (1U << CONFIGURE_STACK_MODE)) != 0U)
		(void)window_restack(server, window, configure.sibling, configure.stack_mode);
	x11_mark_dirty(server, window->x - window->border, window->y - window->border, window->width + 2 * window->border, window->height + 2 * window->border);

	/* A refused border that left everything as it was: the synthetic ConfigureNotify alone. */
	if (border_refused &&
	    new_x == old_x &&
	    new_y == old_y &&
	    !resized &&
	    (configure.mask & (1U << CONFIGURE_STACK_MODE)) == 0U) {
		window_configure_synthetic(server, window, index);
		return 0U;
	}

	/* The window's clients are told, its children follow their gravity, and a new size is drawn again. */
	window_configure_notify(server, window);
	if (resized) {
		window_gravity(server, window, old_width, old_height, new_x - old_x, new_y - old_y);
		window_expose_tree(server, window);
	}

	/* Succeeded: the window is configured. */
	return 0U;
}

/*
 * CirculateWindow: the lowest of the window's mapped children that another
 * covers raised to the top, or the highest that covers another lowered to
 * the bottom; asked of the client that redirects the window's children
 * instead.
 */
unsigned
x11_request_circulate_window(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *asked;
	struct x11_window *window;
	struct x11_window *child;
	struct x11_window *chosen;
	uint8_t event[32];
	unsigned redirect;
	unsigned place;
	int covered;

	/* The direction and the window whose children circulate. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	if (request[1] > CIRCULATE_LOWER_HIGHEST)
		return X11_BAD_VALUE;
	window = x11_window_find(server, x11_read32(request + 4, server->clients[index].order));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* The child that moves: the lowest covered one, or the highest covering one. */
	chosen = NULL;
	place = PLACE_ON_TOP;
	if (request[1] == CIRCULATE_RAISE_LOWEST) {
		for (child = window->first_child; child != NULL && chosen == NULL; child = child->above) {
			covered = window_occluded(child, NULL);
			if (child->mapped && covered)
				chosen = child;
		}
	} else {
		place = PLACE_ON_BOTTOM;
		for (child = window->last_child; child != NULL && chosen == NULL; child = child->below) {
			covered = window_occluding(child, NULL);
			if (child->mapped && covered)
				chosen = child;
		}
	}

	/* None moves. */
	if (chosen == NULL)
		return 0U;

	/* Another client that redirects the children decides. */
	memset(event, 0, sizeof(event));
	x11_write32(event + 4, window->id, 0);
	x11_write32(event + 8, chosen->id, 0);
	event[16] = (uint8_t)place;
	redirect = x11_event_selector(window, X11_MASK_SUBSTRUCTURE_REDIRECT);
	asked = x11_client_of(server, redirect);
	if (asked != NULL && redirect != index) {
		event[0] = X11_EVENT_CIRCULATE_REQUEST;
		x11_event_send(server, asked, event);
		return 0U;
	}

	/* Succeeded: moved, and told to the child's and the window's clients. */
	if (place == PLACE_ON_TOP) {
		(void)window_restack(server, chosen, NULL, STACK_ABOVE);
	} else {
		(void)window_restack(server, chosen, NULL, STACK_BELOW);
	}
	event[0] = X11_EVENT_CIRCULATE_NOTIFY;
	x11_event_structure(server, chosen, event);
	return 0U;
}

/* GetGeometry: a window's or a pixmap's root, depth, place relative to the parent, size and border. */
unsigned
x11_request_get_geometry(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *window;
	struct x11_pixmap *pixmap;
	uint8_t reply[32];
	uint32_t drawable;
	int msb;

	/* The drawable: a window or a pixmap. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 8U)
		return X11_BAD_LENGTH;
	drawable = x11_read32(request + 4, msb);
	window = x11_window_find(server, drawable);
	pixmap = x11_pixmap_find(server, drawable);
	if (window == NULL && pixmap == NULL)
		return X11_BAD_DRAWABLE;

	/* The reply: the root, then the window's geometry or the pixmap's size. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, X11_ROOT_XID, msb);
	if (window != NULL) {
		reply[1] = X11_DEPTH;
		if (window->window_class == X11_INPUT_ONLY)
			reply[1] = 0U;
		x11_write16(reply + 12, (uint16_t)window_relative_x(window), msb);
		x11_write16(reply + 14, (uint16_t)window_relative_y(window), msb);
		x11_write16(reply + 16, window->width, msb);
		x11_write16(reply + 18, window->height, msb);
		x11_write16(reply + 20, window->border, msb);
	} else {
		reply[1] = X11_DEPTH;
		x11_write16(reply + 16, pixmap->width, msb);
		x11_write16(reply + 18, pixmap->height, msb);
	}
	x11_reply(server, client, reply, sizeof(reply));

	/* Succeeded: answered. */
	return 0U;
}

/* QueryTree: the root, the parent and the children in stacking order, bottom first. */
unsigned
x11_request_query_tree(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *window;
	struct x11_window *child;
	uint8_t *reply;
	size_t count;
	int msb;

	/* The window. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* Its children, counted. */
	count = 0U;
	for (child = window->first_child; child != NULL; child = child->above)
		count++;
	if (count > 0xffffU)
		return X11_BAD_ALLOC;

	/* The reply: the root, the parent, the count and each child. */
	reply = calloc(1U, 32U + count * 4U);
	if (reply == NULL)
		return X11_BAD_ALLOC;
	x11_write32(reply + 8, X11_ROOT_XID, msb);
	if (window->parent != NULL)
		x11_write32(reply + 12, window->parent->id, msb);
	x11_write16(reply + 16, (uint16_t)count, msb);
	count = 0U;
	for (child = window->first_child; child != NULL; child = child->above) {
		x11_write32(reply + 32 + count * 4U, child->id, msb);
		count++;
	}
	x11_reply(server, client, reply, 32U + count * 4U);
	free(reply);

	/* Succeeded: answered. */
	return 0U;
}

/* TranslateCoordinates: a place in one window as a place in another, and the other's mapped child there. */
unsigned
x11_request_translate_coordinates(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *source;
	struct x11_window *target;
	struct x11_window *child;
	uint8_t reply[32];
	uint32_t found;
	int x;
	int y;
	int msb;

	/* The two windows. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 16U)
		return X11_BAD_LENGTH;
	source = x11_window_find(server, x11_read32(request + 4, msb));
	if (source == NULL)
		return X11_BAD_WINDOW;
	target = x11_window_find(server, x11_read32(request + 8, msb));
	if (target == NULL)
		return X11_BAD_WINDOW;

	/* The place on the root window, and the target's topmost mapped child holding it (its border too). */
	x = source->x + (int16_t)x11_read16(request + 12, msb);
	y = source->y + (int16_t)x11_read16(request + 14, msb);
	found = 0U;
	for (child = target->last_child; child != NULL && found == 0U; child = child->below) {
		if (!child->mapped)
			continue;
		if (x < child->x - child->border ||
		    y < child->y - child->border ||
		    x >= child->x + child->width + child->border ||
		    y >= child->y + child->height + child->border)
			continue;
		found = child->id;
	}

	/* Succeeded: on the same screen, the child and the place in the target. */
	memset(reply, 0, sizeof(reply));
	reply[1] = 1U;
	x11_write32(reply + 8, found, msb);
	x11_write16(reply + 12, (uint16_t)(x - target->x), msb);
	x11_write16(reply + 14, (uint16_t)(y - target->y), msb);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * Destroys a window and its inferiors: unmapped first when mapped, then
 * DestroyNotify for each, the inferiors before the window, to its and its
 * parent's clients.  The root window is never destroyed.
 */
void
x11_window_destroy(
	struct x11server *server,
	struct x11_window *window)
{
	/* The root window stays. */
	if (window == server->root)
		return;

	/* A mapped window is unmapped first. */
	if (window->mapped)
		window_unmap(server, window, 0);

	/* Succeeded: the tree goes, from its leaves. */
	window_destroy_tree(server, window);
}

/*
 * Puts a window on top of its siblings (the rootless server's own
 * stacking, as its window manager).
 */
void
x11_window_raise(
	struct x11server *server,
	struct x11_window *window)
{
	/* The root window has no siblings. */
	if (window == NULL || window->parent == NULL)
		return;

	/* Succeeded: on top. */
	(void)window_restack(server, window, NULL, STACK_ABOVE);
}

/*
 * Gives a top-level window the place (X11_SURFACE_ANY: the same) and size
 * the system gave its surface: its clients are told, its children follow
 * their gravity, and a new size is drawn again by them.  The surface is
 * not told back what it reported.
 */
void
x11_window_backend_configure(
	struct x11server *server,
	struct x11_window *window,
	int x,
	int y,
	uint16_t width,
	uint16_t height)
{
	unsigned code;
	int old_width;
	int old_height;
	int dx;
	int dy;
	int resized;

	/* What changes. */
	dx = 0;
	dy = 0;
	if (x != X11_SURFACE_ANY && y != X11_SURFACE_ANY) {
		dx = x - window->x;
		dy = y - window->y;
	}
	resized = 0;
	if (width != window->width || height != window->height)
		resized = 1;
	if (dx == 0 && dy == 0 && !resized)
		return;

	/* Pixels of the new size. */
	old_width = window->width;
	old_height = window->height;
	if (resized) {
		code = window_pixels(server, window, width, height);
		if (code != 0U)
			return;
	}

	/* The new place (the surface is already there) and size. */
	x11_mark_dirty(server, window->x, window->y, window->width, window->height);
	window_move(window, dx, dy);
	window->surface_x = window->x;
	window->surface_y = window->y;
	window->width = width;
	window->height = height;
	x11_mark_dirty(server, window->x, window->y, window->width, window->height);

	/* Succeeded: told, and a new size drawn again. */
	window_configure_notify(server, window);
	if (resized) {
		window_gravity(server, window, old_width, old_height, dx, dy);
		window_expose_tree(server, window);
	}
}

/*
 * Tells whether a window is viewable: it and every ancestor mapped.
 */
int
x11_window_viewable(
	const struct x11_window *window)
{
	/* Each window up to the root. */
	while (window != NULL) {
		if (!window->mapped)
			return 0;
		window = window->parent;
	}

	/* Succeeded: viewable. */
	return 1;
}

/*
 * Returns the next window after one in a walk of a tree (each window
 * before its children, children bottom to top), or NULL past the tree's
 * last window.
 */
struct x11_window *
x11_window_next(
	struct x11_window *window,
	const struct x11_window *top)
{
	/* Down to the first child. */
	if (window->first_child != NULL)
		return window->first_child;

	/* Succeeded: the next sibling of the window or of an ancestor within the tree. */
	return window_next_over(window, top);
}

/*
 * Tells whether a window is an ancestor's inferior or the ancestor itself.
 */
int
x11_window_inferior(
	const struct x11_window *window,
	const struct x11_window *ancestor)
{
	/* Up from the window. */
	while (window != NULL) {
		if (window == ancestor)
			return 1;
		window = window->parent;
	}

	/* Not in the ancestor's tree. */
	return 0;
}

/*
 * Returns the top-level window (the root window's child) a window is in,
 * or the window itself for the root window.
 */
struct x11_window *
x11_window_top_level(
	struct x11_window *window)
{
	/* Up the tree to the root window's child. */
	while (window->parent != NULL && window->parent->parent != NULL)
		window = window->parent;

	/* Succeeded: the top-level window. */
	return window;
}

/*
 * Returns the deepest viewable window under a place on the root window,
 * or the root window.
 */
struct x11_window *
x11_window_at(
	struct x11server *server,
	int x,
	int y)
{
	struct x11_window *window;
	struct x11_window *child;

	/* Down the tree, into the topmost mapped child under the place each time. */
	window = server->root;
	for (;;) {
		child = x11_window_child_at(window, x, y);
		if (child == NULL)
			break;
		window = child;
	}

	/* Succeeded: the deepest window there. */
	return window;
}

/*
 * Returns the topmost mapped child of a window under a place on the root
 * window, or NULL.
 */
struct x11_window *
x11_window_child_at(
	struct x11_window *parent,
	int x,
	int y)
{
	struct x11_window *child;

	/* The children from the top of the stack down. */
	for (child = parent->last_child; child != NULL; child = child->below) {
		if (!child->mapped)
			continue;
		if (x < child->x ||
		    y < child->y ||
		    x >= child->x + child->width ||
		    y >= child->y + child->height)
			continue;
		return child;
	}

	/* No child is there. */
	return NULL;
}

/*
 * Allocates pixels of a size, each of a colour.  Returns NULL for an empty
 * size or when there is no memory.
 */
uint32_t *
x11_pixels_alloc(
	uint16_t width,
	uint16_t height,
	uint32_t color)
{
	uint32_t *pixels;
	size_t count;
	size_t index;

	/* An empty size has no pixels. */
	if (width == 0U || height == 0U)
		return NULL;

	/* The memory. */
	count = (size_t)width * (size_t)height;
	pixels = malloc(count * sizeof(*pixels));
	if (pixels == NULL)
		return NULL;

	/* Each pixel of the colour. */
	for (index = 0U; index < count; index++)
		pixels[index] = color;

	/* Succeeded: the pixels. */
	return pixels;
}

/*
 * Fills a rectangle of a window (in its own coordinates) with a colour,
 * clipped to the window.
 */
void
x11_window_fill(
	struct x11server *server,
	struct x11_window *window,
	int x,
	int y,
	int width,
	int height,
	uint32_t color)
{
	int row;
	int column;

	/* Clipped to the window. */
	if (x < 0) {
		width += x;
		x = 0;
	}

	/* Clipped at the top. */
	if (y < 0) {
		height += y;
		y = 0;
	}

	/* Clipped at the right and the bottom. */
	if (x + width > window->width)
		width = window->width - x;
	if (y + height > window->height)
		height = window->height - y;
	if (width <= 0 || height <= 0)
		return;

	/* Each pixel. */
	for (row = y; row < y + height; row++) {
		for (column = x; column < x + width; column++)
			window->pixels[(size_t)row * window->width + (size_t)column] = color;
	}

	/* Shown again. */
	x11_mark_dirty(server, window->x + x, window->y + y, width, height);
}

/*
 * Fills a rectangle of a pixmap with a colour, clipped to the pixmap.
 */
void
x11_pixmap_fill(
	struct x11_pixmap *pixmap,
	int x,
	int y,
	int width,
	int height,
	uint32_t color)
{
	int row;
	int column;

	/* Clipped to the pixmap. */
	if (x < 0) {
		width += x;
		x = 0;
	}

	/* Clipped at the top. */
	if (y < 0) {
		height += y;
		y = 0;
	}

	/* Clipped at the right and the bottom. */
	if (x + width > pixmap->width)
		width = pixmap->width - x;
	if (y + height > pixmap->height)
		height = pixmap->height - y;
	if (width <= 0 || height <= 0)
		return;

	/* Each pixel. */
	for (row = y; row < y + height; row++) {
		for (column = x; column < x + width; column++)
			pixmap->pixels[(size_t)row * pixmap->width + (size_t)column] = color;
	}
}

/*
 * Draws characters (bytes, or big-endian pairs when wide) in the core font
 * onto a window, the cells standing on a baseline at y.
 */
void
x11_draw_text(
	struct x11server *server,
	struct x11_window *window,
	const struct x11_gc *gc,
	int x,
	int y,
	const uint8_t *text,
	size_t count,
	int wide)
{
	struct x11_glyph glyph;
	uint32_t codepoint;
	uint32_t color;
	size_t index;
	unsigned bits;
	int glyph_x;
	int glyph_y;
	int pixel_x;
	int pixel_y;
	int top;
	int missing;

	/* The context's colour. */
	color = WINDOW_TEXT_COLOR;
	if (gc != NULL)
		color = gc->foreground;

	/* Each character. */
	for (index = 0U; index < count; index++) {
		codepoint = text[index];
		if (wide)
			codepoint = ((uint32_t)text[index * 2U] << 8) | (uint32_t)text[index * 2U + 1U];

		/* Its glyph; a character without one is skipped. */
		missing = x11_glyph(&server->glyphs, codepoint, &glyph);
		if (missing != 0)
			continue;

		/* Each set bit of the cell that lands in the window. */
		top = y - (int)glyph.height;
		for (glyph_y = 0; glyph_y < (int)glyph.height; glyph_y++) {
			for (glyph_x = 0; glyph_x < (int)glyph.width; glyph_x++) {
				pixel_x = x + glyph_x;
				pixel_y = top + glyph_y;
				if (pixel_x < 0 ||
				    pixel_y < 0 ||
				    pixel_x >= window->width ||
				    pixel_y >= window->height)
					continue;
				bits = glyph.bitmap[(size_t)glyph_y * glyph.stride + (unsigned)glyph_x / 8U];
				if ((bits & (0x80U >> ((unsigned)glyph_x & 7U))) != 0U)
					window->pixels[(size_t)pixel_y * window->width + (size_t)pixel_x] = color;
			}
		}

		/* The cell is shown again, and the pen moves on. */
		x11_mark_dirty(server, window->x + x, window->y + top, (int)glyph.width, (int)glyph.height);
		x += (int)glyph.advance;
	}
}

/*
 * Adds a rectangle on the root window to what has changed.
 */
void
x11_mark_dirty(
	struct x11server *server,
	int x,
	int y,
	int width,
	int height)
{
	/* An empty rectangle changes nothing. */
	if (width <= 0 || height <= 0)
		return;

	/* The first change is the box. */
	if (!server->dirty) {
		server->dirty = 1;
		server->dirty_x0 = x;
		server->dirty_y0 = y;
		server->dirty_x1 = x + width;
		server->dirty_y1 = y + height;
		return;
	}

	/* A later one widens it. */
	if (x < server->dirty_x0)
		server->dirty_x0 = x;
	if (y < server->dirty_y0)
		server->dirty_y0 = y;
	if (x + width > server->dirty_x1)
		server->dirty_x1 = x + width;
	if (y + height > server->dirty_y1)
		server->dirty_y1 = y + height;
}

/*
 * Releases what a closed client held: its windows (with their inferiors),
 * what it asked for on other windows, its pixmaps, graphics contexts and
 * fonts, and the charge of the properties it wrote that outlive it.
 */
void
x11_resources_release(
	struct x11server *server,
	unsigned owner)
{
	struct x11_window *window;

	/* Its windows. */
	window_release_windows(server, owner);

	/* What it asked for on the windows left. */
	for (window = server->root;
	     window != NULL;
	     window = x11_window_next(window, server->root))
		x11_event_forget_client(window, owner);

	/* Its pixmaps, graphics contexts and fonts. */
	window_release_kind(server, owner, X11_RESOURCE_PIXMAP);
	window_release_kind(server, owner, X11_RESOURCE_GC);
	window_release_kind(server, owner, X11_RESOURCE_FONT);

	/* The properties it wrote on windows that stay. */
	x11_property_forget_client(server, owner);
}

/*
 * Reads the values of a window's attributes a mask names and checks each:
 * BadValue for a value out of range, BadPixmap, BadColormap, BadCursor
 * for what names nothing, BadMatch for a pixmap of another depth, and
 * BadAccess for events only one client may select, held by another (and
 * SubstructureRedirect on the root window, whose children the rootless
 * server manages itself).  Returns 0, or the error.
 */
static unsigned
window_values_read(
	struct x11server *server,
	unsigned index,
	const struct x11_window *window,
	uint32_t mask,
	const uint8_t *values,
	struct window_values *read)
{
	struct x11_pixmap *pixmap;
	uint32_t value;
	unsigned holder;
	unsigned bit;
	int msb;

	/* Each value in bit order. */
	msb = server->clients[index].order;
	memset(read, 0, sizeof(*read));
	read->mask = mask;
	for (bit = 0U; bit < ATTRIBUTE_COUNT; bit++) {
		if ((mask & (1U << bit)) == 0U)
			continue;
		value = x11_read32(values, msb);
		values += 4;
		read->value[bit] = value;

		/* Its check. */
		switch (bit) {
		case ATTRIBUTE_BACKGROUND_PIXMAP:
		case ATTRIBUTE_BORDER_PIXMAP:
			/* None, ParentRelative (CopyFromParent for the border), or a pixmap of the window's depth. */
			if (value == BACKGROUND_NONE || (bit == ATTRIBUTE_BACKGROUND_PIXMAP && value == BACKGROUND_PARENT_RELATIVE))
				break;
			pixmap = x11_pixmap_find(server, value);
			if (pixmap == NULL)
				return X11_BAD_PIXMAP;
			break;
		case ATTRIBUTE_BIT_GRAVITY:
		case ATTRIBUTE_WIN_GRAVITY:
			/* One of the gravities. */
			if (value > GRAVITY_STATIC)
				return X11_BAD_VALUE;
			break;
		case ATTRIBUTE_BACKING_STORE:
			/* NotUseful, WhenMapped or Always. */
			if (value > BACKING_STORE_MAX)
				return X11_BAD_VALUE;
			break;
		case ATTRIBUTE_OVERRIDE_REDIRECT:
		case ATTRIBUTE_SAVE_UNDER:
			/* A boolean. */
			if (value > 1U)
				return X11_BAD_VALUE;
			break;
		case ATTRIBUTE_EVENT_MASK:
			/* The core's events; those only one client may select must be free. */
			if ((value & ~X11_MASK_ALL) != 0U)
				return X11_BAD_VALUE;
			if (window == server->root && (value & X11_MASK_SUBSTRUCTURE_REDIRECT) != 0U)
				return X11_BAD_ACCESS;
			holder = x11_event_selector(window, value & X11_MASK_EXCLUSIVE & ~x11_event_mask_of(window, index));
			if (holder != X11_NO_CLIENT && holder != index)
				return X11_BAD_ACCESS;
			break;
		case ATTRIBUTE_DO_NOT_PROPAGATE:
			/* Device events only. */
			if ((value & ~X11_MASK_DEVICE) != 0U)
				return X11_BAD_VALUE;
			break;
		case ATTRIBUTE_COLORMAP:
			/* CopyFromParent or the screen's one colormap. */
			if (value != 0U && value != X11_COLORMAP_XID)
				return X11_BAD_COLORMAP;
			break;
		case ATTRIBUTE_CURSOR:
			/* None (the server has no cursor a client could name). */
			if (value != 0U)
				return X11_BAD_CURSOR;
			break;
		default:
			/* A pixel, the backing planes or pixel: any value. */
			break;
		}
	}

	/* Succeeded: every value may be kept. */
	return 0U;
}

/* Keeps the attributes read and checked.  Returns 0, or BadAlloc when the events cannot be kept. */
static unsigned
window_values_apply(
	struct x11server *server,
	unsigned index,
	struct x11_window *window,
	const struct window_values *values)
{
	unsigned code;

	/* The background and the border. */
	if ((values->mask & (1U << ATTRIBUTE_BACKGROUND_PIXMAP)) != 0U)
		window->background_pixmap = values->value[ATTRIBUTE_BACKGROUND_PIXMAP];
	if ((values->mask & (1U << ATTRIBUTE_BACKGROUND_PIXEL)) != 0U) {
		window->background = values->value[ATTRIBUTE_BACKGROUND_PIXEL];
		window->background_pixmap = BACKGROUND_NONE;
	}
	if ((values->mask & (1U << ATTRIBUTE_BORDER_PIXMAP)) != 0U)
		window->border_pixmap = values->value[ATTRIBUTE_BORDER_PIXMAP];
	if ((values->mask & (1U << ATTRIBUTE_BORDER_PIXEL)) != 0U)
		window->border_pixel = values->value[ATTRIBUTE_BORDER_PIXEL];

	/* The gravities, backing store, override-redirect and save under. */
	if ((values->mask & (1U << ATTRIBUTE_BIT_GRAVITY)) != 0U)
		window->bit_gravity = (uint8_t)values->value[ATTRIBUTE_BIT_GRAVITY];
	if ((values->mask & (1U << ATTRIBUTE_WIN_GRAVITY)) != 0U)
		window->win_gravity = (uint8_t)values->value[ATTRIBUTE_WIN_GRAVITY];
	if ((values->mask & (1U << ATTRIBUTE_BACKING_STORE)) != 0U)
		window->backing_store = (uint8_t)values->value[ATTRIBUTE_BACKING_STORE];
	if ((values->mask & (1U << ATTRIBUTE_BACKING_PLANES)) != 0U)
		window->backing_planes = values->value[ATTRIBUTE_BACKING_PLANES];
	if ((values->mask & (1U << ATTRIBUTE_BACKING_PIXEL)) != 0U)
		window->backing_pixel = values->value[ATTRIBUTE_BACKING_PIXEL];
	if ((values->mask & (1U << ATTRIBUTE_OVERRIDE_REDIRECT)) != 0U)
		window->override_redirect = (uint8_t)values->value[ATTRIBUTE_OVERRIDE_REDIRECT];
	if ((values->mask & (1U << ATTRIBUTE_SAVE_UNDER)) != 0U)
		window->save_under = (uint8_t)values->value[ATTRIBUTE_SAVE_UNDER];

	/* The events kept from propagating, the colormap and the cursor. */
	if ((values->mask & (1U << ATTRIBUTE_DO_NOT_PROPAGATE)) != 0U)
		window->do_not_propagate = values->value[ATTRIBUTE_DO_NOT_PROPAGATE];
	if ((values->mask & (1U << ATTRIBUTE_COLORMAP)) != 0U && window->window_class == X11_INPUT_OUTPUT)
		window->colormap = X11_COLORMAP_XID;
	if ((values->mask & (1U << ATTRIBUTE_CURSOR)) != 0U)
		window->cursor = values->value[ATTRIBUTE_CURSOR];

	/* The events the client asks for. */
	if ((values->mask & (1U << ATTRIBUTE_EVENT_MASK)) != 0U) {
		code = x11_event_select(server, window, index, values->value[ATTRIBUTE_EVENT_MASK]);
		if (code != 0U)
			return code;
	}

	/* Succeeded: kept. */
	return 0U;
}

/*
 * Gives an InputOutput window pixels of a size, keeping the part that
 * fits and clearing the rest to its background, charged to its owner.
 * Returns 0, or BadAlloc.
 */
static unsigned
window_pixels(
	struct x11server *server,
	struct x11_window *window,
	uint16_t width,
	uint16_t height)
{
	uint32_t *pixels;
	size_t old_bytes;
	size_t new_bytes;
	unsigned copy_width;
	unsigned copy_height;
	unsigned row;
	int error;

	/* An InputOnly window shows nothing. */
	if (window->window_class == X11_INPUT_ONLY)
		return 0U;

	/* The owner is charged for the new pixels instead of the old. */
	old_bytes = 0U;
	if (window->pixels != NULL)
		old_bytes = (size_t)window->width * (size_t)window->height * sizeof(*pixels);
	new_bytes = (size_t)width * (size_t)height * sizeof(*pixels);
	x11_memory_release(server, window->owner, old_bytes);
	error = x11_memory_charge(server, window->owner, new_bytes);
	if (error != 0) {
		(void)x11_memory_charge(server, window->owner, old_bytes);
		return X11_BAD_ALLOC;
	}

	/* The new pixels. */
	pixels = x11_pixels_alloc(width, height, window->background);
	if (pixels == NULL) {
		x11_memory_release(server, window->owner, new_bytes);
		(void)x11_memory_charge(server, window->owner, old_bytes);
		return X11_BAD_ALLOC;
	}

	/* The part of the old ones that fits, row by row. */
	copy_width = width;
	if (copy_width > window->width)
		copy_width = window->width;
	copy_height = height;
	if (copy_height > window->height)
		copy_height = window->height;
	for (row = 0U; row < copy_height && window->pixels != NULL; row++)
		memcpy(pixels + (size_t)row * width, window->pixels + (size_t)row * window->width, (size_t)copy_width * sizeof(*pixels));

	/* Succeeded: the old pixels go. */
	free(window->pixels);
	window->pixels = pixels;
	return 0U;
}

/* Puts a window among a parent's children just below one of them (NULL: on top). */
static void
window_link(
	struct x11_window *parent,
	struct x11_window *window,
	struct x11_window *under)
{
	/* Its neighbours. */
	window->parent = parent;
	window->above = under;
	window->below = parent->last_child;
	if (under != NULL)
		window->below = under->below;

	/* The windows on either side point at it. */
	if (window->below != NULL) {
		window->below->above = window;
	} else {
		parent->first_child = window;
	}
	if (window->above != NULL) {
		window->above->below = window;
	} else {
		parent->last_child = window;
	}
}

/* Takes a window out of its parent's children. */
static void
window_unlink(
	struct x11_window *window)
{
	/* The windows on either side point past it. */
	if (window->below != NULL) {
		window->below->above = window->above;
	} else {
		window->parent->first_child = window->above;
	}
	if (window->above != NULL) {
		window->above->below = window->below;
	} else {
		window->parent->last_child = window->below;
	}

	/* No neighbours. */
	window->below = NULL;
	window->above = NULL;
}

/* Returns the next window in a walk of a tree that does not go into a window's children, or NULL past the tree. */
static struct x11_window *
window_next_over(
	struct x11_window *window,
	const struct x11_window *top)
{
	/* The next sibling of the window, or of the nearest ancestor that has one, within the tree. */
	while (window != top) {
		if (window->above != NULL)
			return window->above;
		window = window->parent;
	}

	/* Past the tree. */
	return NULL;
}

/* Moves a window and its inferiors by a distance (places are on the root window). */
static void
window_move(
	struct x11_window *window,
	int dx,
	int dy)
{
	struct x11_window *moved;

	/* Nothing moves when the window does not. */
	if (dx == 0 && dy == 0)
		return;

	/* Each window of the tree. */
	for (moved = window;
	     moved != NULL;
	     moved = x11_window_next(moved, window)) {
		moved->x += dx;
		moved->y += dy;
	}
}

/* Returns a window's x relative to its parent's origin (the corner outside its border). */
static int
window_relative_x(
	const struct x11_window *window)
{
	/* The root window is at the origin. */
	if (window->parent == NULL)
		return 0;

	/* Succeeded: from the parent's origin. */
	return window->x - window->border - window->parent->x;
}

/* Returns a window's y relative to its parent's origin (the corner outside its border). */
static int
window_relative_y(
	const struct x11_window *window)
{
	/* The root window is at the origin. */
	if (window->parent == NULL)
		return 0;

	/* Succeeded: from the parent's origin. */
	return window->y - window->border - window->parent->y;
}

/*
 * Maps a window, or asks the client that redirects its parent's children
 * to: MapNotify to its and its parent's clients, and, once it is
 * viewable, Expose for it and its viewable inferiors.
 */
static void
window_map(
	struct x11server *server,
	unsigned requester,
	struct x11_window *window)
{
	struct x11_client *client;
	uint8_t event[32];
	unsigned redirect;
	int viewable;
	int iconic;

	/* Mapped already, or the root window. */
	if (window->mapped || window->parent == NULL)
		return;

	/* Another client that redirects the parent's children is asked instead. */
	redirect = x11_event_selector(window->parent, X11_MASK_SUBSTRUCTURE_REDIRECT);
	if (!window->override_redirect && redirect != X11_NO_CLIENT && redirect != requester) {
		client = x11_client_of(server, redirect);
		if (client == NULL)
			return;
		memset(event, 0, sizeof(event));
		event[0] = X11_EVENT_MAP_REQUEST;
		x11_write32(event + 4, window->parent->id, 0);
		x11_write32(event + 8, window->id, 0);
		x11_event_send(server, client, event);
		return;
	}

	/*
	 * The rootless server is the top-level windows' window manager and
	 * keeps them without a border (the outer corner stays): the clients
	 * hear the change before the map.
	 */
	if (window->parent == server->root && !window->override_redirect && window->border != 0U) {
		window_move(window, -(int)window->border, -(int)window->border);
		window->border = 0U;
		window_configure_notify(server, window);
	}

	/* The window manager keeps a top-level window that starts minimized (ICCCM's Iconic) unmapped. */
	iconic = x11_wm_map(server, window);
	if (iconic)
		return;

	/* Mapped, and told. */
	window->mapped = 1;
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_MAP_NOTIFY;
	x11_write32(event + 8, window->id, 0);
	event[12] = window->override_redirect;
	x11_event_structure(server, window, event);

	/* Succeeded: shown, and drawn by its clients when it can be seen. */
	viewable = x11_window_viewable(window);
	if (!viewable)
		return;
	x11_mark_dirty(server, window->x - window->border, window->y - window->border, window->width + 2 * window->border, window->height + 2 * window->border);
	window_expose_tree(server, window);
}

/* Unmaps a window: UnmapNotify to its and its parent's clients, and what was under it shown again. */
static void
window_unmap(
	struct x11server *server,
	struct x11_window *window,
	int from_configure)
{
	uint8_t event[32];

	/* Not mapped. */
	if (!window->mapped)
		return;

	/* Hidden, and told. */
	window->mapped = 0;
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_UNMAP_NOTIFY;
	x11_write32(event + 8, window->id, 0);
	event[12] = (uint8_t)from_configure;
	x11_event_structure(server, window, event);

	/* What was under it is shown again. */
	x11_mark_dirty(server, window->x - window->border, window->y - window->border, window->width + 2 * window->border, window->height + 2 * window->border);

	/* The focus in it reverts, and the grabs on it end. */
	x11_input_window_hidden(server, window);

	/* Succeeded: the window manager hears that a top-level window left the screen. */
	x11_wm_unmapped(server, window);
}

/* Tells the clients of a viewable window and of each of its viewable inferiors that all of it must be drawn, the window first. */
static void
window_expose_tree(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_window *exposed;
	int viewable;

	/* Not seen. */
	viewable = x11_window_viewable(window);
	if (!viewable)
		return;

	/* Each mapped window of the tree, not into an unmapped one. */
	exposed = window;
	while (exposed != NULL) {
		if (!exposed->mapped) {
			exposed = window_next_over(exposed, window);
			continue;
		}
		x11_event_expose(server, exposed);
		exposed = x11_window_next(exposed, window);
	}
}

/* Tells a window's and its parent's clients the window's configuration. */
static void
window_configure_notify(
	struct x11server *server,
	struct x11_window *window)
{
	uint8_t event[32];

	/* The window, the sibling below it, its place relative to its parent, its size, border and override-redirect. */
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_CONFIGURE_NOTIFY;
	x11_write32(event + 8, window->id, 0);
	if (window->below != NULL)
		x11_write32(event + 12, window->below->id, 0);
	x11_write16(event + 16, (uint16_t)window_relative_x(window), 0);
	x11_write16(event + 18, (uint16_t)window_relative_y(window), 0);
	x11_write16(event + 20, window->width, 0);
	x11_write16(event + 22, window->height, 0);
	x11_write16(event + 24, window->border, 0);
	event[26] = window->override_redirect;

	/* Told. */
	x11_event_structure(server, window, event);
}

/*
 * Tells the client that asked to configure a top-level window its
 * configuration as it stands, in a synthetic ConfigureNotify (sent as by
 * SendEvent, its place on the root window), as a window manager answers
 * a request it did not carry out.
 */
static void
window_configure_synthetic(
	struct x11server *server,
	struct x11_window *window,
	unsigned client)
{
	struct x11_client *asked;
	uint8_t event[32];

	/* The client, still there. */
	asked = x11_client_of(server, client);
	if (asked == NULL)
		return;

	/* The event and the window, no sibling, its place on the root window, its size, border and override-redirect. */
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_CONFIGURE_NOTIFY | X11_EVENT_SENT;
	x11_write32(event + 4, window->id, 0);
	x11_write32(event + 8, window->id, 0);
	x11_write16(event + 16, (uint16_t)(window->x - (int)window->border), 0);
	x11_write16(event + 18, (uint16_t)(window->y - (int)window->border), 0);
	x11_write16(event + 20, window->width, 0);
	x11_write16(event + 22, window->height, 0);
	x11_write16(event + 24, window->border, 0);
	event[26] = window->override_redirect;

	/* Succeeded: sent. */
	x11_event_send(server, asked, event);
}

/*
 * Reads a ConfigureWindow's values (the window's own for those not named)
 * and checks them: BadValue for an empty size or an unknown stacking mode,
 * BadWindow for a sibling that is no window, BadMatch for one that is not
 * a sibling or is named without a stacking mode.  Returns 0, or the error.
 */
static unsigned
window_configure_read(
	struct x11server *server,
	struct x11_window *window,
	uint32_t mask,
	const uint8_t *values,
	int msb,
	struct window_configure *configure)
{
	uint32_t value;
	unsigned bit;

	/* What the window has now. */
	memset(configure, 0, sizeof(*configure));
	configure->mask = mask;
	configure->x = window_relative_x(window);
	configure->y = window_relative_y(window);
	configure->width = window->width;
	configure->height = window->height;
	configure->border = window->border;
	configure->stack_mode = STACK_ABOVE;

	/* Each value in bit order. */
	for (bit = 0U; bit < CONFIGURE_COUNT; bit++) {
		if ((mask & (1U << bit)) == 0U)
			continue;
		value = x11_read32(values, msb);
		values += 4;

		/* Its meaning. */
		switch (bit) {
		case CONFIGURE_X:
			configure->x = (int16_t)value;
			break;
		case CONFIGURE_Y:
			configure->y = (int16_t)value;
			break;
		case CONFIGURE_WIDTH:
			/* A size is never empty. */
			if ((uint16_t)value == 0U)
				return X11_BAD_VALUE;
			configure->width = (uint16_t)value;
			break;
		case CONFIGURE_HEIGHT:
			/* A size is never empty. */
			if ((uint16_t)value == 0U)
				return X11_BAD_VALUE;
			configure->height = (uint16_t)value;
			break;
		case CONFIGURE_BORDER:
			configure->border = (uint16_t)value;
			break;
		case CONFIGURE_SIBLING:
			/* A window that shares the parent. */
			configure->sibling = x11_window_find(server, value);
			if (configure->sibling == NULL)
				return X11_BAD_WINDOW;
			if (configure->sibling == window || configure->sibling->parent != window->parent)
				return X11_BAD_MATCH;
			break;
		default:
			/* One of the five stacking modes. */
			if (value > STACK_OPPOSITE)
				return X11_BAD_VALUE;
			configure->stack_mode = value;
			break;
		}
	}

	/* A sibling needs a stacking mode, and an InputOnly window no border. */
	if ((mask & (1U << CONFIGURE_SIBLING)) != 0U && (mask & (1U << CONFIGURE_STACK_MODE)) == 0U)
		return X11_BAD_MATCH;
	if (window->window_class == X11_INPUT_ONLY && configure->border != 0U)
		return X11_BAD_MATCH;

	/* Succeeded: the new configuration. */
	return 0U;
}

/* Asks the client that redirects a window's parent's children for a configuration (ConfigureRequest). */
static void
window_configure_request(
	struct x11server *server,
	unsigned redirect,
	struct x11_window *window,
	const struct window_configure *configure)
{
	struct x11_client *client;
	uint8_t event[32];

	/* The redirecting client must still be there. */
	client = x11_client_of(server, redirect);
	if (client == NULL)
		return;

	/* The stacking mode, the parent, the window, the sibling, the configuration and the bits named. */
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_CONFIGURE_REQUEST;
	event[1] = (uint8_t)configure->stack_mode;
	x11_write32(event + 4, window->parent->id, 0);
	x11_write32(event + 8, window->id, 0);
	if (configure->sibling != NULL)
		x11_write32(event + 12, configure->sibling->id, 0);
	x11_write16(event + 16, (uint16_t)configure->x, 0);
	x11_write16(event + 18, (uint16_t)configure->y, 0);
	x11_write16(event + 20, configure->width, 0);
	x11_write16(event + 22, configure->height, 0);
	x11_write16(event + 24, configure->border, 0);
	x11_write16(event + 26, (uint16_t)configure->mask, 0);

	/* Succeeded: asked. */
	x11_event_send(server, client, event);
}

/*
 * Moves a resized window's children by their gravity (static: where they
 * were on the root window, before the window moved by dx and dy; unmap:
 * unmapped), telling each moved one's clients (GravityNotify).
 */
static void
window_gravity(
	struct x11server *server,
	struct x11_window *window,
	int old_width,
	int old_height,
	int dx,
	int dy)
{
	static const int across[11] = { 0, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0 };
	static const int down[11] = { 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 0 };
	struct x11_window *child;
	uint8_t event[32];
	int grow_x;
	int grow_y;
	int move_x;
	int move_y;

	/* How much the window grew each way. */
	grow_x = (int)window->width - old_width;
	grow_y = (int)window->height - old_height;

	/* Each child, by its gravity. */
	for (child = window->first_child; child != NULL; child = child->above) {
		if (child->win_gravity == GRAVITY_NORTH_WEST)
			continue;

		/* Unmap gravity: unmapped, as by the configuration. */
		if (child->win_gravity == GRAVITY_UNMAP) {
			window_unmap(server, child, 1);
			continue;
		}

		/* Static gravity keeps the child where it was on the root window; the others follow a side or the middle. */
		move_x = across[child->win_gravity] * grow_x / 2;
		move_y = down[child->win_gravity] * grow_y / 2;
		if (child->win_gravity == GRAVITY_STATIC) {
			move_x = -dx;
			move_y = -dy;
		}
		if (move_x == 0 && move_y == 0)
			continue;
		window_move(child, move_x, move_y);

		/* Its clients are told where it is now. */
		memset(event, 0, sizeof(event));
		event[0] = X11_EVENT_GRAVITY_NOTIFY;
		x11_write32(event + 8, child->id, 0);
		x11_write16(event + 12, (uint16_t)window_relative_x(child), 0);
		x11_write16(event + 14, (uint16_t)window_relative_y(child), 0);
		x11_event_structure(server, child, event);
	}
}

/* Tells whether one mapped window above another covers part of it (their borders included). */
static int
window_occludes(
	const struct x11_window *upper,
	const struct x11_window *lower)
{
	/* Both must be seen. */
	if (!upper->mapped || !lower->mapped)
		return 0;

	/* Apart side by side. */
	if (upper->x + upper->width + upper->border <= lower->x - lower->border)
		return 0;
	if (lower->x + lower->width + lower->border <= upper->x - upper->border)
		return 0;

	/* Apart one over the other. */
	if (upper->y + upper->height + upper->border <= lower->y - lower->border)
		return 0;
	if (lower->y + lower->height + lower->border <= upper->y - upper->border)
		return 0;

	/* Succeeded: they overlap. */
	return 1;
}

/* Tells whether a sibling above a window (any sibling, when NULL) covers it. */
static int
window_occluded(
	const struct x11_window *window,
	const struct x11_window *sibling)
{
	const struct x11_window *upper;
	int covers;

	/* Each sibling above the window. */
	for (upper = window->above; upper != NULL; upper = upper->above) {
		if (sibling != NULL && upper != sibling)
			continue;
		covers = window_occludes(upper, window);
		if (covers)
			return 1;
	}

	/* None covers it. */
	return 0;
}

/* Tells whether a window covers a sibling below it (any sibling, when NULL). */
static int
window_occluding(
	const struct x11_window *window,
	const struct x11_window *sibling)
{
	const struct x11_window *lower;
	int covers;

	/* Each sibling below the window. */
	for (lower = window->below; lower != NULL; lower = lower->below) {
		if (sibling != NULL && lower != sibling)
			continue;
		covers = window_occludes(window, lower);
		if (covers)
			return 1;
	}

	/* It covers none. */
	return 0;
}

/*
 * Moves a window in its siblings' stack by a stacking mode (with a
 * sibling, or among all its siblings).  Returns nonzero when it moved.
 */
static int
window_restack(
	struct x11server *server,
	struct x11_window *window,
	struct x11_window *sibling,
	unsigned mode)
{
	struct x11_window *parent;
	struct x11_window *under;
	int covered;
	int covering;

	/* Where the mode puts it: just under a window (NULL: on top), or at the bottom. */
	parent = window->parent;
	under = NULL;
	switch (mode) {
	case STACK_ABOVE:
		/* Just above the sibling, or on top. */
		if (sibling != NULL)
			under = sibling->above;
		break;
	case STACK_BELOW:
		/* Just below the sibling, or at the bottom. */
		under = parent->first_child;
		if (sibling != NULL)
			under = sibling;
		break;
	case STACK_TOP_IF:
		/* On top when the sibling (or any) covers it. */
		covered = window_occluded(window, sibling);
		if (!covered)
			return 0;
		break;
	case STACK_BOTTOM_IF:
		/* At the bottom when it covers the sibling (or any). */
		covering = window_occluding(window, sibling);
		if (!covering)
			return 0;
		under = parent->first_child;
		break;
	default:
		/* Opposite: on top when covered, else at the bottom when covering. */
		covered = window_occluded(window, sibling);
		covering = window_occluding(window, sibling);
		if (!covered && !covering)
			return 0;
		if (!covered)
			under = parent->first_child;
		break;
	}

	/* Already there. */
	if (under == window || window->above == under)
		return 0;

	/* Succeeded: moved, and its place shown again. */
	window_unlink(window);
	window_link(parent, window, under);
	x11_mark_dirty(server, window->x - window->border, window->y - window->border, window->width + 2 * window->border, window->height + 2 * window->border);
	return 1;
}

/*
 * Frees a window that has no children left: what it held, its place in
 * the tree and its id.  The focus and the grabs on it go.
 */
static void
window_free(
	struct x11server *server,
	struct x11_window *window)
{
	void *found;

	/* The focus on it reverts, and the grabs on it end. */
	x11_input_window_destroyed(server, window);

	/* The selections it owned, its properties, and its surface. */
	x11_selection_forget_window(server, window->id);
	x11_property_forget_window(server, window);
	x11_rootless_forget(server, window);
	x11_mark_dirty(server, window->x - window->border, window->y - window->border, window->width + 2 * window->border, window->height + 2 * window->border);

	/* Its pixels and its own charge. */
	if (window->pixels != NULL)
		x11_memory_release(server, window->owner, (size_t)window->width * (size_t)window->height * sizeof(uint32_t));
	x11_memory_release(server, window->owner, sizeof(*window));
	free(window->pixels);
	free(window->selects);

	/* Its place in the tree and its id. */
	if (window->parent != NULL)
		window_unlink(window);
	found = x11_resource_find(server, window->id, X11_RESOURCE_WINDOW);
	if (found == window)
		x11_resource_remove(server, window->id);

	/* Succeeded: gone. */
	free(window);
}

/*
 * Destroys a window and its inferiors, each after its own inferiors: its
 * and its parent's clients are told (DestroyNotify), then it is freed.
 */
static void
window_destroy_tree(
	struct x11server *server,
	struct x11_window *top)
{
	struct x11_window *window;
	struct x11_window *next;
	uint8_t event[32];

	/* From the first leaf, each window after its inferiors. */
	window = window_first_leaf(top);
	while (window != NULL) {
		/* The next one: the next sibling's first leaf, or the parent (none after the top). */
		next = NULL;
		if (window != top) {
			next = window->parent;
			if (window->above != NULL)
				next = window_first_leaf(window->above);
		}

		/* Told, and freed. */
		memset(event, 0, sizeof(event));
		event[0] = X11_EVENT_DESTROY_NOTIFY;
		x11_write32(event + 8, window->id, 0);
		x11_event_structure(server, window, event);
		window_free(server, window);
		window = next;
	}
}

/* Returns the first window of a tree with no children, down the bottom children. */
static struct x11_window *
window_first_leaf(
	struct x11_window *window)
{
	/* Down the first child each time. */
	while (window->first_child != NULL)
		window = window->first_child;

	/* Succeeded: the leaf. */
	return window;
}

/* Destroys every window a client created (each with its inferiors). */
static void
window_release_windows(
	struct x11server *server,
	unsigned owner)
{
	struct x11_window *window;
	uint32_t *ids;
	size_t count;
	size_t index;
	int error;

	/* The client's windows, listed, each destroyed when an earlier one did not take it with it. */
	error = x11_resource_owned(server, owner, X11_RESOURCE_WINDOW, &ids, &count);
	if (error == 0) {
		for (index = 0U; index < count; index++) {
			window = x11_window_find(server, ids[index]);
			if (window != NULL)
				x11_window_destroy(server, window);
		}
		free(ids);
		return;
	}

	/* No memory for the list: a walk of the tree for one of them at a time. */
	for (;;) {
		for (window = server->root;
		     window != NULL;
		     window = x11_window_next(window, server->root)) {
			if (window->owner == owner)
				break;
		}
		if (window == NULL)
			break;
		x11_window_destroy(server, window);
	}
}

/* Frees every pixmap, graphics context or font a client created. */
static void
window_release_kind(
	struct x11server *server,
	unsigned owner,
	unsigned type)
{
	struct x11_pixmap *pixmap;
	void *object;
	uint32_t *ids;
	size_t count;
	size_t index;
	int error;

	/* The client's resources of the kind, listed (none freed when there is no memory for the list). */
	error = x11_resource_owned(server, owner, type, &ids, &count);
	if (error != 0)
		return;

	/* Each one, its pixels too, and its charge. */
	for (index = 0U; index < count; index++) {
		object = x11_resource_find(server, ids[index], type);
		x11_resource_remove(server, ids[index]);
		if (type == X11_RESOURCE_PIXMAP) {
			pixmap = object;
			x11_memory_release(server, owner, sizeof(*pixmap) + (size_t)pixmap->width * (size_t)pixmap->height * sizeof(uint32_t));
			free(pixmap->pixels);
		} else if (type == X11_RESOURCE_GC) {
			x11_memory_release(server, owner, sizeof(struct x11_gc));
		} else {
			x11_memory_release(server, owner, sizeof(struct x11_font));
		}
		free(object);
	}

	/* Succeeded: none is left. */
	free(ids);
}
