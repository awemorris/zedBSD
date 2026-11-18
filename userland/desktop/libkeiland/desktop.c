/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop surface (ws094-p003, plan/ws094/design.md §3): the wrapper
 * of the compositor's kl_desktop_v1 protocol, which gives the program the
 * compositor started for the desktop's icons the surface over the
 * wallpaper and under every window.
 *
 * The protocol's interfaces are described here, as wayland-scanner would
 * make them, over libwayland's marshalling (the protocol is the compositor's
 * alone and has no generated code in libwayland).
 */

#include <keiland/keiland.h>

#include "desktop-private.h"

#include <wayland-client.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Marks protocol callback arguments that this client does not inspect. */
#define UNUSED_PARAMETER(name) ((void)(name))

/* The version of the protocol this library speaks, and its requests (set_drawn since version 2, ws270-p007). */
#define DESKTOP_VERSION		2U
#define DESKTOP_MANAGER_DESTROY	0U
#define DESKTOP_MANAGER_GET	1U
#define DESKTOP_SURFACE_DESTROY	0U
#define DESKTOP_SURFACE_ACK	1U
#define DESKTOP_SURFACE_DRAWN	2U

/*
 * The side of the square tiles a frame is looked at in for what it draws,
 * and the most rectangles set_drawn tells (a frame of more tells the box
 * around them all).
 */
#define DESKTOP_TILE		32U
#define DESKTOP_DRAWN_MAX	32U

/*
 * The desktop surface of a program: its kl_desktop_surface_v1 and the
 * program's listener with its data.
 */
struct kl_desktop {
	struct wl_proxy *proxy;
	const struct kl_desktop_listener *listener;
	void *data;
	/*
	 * The frames' tiles (ws270-p007): for each DESKTOP_TILE square of the
	 * last frame, whether any of its pixels is not fully transparent, for
	 * a frame of width by height (tiles_across by tiles_down of them; NULL
	 * before the first frame and after a failed allocation, which tells
	 * nothing).  The rectangles told last, so that the same are not told
	 * again; told_count is DESKTOP_DRAWN_MAX + 1 before any.
	 */
	unsigned char *tiles;
	uint32_t width;
	uint32_t height;
	uint32_t tiles_across;
	uint32_t tiles_down;
	int32_t told[DESKTOP_DRAWN_MAX][4];
	unsigned told_count;
};

/* The protocol's search: the manager's global name (0 for none) and the version it is offered at. */

struct desktop_search {
	uint32_t name;
	uint32_t version;
};

/*
 * The listener of kl_desktop_surface_v1's one event, as libwayland
 * calls it.
 */
struct desktop_proxy_listener {
	void (*configure)(void *data, struct wl_proxy *proxy, uint32_t serial, int32_t x, int32_t y, int32_t width, int32_t height);
};

static void desktop_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void desktop_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static void desktop_configure(void *data, struct wl_proxy *proxy, uint32_t serial, int32_t x, int32_t y, int32_t width, int32_t height);
static int desktop_tiles(struct kl_desktop *desktop, uint32_t width, uint32_t height);
static void desktop_scan(struct kl_desktop *desktop, const uint32_t *pixels, size_t stride, const struct kl_rect *part);
static unsigned desktop_rects(const struct kl_desktop *desktop, int32_t (*rects)[4]);
static void desktop_tell(struct kl_desktop *desktop, const int32_t (*rects)[4], unsigned count);

extern const struct wl_interface kl_desktop_manager_v1_interface;
extern const struct wl_interface kl_desktop_surface_v1_interface;

/* get_desktop_surface: the new desktop surface, the surface and the token. */
static const struct wl_interface *desktop_get_types[] = {
	&kl_desktop_surface_v1_interface,
	&wl_surface_interface,
	NULL,
};

/* The arguments of messages that name no interface (at most five). */
static const struct wl_interface *desktop_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The requests of kl_desktop_manager_v1. */
static const struct wl_message desktop_manager_requests[] = {
	{ "destroy", "", NULL },
	{ "get_desktop_surface", "nos", desktop_get_types },
};

/* kl_desktop_manager_v1. */
const struct wl_interface kl_desktop_manager_v1_interface = {
	"kl_desktop_manager_v1",
	2,
	2,
	desktop_manager_requests,
	0,
	NULL
};

/* The requests of kl_desktop_surface_v1. */
static const struct wl_message desktop_surface_requests[] = {
	{ "destroy", "", NULL },
	{ "ack_configure", "u", desktop_plain_types },
	{ "set_drawn", "2a", desktop_plain_types },
};

/* The events of kl_desktop_surface_v1. */
static const struct wl_message desktop_surface_events[] = {
	{ "configure", "uiiii", desktop_plain_types },
};

/* kl_desktop_surface_v1. */
const struct wl_interface kl_desktop_surface_v1_interface = {
	"kl_desktop_surface_v1",
	2,
	3,
	desktop_surface_requests,
	1,
	desktop_surface_events
};

/* The registry's callbacks while the manager is looked for. */
static const struct wl_registry_listener desktop_registry_listener = {
	desktop_global,
	desktop_global_remove
};

/* The desktop surface's listener. */
static const struct desktop_proxy_listener desktop_proxy_listener = {
	desktop_configure
};

static struct wl_proxy *desktop_bind(struct wl_display *display);

/*
 * Gives a surface the desktop's role with the token the compositor gave
 * the program.  The listener hears configure (where the surface is and its
 * size) before anything is drawn; the program acknowledges it with
 * kl_desktop_ack.  Returns NULL with errno set: EINVAL without a
 * token, ENOTSUP for a compositor without the desktop, ENOMEM.  A token
 * the compositor does not know ends the connection (a protocol error).
 */
struct kl_desktop *
kl_desktop_create(
	struct wl_display *display,
	struct wl_surface *surface,
	const char *token,
	const struct kl_desktop_listener *listener,
	void *data)
{
	struct kl_desktop *desktop;
	struct wl_proxy *manager;
	uint32_t version;
	int status;

	/* A token is needed. */
	if (token == NULL || token[0] == '\0') {
		errno = EINVAL;
		return NULL;
	}

	/* The compositor's manager, bound for this surface. */
	manager = desktop_bind(display);
	if (manager == NULL)
		return NULL;

	/* The record. */
	desktop = calloc(1, sizeof(*desktop));
	if (desktop == NULL) {
		wl_proxy_marshal(manager, DESKTOP_MANAGER_DESTROY);
		wl_proxy_destroy(manager);
		errno = ENOMEM;
		return NULL;
	}

	/* The program's listener and its data. */
	desktop->listener = listener;
	desktop->data = data;
	desktop->told_count = DESKTOP_DRAWN_MAX + 1U;

	/* The protocol object, at the manager's version; the binding is not needed after it. */
	version = wl_proxy_get_version(manager);
	desktop->proxy = wl_proxy_marshal_constructor_versioned(manager, DESKTOP_MANAGER_GET, &kl_desktop_surface_v1_interface, version, NULL, surface, token);
	if (desktop->proxy == NULL) {
		wl_proxy_marshal(manager, DESKTOP_MANAGER_DESTROY);
		wl_proxy_destroy(manager);
		free(desktop);
		errno = ENOMEM;
		return NULL;
	}

	/* Releases the binding once the new protocol object owns its role request. */
	wl_proxy_marshal(manager, DESKTOP_MANAGER_DESTROY);
	wl_proxy_destroy(manager);

	/* The configure comes to the record. */
	status = wl_proxy_add_listener(desktop->proxy, (void (**)(void))&desktop_proxy_listener, desktop);
	if (status != 0) {
		wl_proxy_destroy(desktop->proxy);
		free(desktop);
		errno = ENOMEM;
		return NULL;
	}

	/* Succeeded: the surface is the desktop's once the compositor agrees. */
	return desktop;
}

/*
 * Acknowledges a configure: the next commit is drawn for it.
 */
void
kl_desktop_ack(
	struct kl_desktop *desktop,
	uint32_t serial)
{
	/* The request, sent with the next flush. */
	wl_proxy_marshal(desktop->proxy, DESKTOP_SURFACE_ACK, serial);

	/* Succeeded: the configure acknowledgement is queued. */
	return;
}

/*
 * Gives the desktop's role up; the surface shows nothing more.
 */
void
kl_desktop_destroy(
	struct kl_desktop *desktop)
{
	/* No desktop, nothing to destroy. */
	if (desktop == NULL)
		return;

	/* The protocol object, then the record. */
	wl_proxy_marshal(desktop->proxy, DESKTOP_SURFACE_DESTROY);
	wl_proxy_destroy(desktop->proxy);
	free(desktop->tiles);
	free(desktop);

	/* Succeeded: the desktop role and record are released. */
	return;
}

/*
 * Tells the compositor which parts of the desktop's surface a frame draws
 * anything in (ws270-p007): the frame (premultiplied words, stride words a
 * row, width by height) is looked at in the parts that changed (count of
 * them, all of it for none), tile by tile, and the tiles not fully
 * transparent are told as rectangles with the frame's commit.  A
 * compositor of version 1 is told nothing and draws all of the surface.
 */
void
keiui_desktop_frame(
	struct kl_desktop *desktop,
	const uint32_t *pixels,
	size_t stride,
	uint32_t width,
	uint32_t height,
	const struct kl_rect *parts,
	unsigned count)
{
	int32_t rects[DESKTOP_DRAWN_MAX][4];
	struct kl_rect whole;
	unsigned rect_count;
	unsigned index;
	uint32_t version;
	int fresh;

	/* Only a compositor that takes the request. */
	if (desktop == NULL || pixels == NULL || width == 0U || height == 0U)
		return;
	version = wl_proxy_get_version(desktop->proxy);
	if (version < 2U)
		return;

	/* The tiles of a frame of this size; a new size looks at all of it. */
	fresh = desktop_tiles(desktop, width, height);
	if (fresh < 0)
		return;
	if (fresh > 0)
		count = 0U;

	/* The parts that changed, or the whole frame. */
	if (count == 0U || parts == NULL) {
		whole.x = 0;
		whole.y = 0;
		whole.width = (int)width;
		whole.height = (int)height;
		desktop_scan(desktop, pixels, stride, &whole);
	} else {
		for (index = 0U; index < count; index++)
			desktop_scan(desktop, pixels, stride, &parts[index]);
	}

	/* The tiles drawn in as rectangles, told when they changed. */
	rect_count = desktop_rects(desktop, rects);
	desktop_tell(desktop, (const int32_t (*)[4])rects, rect_count);
}

/* Binds the compositor's desktop manager through a registry of the library's own (as glass.c does). */
static struct wl_proxy *
desktop_bind(
	struct wl_display *display)
{
	struct desktop_search search;
	struct wl_event_queue *queue;
	struct wl_display *wrapper;
	struct wl_registry *registry;
	struct wl_proxy *manager;
	uint32_t version;
	int status;

	/* The search's own queue. */
	queue = wl_display_create_queue(display);
	if (queue == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* The display as the search sees it. */
	wrapper = wl_proxy_create_wrapper(display);
	if (wrapper == NULL) {
		wl_event_queue_destroy(queue);
		errno = ENOMEM;
		return NULL;
	}

	/* What the wrapper makes lives on the search's queue. */
	wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);

	/* The globals, announced to this search alone. */
	search.name = 0;
	search.version = 0;
	registry = wl_display_get_registry(wrapper);
	if (registry == NULL) {
		wl_proxy_wrapper_destroy(wrapper);
		wl_event_queue_destroy(queue);
		errno = ENOTSUP;
		return NULL;
	}

	/* Receives globals before the search begins dispatching its private queue. */
	status = wl_registry_add_listener(registry, &desktop_registry_listener, &search);
	if (status != 0) {
		wl_registry_destroy(registry);
		wl_proxy_wrapper_destroy(wrapper);
		wl_event_queue_destroy(queue);
		errno = ENOTSUP;
		return NULL;
	}

	/* A failed roundtrip cannot supply a usable manager, even after partial announcements. */
	status = wl_display_roundtrip_queue(display, queue);
	if (status < 0) {
		wl_registry_destroy(registry);
		wl_proxy_wrapper_destroy(wrapper);
		wl_event_queue_destroy(queue);
		errno = ENOTSUP;
		return NULL;
	}

	/* The manager, bound when announced, is moved to the application's default queue. */
	manager = NULL;
	if (registry != NULL && search.name != 0U) {
		version = DESKTOP_VERSION;
		if (search.version < version)
			version = search.version;
		manager = wl_registry_bind(registry, search.name, &kl_desktop_manager_v1_interface, version);
		if (manager != NULL)
			wl_proxy_set_queue(manager, NULL);
	}

	/* The search's objects go. */
	if (registry != NULL)
		wl_registry_destroy(registry);
	wl_proxy_wrapper_destroy(wrapper);
	wl_event_queue_destroy(queue);

	/* A compositor without the desktop. */
	if (manager == NULL) {
		errno = ENOTSUP;
		return NULL;
	}

	/* Succeeded: the manager. */
	return manager;
}

/* Keeps the desktop manager's global name when it is announced. */
static void
desktop_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct desktop_search *search;
	int match;

	UNUSED_PARAMETER(registry);

	/* Only the desktop manager is looked for, with the version offered. */
	search = data;
	match = strcmp(interface, "kl_desktop_manager_v1");
	if (match == 0) {
		search->name = name;
		search->version = version;
	}

	/* Succeeded: any matching manager name has been recorded. */
	return;
}

/* A global going away does not matter to the search. */
static void
desktop_global_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(registry);
	UNUSED_PARAMETER(name);

	/* Succeeded: the completed search needs no removal bookkeeping. */
	return;
}

/* Passes a configure on to the program's listener. */
static void
desktop_configure(
	void *data,
	struct wl_proxy *proxy,
	uint32_t serial,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height)
{
	struct kl_desktop *desktop;

	UNUSED_PARAMETER(proxy);

	/* The record the listener was added with. */
	desktop = data;

	/* The program's listener, when it has one. */
	if (desktop->listener != NULL && desktop->listener->configure != NULL)
		desktop->listener->configure(desktop->data, desktop, serial, x, y, width, height);

	/* Succeeded: any registered listener has received the configure. */
	return;
}

/*
 * Makes the tiles for a frame of a size, all transparent, when the size is
 * new.  Returns 1 when they were made (the whole frame is to be looked
 * at), 0 when the size is the last frame's, or -1 when there is no memory
 * for them.
 */
static int
desktop_tiles(
	struct kl_desktop *desktop,
	uint32_t width,
	uint32_t height)
{
	unsigned char *tiles;
	uint32_t across;
	uint32_t down;

	/* The same size keeps its tiles. */
	if (desktop->tiles != NULL && desktop->width == width && desktop->height == height)
		return 0;

	/* The tiles across and down, a part tile at the edges. */
	across = (width + DESKTOP_TILE - 1U) / DESKTOP_TILE;
	down = (height + DESKTOP_TILE - 1U) / DESKTOP_TILE;
	tiles = calloc((size_t)across * down, 1U);
	if (tiles == NULL)
		return -1;

	/* The new tiles replace the old. */
	free(desktop->tiles);
	desktop->tiles = tiles;
	desktop->width = width;
	desktop->height = height;
	desktop->tiles_across = across;
	desktop->tiles_down = down;

	/* Succeeded: the whole frame is to be looked at. */
	return 1;
}

/* Looks at the tiles a part of the frame meets: each is drawn in when any of its pixels has alpha. */
static void
desktop_scan(
	struct kl_desktop *desktop,
	const uint32_t *pixels,
	size_t stride,
	const struct kl_rect *part)
{
	const uint32_t *row;
	uint32_t first_x;
	uint32_t first_y;
	uint32_t last_x;
	uint32_t last_y;
	uint32_t tile_x;
	uint32_t tile_y;
	uint32_t x;
	uint32_t y;
	uint32_t right;
	uint32_t bottom;
	unsigned char drawn;

	/* The part within the frame; nothing of it, nothing to look at. */
	if (part->width <= 0 || part->height <= 0)
		return;
	if (part->x >= (int)desktop->width || part->y >= (int)desktop->height)
		return;
	if (part->x + part->width <= 0 || part->y + part->height <= 0)
		return;

	/* The tiles it meets. */
	first_x = 0U;
	first_y = 0U;
	if (part->x > 0)
		first_x = (uint32_t)part->x / DESKTOP_TILE;
	if (part->y > 0)
		first_y = (uint32_t)part->y / DESKTOP_TILE;
	last_x = ((uint32_t)(part->x + part->width) - 1U) / DESKTOP_TILE;
	last_y = ((uint32_t)(part->y + part->height) - 1U) / DESKTOP_TILE;
	if (last_x >= desktop->tiles_across)
		last_x = desktop->tiles_across - 1U;
	if (last_y >= desktop->tiles_down)
		last_y = desktop->tiles_down - 1U;

	/* Each of those tiles, looked at whole: the first pixel with alpha settles it. */
	for (tile_y = first_y; tile_y <= last_y; tile_y++) {
		for (tile_x = first_x; tile_x <= last_x; tile_x++) {
			/* The tile's pixels within the frame. */
			right = (tile_x + 1U) * DESKTOP_TILE;
			bottom = (tile_y + 1U) * DESKTOP_TILE;
			if (right > desktop->width)
				right = desktop->width;
			if (bottom > desktop->height)
				bottom = desktop->height;

			/* Row by row, until a pixel with alpha. */
			drawn = 0U;
			for (y = tile_y * DESKTOP_TILE; y < bottom && !drawn; y++) {
				row = pixels + (size_t)y * stride;
				for (x = tile_x * DESKTOP_TILE; x < right; x++) {
					if ((row[x] >> 24) == 0U)
						continue;
					drawn = 1U;
					break;
				}
			}
			desktop->tiles[(size_t)tile_y * desktop->tiles_across + tile_x] = drawn;
		}
	}
}

/*
 * Turns the tiles drawn in into rectangles (x, y, width, height in the
 * surface's pixels): each row's runs of tiles, a run going on down while
 * the rows under it have the same run.  More than DESKTOP_DRAWN_MAX of them
 * become the one box around them all.  Returns how many.
 */
static unsigned
desktop_rects(
	const struct kl_desktop *desktop,
	int32_t (*rects)[4])
{
	const unsigned char *line;
	unsigned open[DESKTOP_DRAWN_MAX];
	unsigned open_count;
	unsigned still[DESKTOP_DRAWN_MAX];
	unsigned still_count;
	unsigned count;
	unsigned index;
	unsigned rect;
	uint32_t tile_x;
	uint32_t tile_y;
	uint32_t start;
	int32_t box[4];
	int32_t left;
	int32_t width;
	int joined;

	/* Each row of tiles, top to bottom: runs joined to the run just above when it is the same. */
	count = 0U;
	open_count = 0U;
	box[0] = INT32_MAX;
	box[1] = INT32_MAX;
	box[2] = 0;
	box[3] = 0;
	for (tile_y = 0U; tile_y < desktop->tiles_down; tile_y++) {
		line = desktop->tiles + (size_t)tile_y * desktop->tiles_across;
		still_count = 0U;
		tile_x = 0U;
		while (tile_x < desktop->tiles_across) {
			/* The next run of tiles drawn in. */
			if (!line[tile_x]) {
				tile_x++;
				continue;
			}
			start = tile_x;
			while (tile_x < desktop->tiles_across && line[tile_x])
				tile_x++;
			left = (int32_t)(start * DESKTOP_TILE);
			width = (int32_t)((tile_x - start) * DESKTOP_TILE);

			/* The box around every run. */
			if (left < box[0])
				box[0] = left;
			if ((int32_t)(tile_y * DESKTOP_TILE) < box[1])
				box[1] = (int32_t)(tile_y * DESKTOP_TILE);
			if (left + width > box[2])
				box[2] = left + width;
			box[3] = (int32_t)((tile_y + 1U) * DESKTOP_TILE);

			/* The rectangle above of the same run goes on down. */
			joined = 0;
			for (index = 0U; index < open_count; index++) {
				rect = open[index];
				if (rect >= count || rects[rect][0] != left || rects[rect][2] != width)
					continue;
				rects[rect][3] += (int32_t)DESKTOP_TILE;
				if (still_count < DESKTOP_DRAWN_MAX) {
					still[still_count] = rect;
					still_count++;
				}
				joined = 1;
				break;
			}
			if (joined)
				continue;

			/* A new rectangle, while there is room for one. */
			if (count < DESKTOP_DRAWN_MAX) {
				rects[count][0] = left;
				rects[count][1] = (int32_t)(tile_y * DESKTOP_TILE);
				rects[count][2] = width;
				rects[count][3] = (int32_t)DESKTOP_TILE;
				if (still_count < DESKTOP_DRAWN_MAX) {
					still[still_count] = count;
					still_count++;
				}
			}
			count++;
		}

		/* The runs this row ended on are those the next row may go on. */
		memcpy(open, still, (size_t)still_count * sizeof(open[0]));
		open_count = still_count;
	}

	/* Too many: the box around them all. */
	if (count > DESKTOP_DRAWN_MAX) {
		rects[0][0] = box[0];
		rects[0][1] = box[1];
		rects[0][2] = box[2] - box[0];
		rects[0][3] = box[3] - box[1];
		count = 1U;
	}

	/* Succeeded: the rectangles drawn in, none for a frame wholly transparent. */
	return count;
}

/* Tells the compositor the rectangles drawn in, with the frame's commit, unless they are those told last. */
static void
desktop_tell(
	struct kl_desktop *desktop,
	const int32_t (*rects)[4],
	unsigned count)
{
	struct wl_array array;
	void *room;
	int same;

	/* The same rectangles as last time need not be told again. */
	same = 0;
	if (count == desktop->told_count) {
		same = 1;
		if (count != 0U && memcmp(desktop->told, rects, (size_t)count * sizeof(rects[0])) != 0)
			same = 0;
	}
	if (same)
		return;

	/* The rectangles as an array of x, y, width and height. */
	wl_array_init(&array);
	if (count != 0U) {
		room = wl_array_add(&array, (size_t)count * sizeof(rects[0]));
		if (room == NULL) {
			wl_array_release(&array);
			return;
		}
		memcpy(room, rects, (size_t)count * sizeof(rects[0]));
	}

	/* The request, applied with the frame's commit. */
	wl_proxy_marshal(desktop->proxy, DESKTOP_SURFACE_DRAWN, &array);
	wl_array_release(&array);

	/* Remembered, so that the same are not told again. */
	if (count != 0U)
		memcpy(desktop->told, rects, (size_t)count * sizeof(rects[0]));
	desktop->told_count = count;
}
