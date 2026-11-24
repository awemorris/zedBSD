/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tab-only backend of the X server: shows every top-level window and
 * the root window in mikuterm tabs, and keeps popups and owned dialogs
 * as overlays associated with their home tab.
 */

#include "x11-tab-only.h"

#include "userland/desktop/xserver/win32/scale.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Queued bytes threshold above which pixel frames wait for the next round. */
#define TAB_ONLY_HIGH		(1024U * 1024U)

/* Delay before a closed tab whose X window stayed open reopens in a tab (ms). */
#define TAB_ONLY_REOPEN_MS	2000U

/* Maximum pixels carried by a single X11_LINK_TAB_PIXELS frame. */
#define TAB_ONLY_PART_MAX	((X11_LINK_BODY_MAX - X11_LINK_TAB_PIXELS_FIXED) / 4U)

/* Maximum number of overlays tracked simultaneously. */
#define TAB_ONLY_OVERLAYS	64U

/* Number of pointer buttons represented in X11 state bits (1 to 3). */
#define TAB_ONLY_BUTTONS	3U

/* Background pixel outside a shaped window (opaque white) and outside root viewport. */
#define TAB_ONLY_BACKGROUND	0xFFFFFFFFU
#define TAB_ONLY_VIEW_BG	0xFF202020U

/* Root viewport zoom limits and detent capture/release thresholds. */
#define TAB_ONLY_ROOT_MIN_SCALE	0.25
#define TAB_ONLY_ROOT_MAX_SCALE	4.0
#define TAB_ONLY_DETENT_SNAP	0.06
#define TAB_ONLY_DETENT_ESCAPE	0.12
#define TAB_ONLY_DETENT_NONE	0
#define TAB_ONLY_DETENT_FIT	1
#define TAB_ONLY_DETENT_ONE	2

/*
 * One X window surface: either a tab (tab == 1) or an overlay (tab == 0)
 * anchored to its home tab.
 */
struct tab_surface {
	int tab;
	struct tab_surface *home;
	uint32_t window;
	unsigned kind;
	int32_t x;
	int32_t y;
	unsigned width;
	unsigned height;
	unsigned shown_width;
	unsigned shown_height;
	uint32_t *pixels;
	struct x11_win32_box damage;
	int damaged;
	char title[X11_LINK_TAB_TITLE_MAX + 1U];
	int opened;
	int closing;
	uint32_t closed_at;
	int entered;
	int shaped;
	struct x11_rect *shape;
	unsigned shape_count;
	int default_place;
	unsigned flags;
	struct x11_size_hints hints;
	unsigned view_width;
	unsigned view_height;
	int root_configured;
	int root_detent;
	double root_scale;
	double root_raw_scale;
	double root_fit_scale;
	double root_offset_x;
	double root_offset_y;
};

/*
 * Backend state for one X server: link callbacks, scale, tab slots,
 * overlay list in open order, frontmost tab, and input state.
 */
struct x11_tab_only {
	struct x11server *server;
	struct x11_tab_only_link link;
	unsigned scale;
	struct tab_surface *tabs[X11_LINK_TABS];
	struct tab_surface *overlays[TAB_ONLY_OVERLAYS];
	unsigned overlay_count;
	struct tab_surface *front;
	int pointer_x;
	int pointer_y;
	uint16_t buttons;
	uint8_t held[X11_KEYMAP_KEYCODES];
	uint8_t body[X11_LINK_BODY_MAX];
	uint32_t row_buf[X11_LINK_TAB_SIDE_MAX];
};

static struct x11_surface *tab_only_open(void *context, uint32_t window, const struct x11_surface_info *info);
static void tab_only_update(void *context, struct x11_surface *surface, const struct x11_surface_info *info, unsigned changed);
static void tab_only_close(void *context, struct x11_surface *surface);
static int tab_only_vulkan(void *context, struct x11_surface *surface, void *instance, void *result);
static int tab_only_present(void *context, struct x11_surface *surface, const uint32_t *pixels, size_t stride, const struct x11_rect *parts, unsigned count);
static int tab_only_clipboard_offer(void *context, unsigned selection, const uint8_t *text, size_t size);
static int tab_only_clipboard_read(void *context, unsigned selection, uint8_t **text, size_t *size);
static void tab_only_capture(void *context, struct x11_surface *surface, int on);
static int tab_only_pointer(void *context, int *x, int *y);
static int tab_only_warp(void *context, int x, int y);
static void tab_only_bell(void *context, int percent);
static void tab_only_cursor(void *context, struct x11_surface *surface, const struct x11_cursor_image *image);
static int tab_only_is_tab(const struct x11_surface_info *info);
static int tab_only_free_place(const struct x11_tab_only *tabs);
static struct tab_surface *tab_only_find(const struct x11_tab_only *tabs, uint32_t window);
static struct tab_surface *tab_only_resolve_home(const struct x11_tab_only *tabs, struct x11_surface *owner);
static void tab_only_keep(struct tab_surface *surface, const struct x11_surface_info *info);
static void tab_only_keep_shape(struct tab_surface *surface, const struct x11_surface_info *info);
static void tab_only_outside(struct tab_surface *surface, int32_t row, int32_t x0, int32_t x1, uint32_t background);
static int tab_only_shown(struct tab_surface *surface);
static void tab_only_damage(struct tab_surface *surface, int32_t x0, int32_t y0, int32_t x1, int32_t y1);
static void tab_only_damage_root(struct tab_surface *surface, int32_t rx0, int32_t ry0, int32_t rx1, int32_t ry1);
static void tab_only_damage_overlay(const struct tab_surface *overlay);
static void tab_only_composite_row(const struct x11_tab_only *tabs, const struct tab_surface *tab, int32_t row, int32_t x0, uint32_t width, uint32_t *out);
static void tab_only_set_front(struct x11_tab_only *tabs, struct tab_surface *surface);
static void tab_only_root_configure(struct x11_tab_only *tabs, struct tab_surface *surface, unsigned vw, unsigned vh);
static void tab_only_root_pan(struct tab_surface *surface, int32_t dx, int32_t dy);
static void tab_only_root_pinch(struct tab_surface *surface, int32_t cx, int32_t cy, double ratio);
static void tab_only_root_clamp(struct tab_surface *surface);
static void tab_only_send_open(struct x11_tab_only *tabs, struct tab_surface *surface);
static void tab_only_send_title(struct x11_tab_only *tabs, struct tab_surface *surface);
static int tab_only_send_pixels(struct x11_tab_only *tabs, struct tab_surface *surface);
static void tab_only_input(struct x11_tab_only *tabs, struct tab_surface *surface, const struct x11_link_frame *frame, uint32_t now);
static void tab_only_key(struct x11_tab_only *tabs, uint32_t code, int pressed, uint32_t now);
static void tab_only_text(struct x11_tab_only *tabs, const uint8_t *text, size_t size, uint32_t now);
static void tab_only_button(struct x11_tab_only *tabs, struct tab_surface *surface, uint32_t value, int32_t x, int32_t y, uint32_t now);
static void tab_only_release_keys(struct x11_tab_only *tabs, uint32_t now);
static void tab_only_forget_tab(struct x11_tab_only *tabs, struct tab_surface *surface);
static void tab_only_forget_overlay(struct x11_tab_only *tabs, struct tab_surface *surface);

/* Backend operations table for the tab-only X server. */
const struct x11_backend_ops x11_tab_only_ops = {
	tab_only_open,
	tab_only_update,
	tab_only_close,
	tab_only_vulkan,
	tab_only_present,
	tab_only_clipboard_offer,
	tab_only_clipboard_read,
	tab_only_capture,
	tab_only_pointer,
	tab_only_warp,
	tab_only_bell,
	tab_only_cursor,
	0U
};

/*
 * Creates a tab-only backend with the given link callbacks and X scale.
 * Returns 0 on success, or ENOMEM.
 */
int
x11_tab_only_create(
	const struct x11_tab_only_link *link,
	unsigned scale,
	struct x11_tab_only **result)
{
	struct x11_tab_only *tabs;

	*result = NULL;

	/* Allocates and zero-initializes the backend state. */
	tabs = calloc(1U, sizeof(*tabs));
	if (tabs == NULL)
		return ENOMEM;

	tabs->link = *link;
	tabs->scale = scale;

	*result = tabs;
	return 0;
}

/*
 * Attaches the X server core to the backend so backend events can be delivered.
 */
void
x11_tab_only_attach(
	struct x11_tab_only *tabs,
	struct x11server *server)
{
	tabs->server = server;
}

/*
 * Frees the backend and any remaining surfaces after the core has stopped.
 */
void
x11_tab_only_destroy(
	struct x11_tab_only *tabs)
{
	unsigned index;

	if (tabs == NULL)
		return;

	/* Frees any tab surfaces still present. */
	for (index = 0U; index < X11_LINK_TABS; index++) {
		if (tabs->tabs[index] == NULL)
			continue;
		free(tabs->tabs[index]->pixels);
		free(tabs->tabs[index]->shape);
		free(tabs->tabs[index]);
	}

	/* Frees any overlay surfaces still present. */
	for (index = 0U; index < tabs->overlay_count; index++) {
		free(tabs->overlays[index]->pixels);
		free(tabs->overlays[index]->shape);
		free(tabs->overlays[index]);
	}

	free(tabs);
}

/*
 * Handles one tab frame received from mikuterm (input, configure, close,
 * or refuse).  Returns 0, or EPROTO for a malformed input frame.
 */
int
x11_tab_only_take(
	struct x11_tab_only *tabs,
	const struct x11_link_frame *frame,
	uint32_t now)
{
	struct tab_surface *surface;
	uint32_t width;
	uint32_t height;
	unsigned kind;

	/* Validates fixed-size input frames before looking up the surface. */
	if (frame->type == X11_LINK_TAB_INPUT) {
		if (frame->size < 4U)
			return EPROTO;
		kind = x11_link_get32(frame->body);
		if (kind != X11_LINK_INPUT_TEXT && frame->size != X11_LINK_TAB_INPUT_BODY)
			return EPROTO;
	}

	/* Drops frames for windows that already closed. */
	surface = tab_only_find(tabs, frame->id);
	if (surface == NULL || tabs->server == NULL)
		return 0;

	/* Dispatches according to frame type. */
	switch (frame->type) {
	case X11_LINK_TAB_INPUT:
		if (!surface->closing)
			tab_only_input(tabs, surface, frame, now);
		break;
	case X11_LINK_TAB_CONFIGURE:
		if (frame->size < X11_LINK_TAB_SIZE_BODY)
			return EPROTO;
		width = x11_link_get32(frame->body);
		height = x11_link_get32(frame->body + 4);
		if (surface->closing)
			break;
		if (width == 0U || height == 0U || width > X11_LINK_TAB_SIDE_MAX || height > X11_LINK_TAB_SIDE_MAX)
			break;
		if (surface->kind == X11_SURFACE_ROOT) {
			tab_only_root_configure(tabs, surface, width, height);
			break;
		}

		x11server_backend_configure(tabs->server, surface->window, 0, 0, (int)width, (int)height);
		break;
	case X11_LINK_TAB_CLOSE:
		surface->closing = 1;
		surface->closed_at = now;
		surface->opened = 0;
		x11server_backend_close(tabs->server, surface->window);
		break;
	case X11_LINK_TAB_REFUSE:
		x11server_backend_close(tabs->server, surface->window);
		break;
	default:
		break;
	}

	return 0;
}

/*
 * Sends pending tab open frames and damaged pixel rectangles to mikuterm
 * while the link queue has room.
 */
void
x11_tab_only_send(
	struct x11_tab_only *tabs,
	uint32_t now)
{
	struct tab_surface *surface;
	unsigned place;
	size_t waiting;
	int error;

	for (place = 0U; place < X11_LINK_TABS; place++) {
		surface = tabs->tabs[place];
		if (surface == NULL)
			continue;

		/* Reopens a tab if the X window survived past the close grace period. */
		if (surface->closing && now - surface->closed_at > TAB_ONLY_REOPEN_MS)
			surface->closing = 0;
		if (surface->closing)
			continue;

		if (!surface->opened)
			tab_only_send_open(tabs, surface);

		/* Sends damaged pixel rows until clean or the link queue hits the high water mark. */
		while (surface->damaged) {
			waiting = tabs->link.waiting(tabs->link.context);
			if (waiting >= TAB_ONLY_HIGH)
				return;
			error = tab_only_send_pixels(tabs, surface);
			if (error != 0)
				return;
		}
	}
}

/* Opens a tab or overlay surface for an X window. */
static struct x11_surface *
tab_only_open(
	void *context,
	uint32_t window,
	const struct x11_surface_info *info)
{
	struct x11_tab_only *tabs;
	struct tab_surface *surface;
	int place;
	int is_tab;
	int error;

	tabs = context;
	surface = calloc(1U, sizeof(*surface));
	if (surface == NULL)
		return NULL;

	surface->window = window;
	surface->kind = info->kind;
	tab_only_keep(surface, info);

	/* Opens top-level ownerless windows and the root window as tabs. */
	is_tab = tab_only_is_tab(info);
	if (is_tab) {
		place = tab_only_free_place(tabs);
		if (place < 0) {
			free(surface);
			return NULL;
		}

		surface->tab = 1;
		if (info->kind == X11_SURFACE_TOPLEVEL && (info->x != 0 || info->y != 0)) {
			surface->x = 0;
			surface->y = 0;
			if (tabs->server != NULL)
				x11server_backend_configure(tabs->server, window, 0, 0, (int)info->width, (int)info->height);
		}

		tab_only_keep_shape(surface, info);
		error = tab_only_shown(surface);
		if (error != 0) {
			free(surface->shape);
			free(surface);
			return NULL;
		}

		tabs->tabs[place] = surface;
		if (tabs->front == NULL)
			tabs->front = surface;
		return (struct x11_surface *)surface;
	}

	/* Opens popups and owned dialogs as overlays anchored to their home tab. */
	if (tabs->overlay_count >= TAB_ONLY_OVERLAYS) {
		free(surface);
		return NULL;
	}

	surface->tab = 0;
	surface->home = tab_only_resolve_home(tabs, info->owner);
	if (info->default_place && surface->home != NULL) {
		surface->x = (surface->home->shown_width > surface->width) ?
			(int32_t)((surface->home->shown_width - surface->width) / 2U) : 0;
		surface->y = (surface->home->shown_height > surface->height) ?
			(int32_t)((surface->home->shown_height - surface->height) / 2U) : 0;
		if (tabs->server != NULL)
			x11server_backend_configure(tabs->server, window, surface->x, surface->y, (int)surface->width, (int)surface->height);
	}

	tab_only_keep_shape(surface, info);
	error = tab_only_shown(surface);
	if (error != 0) {
		free(surface->shape);
		free(surface);
		return NULL;
	}

	tabs->overlays[tabs->overlay_count] = surface;
	tabs->overlay_count += 1U;
	tab_only_damage_overlay(surface);
	return (struct x11_surface *)surface;
}

/* Updates surface metadata, shape, title, or size. */
static void
tab_only_update(
	void *context,
	struct x11_surface *surface,
	const struct x11_surface_info *info,
	unsigned changed)
{
	struct x11_tab_only *tabs;
	struct tab_surface *shown;
	uint8_t size[X11_LINK_TAB_SIZE_BODY];
	int32_t row;
	int error;

	tabs = context;
	shown = (struct tab_surface *)surface;

	if (!shown->tab) {
		tab_only_damage_overlay(shown);
		tab_only_keep(shown, info);
		if ((changed & X11_SURFACE_CHANGED_OWNER) != 0U)
			shown->home = tab_only_resolve_home(tabs, info->owner);
		if ((changed & X11_SURFACE_CHANGED_SHAPE) != 0U)
			tab_only_keep_shape(shown, info);
		if ((changed & X11_SURFACE_CHANGED_SIZE) != 0U)
			(void)tab_only_shown(shown);
		tab_only_damage_overlay(shown);
		return;
	}

	tab_only_keep(shown, info);

	/* Rebuilds outside-shape background if the window shape changed. */
	if ((changed & X11_SURFACE_CHANGED_SHAPE) != 0U) {
		tab_only_keep_shape(shown, info);
		for (row = 0; row < (int32_t)shown->shown_height; row++)
			tab_only_outside(shown, row, 0, (int32_t)shown->shown_width, TAB_ONLY_BACKGROUND);
		tab_only_damage(shown, 0, 0, (int32_t)shown->shown_width, (int32_t)shown->shown_height);
	}

	if ((changed & X11_SURFACE_CHANGED_TITLE) != 0U && shown->opened && !shown->closing)
		tab_only_send_title(tabs, shown);

	if ((changed & X11_SURFACE_CHANGED_SIZE) != 0U) {
		error = tab_only_shown(shown);
		if (error != 0)
			return;
		if (shown->opened && !shown->closing) {
			x11_link_put32(size, shown->shown_width);
			x11_link_put32(size + 4, shown->shown_height);
			(void)tabs->link.frame(tabs->link.context, X11_LINK_TAB_SIZE, shown->window, size, sizeof(size));
		}
	}
}

/* Closes a tab or overlay surface and releases its resources. */
static void
tab_only_close(
	void *context,
	struct x11_surface *surface)
{
	struct x11_tab_only *tabs;
	struct tab_surface *shown;

	tabs = context;
	shown = (struct tab_surface *)surface;

	if (shown->tab) {
		if (shown->opened)
			(void)tabs->link.frame(tabs->link.context, X11_LINK_TAB_GONE, shown->window, NULL, 0U);
		tab_only_forget_tab(tabs, shown);
	} else {
		tab_only_damage_overlay(shown);
		tab_only_forget_overlay(tabs, shown);
	}

	free(shown->pixels);
	free(shown->shape);
	free(shown);
}

/* Vulkan swapchains are not supported; pixels are always presented on CPU. */
static int
tab_only_vulkan(
	void *context,
	struct x11_surface *surface,
	void *instance,
	void *result)
{
	(void)context;
	(void)surface;
	(void)instance;
	(void)result;
	return ENOTSUP;
}

/* Copies presented pixels into the surface buffer and records damage on tabs. */
static int
tab_only_present(
	void *context,
	struct x11_surface *surface,
	const uint32_t *pixels,
	size_t stride,
	const struct x11_rect *parts,
	unsigned count)
{
	struct tab_surface *shown;
	const struct x11_rect *part;
	unsigned index;
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
	int32_t row;

	(void)context;
	shown = (struct tab_surface *)surface;

	for (index = 0U; index < count; index++) {
		part = &parts[index];
		x0 = part->x;
		y0 = part->y;
		x1 = part->x + (int32_t)part->width;
		y1 = part->y + (int32_t)part->height;
		if (x0 < 0)
			x0 = 0;
		if (y0 < 0)
			y0 = 0;
		if (x1 > (int32_t)shown->shown_width)
			x1 = (int32_t)shown->shown_width;
		if (y1 > (int32_t)shown->shown_height)
			y1 = (int32_t)shown->shown_height;
		if (x0 >= x1 || y0 >= y1)
			continue;

		for (row = y0; row < y1; row++) {
			memcpy(shown->pixels + (size_t)row * shown->shown_width + (size_t)x0,
			       pixels + (size_t)row * stride + (size_t)x0,
			       (size_t)(x1 - x0) * sizeof(*pixels));
			if (shown->tab && shown->shaped)
				tab_only_outside(shown, row, x0, x1, TAB_ONLY_BACKGROUND);
		}

		if (shown->tab)
			tab_only_damage_root(shown, x0, y0, x1, y1);
		else if (shown->home != NULL) {
			int32_t hx0 = shown->x + x0;
			int32_t hy0 = shown->y + y0;
			int32_t hx1 = shown->x + x1;
			int32_t hy1 = shown->y + y1;
			if (hx0 < 0)
				hx0 = 0;
			if (hy0 < 0)
				hy0 = 0;
			if (hx1 > (int32_t)shown->home->shown_width)
				hx1 = (int32_t)shown->home->shown_width;
			if (hy1 > (int32_t)shown->home->shown_height)
				hy1 = (int32_t)shown->home->shown_height;
			if (hx0 < hx1 && hy0 < hy1)
				tab_only_damage_root(shown->home, hx0, hy0, hx1, hy1);
		}
	}

	return 0;
}

/* System clipboard bridging is not used by the tab-only backend. */
static int
tab_only_clipboard_offer(
	void *context,
	unsigned selection,
	const uint8_t *text,
	size_t size)
{
	(void)context;
	(void)selection;
	(void)text;
	(void)size;
	return ENOTSUP;
}

/* System clipboard bridging is not used by the tab-only backend. */
static int
tab_only_clipboard_read(
	void *context,
	unsigned selection,
	uint8_t **text,
	size_t *size)
{
	(void)context;
	(void)selection;
	(void)text;
	(void)size;
	return ENOTSUP;
}

/* Pointer capture is a no-op because all pointer input arrives via tabs. */
static void
tab_only_capture(
	void *context,
	struct x11_surface *surface,
	int on)
{
	(void)context;
	(void)surface;
	(void)on;
}

/* Returns the last pointer coordinates received on any tab. */
static int
tab_only_pointer(
	void *context,
	int *x,
	int *y)
{
	struct x11_tab_only *tabs;

	tabs = context;
	*x = tabs->pointer_x;
	*y = tabs->pointer_y;
	return 0;
}

/* Warping the system pointer is not supported. */
static int
tab_only_warp(
	void *context,
	int x,
	int y)
{
	(void)context;
	(void)x;
	(void)y;
	return ENOTSUP;
}

/* Bell notifications are ignored by the tab-only backend. */
static void
tab_only_bell(
	void *context,
	int percent)
{
	(void)context;
	(void)percent;
}

/* Cursor images are ignored because tabs use the host pointer or touch. */
static void
tab_only_cursor(
	void *context,
	struct x11_surface *surface,
	const struct x11_cursor_image *image)
{
	(void)context;
	(void)surface;
	(void)image;
}

/* Returns 1 if the surface should be shown as a tab, 0 if as an overlay. */
static int
tab_only_is_tab(
	const struct x11_surface_info *info)
{
	if (info->kind == X11_SURFACE_ROOT)
		return 1;
	if (info->kind == X11_SURFACE_TOPLEVEL && info->owner == NULL)
		return 1;
	return 0;
}

/* Returns the first free tab slot, or -1 if all X11_LINK_TABS slots are full. */
static int
tab_only_free_place(
	const struct x11_tab_only *tabs)
{
	unsigned place;

	for (place = 0U; place < X11_LINK_TABS; place++) {
		if (tabs->tabs[place] == NULL)
			return (int)place;
	}

	return -1;
}

/* Finds the tab surface with the given X window id, or NULL. */
static struct tab_surface *
tab_only_find(
	const struct x11_tab_only *tabs,
	uint32_t window)
{
	unsigned place;

	for (place = 0U; place < X11_LINK_TABS; place++) {
		if (tabs->tabs[place] != NULL && tabs->tabs[place]->window == window)
			return tabs->tabs[place];
	}

	return NULL;
}

/* Resolves the home tab for an overlay from its owner or the current front tab. */
static struct tab_surface *
tab_only_resolve_home(
	const struct x11_tab_only *tabs,
	struct x11_surface *owner)
{
	struct tab_surface *parent;

	if (owner != NULL) {
		parent = (struct tab_surface *)owner;
		if (parent->tab)
			return parent;
		if (parent->home != NULL)
			return parent->home;
	}

	return tabs->front;
}

/* Copies surface geometry, title, flags, and hints from info. */
static void
tab_only_keep(
	struct tab_surface *surface,
	const struct x11_surface_info *info)
{
	size_t length;

	surface->x = info->x;
	surface->y = info->y;
	surface->width = info->width;
	surface->height = info->height;

	/* Truncates title to X11_LINK_TAB_TITLE_MAX without splitting UTF-8 sequences. */
	length = 0U;
	if (info->title != NULL)
		length = strlen(info->title);
	if (length > X11_LINK_TAB_TITLE_MAX) {
		length = X11_LINK_TAB_TITLE_MAX;
		while (length > 0U && ((unsigned char)info->title[length] & 0xC0U) == 0x80U)
			length--;
	}

	if (length != 0U)
		memcpy(surface->title, info->title, length);
	surface->title[length] = '\0';

	surface->flags = info->flags;
	surface->hints = info->hints;
	surface->default_place = info->default_place;
}

/* Copies the surface's shape rectangles, or clears shape when unshaped. */
static void
tab_only_keep_shape(
	struct tab_surface *surface,
	const struct x11_surface_info *info)
{
	struct x11_rect *copy;

	free(surface->shape);
	surface->shape = NULL;
	surface->shape_count = 0U;
	surface->shaped = 0;

	if (!info->shaped)
		return;

	copy = malloc(((size_t)info->shape_count + 1U) * sizeof(*copy));
	if (copy == NULL)
		return;
	if (info->shape_count != 0U)
		memcpy(copy, info->shape, (size_t)info->shape_count * sizeof(*copy));
	surface->shape = copy;
	surface->shape_count = info->shape_count;
	surface->shaped = 1;
}

/* Paints background into pixels of row [x0, x1) that lie outside the window shape. */
static void
tab_only_outside(
	struct tab_surface *surface,
	int32_t row,
	int32_t x0,
	int32_t x1,
	uint32_t background)
{
	const struct x11_rect *rect;
	uint32_t *line;
	int32_t x;
	int32_t next;
	unsigned index;
	int covered;

	line = surface->pixels + (size_t)row * surface->shown_width;
	x = x0;
	while (x < x1) {
		covered = 0;
		next = x1;
		for (index = 0U; index < surface->shape_count && !covered; index++) {
			rect = &surface->shape[index];
			if (row < rect->y || row >= rect->y + (int32_t)rect->height) {
				/* Outside this rectangle vertically. */
			} else if (x >= rect->x && x < rect->x + (int32_t)rect->width) {
				covered = 1;
				next = rect->x + (int32_t)rect->width;
			} else if (rect->x > x && rect->x < next) {
				next = rect->x;
			}
		}

		if (!covered) {
			while (x < next) {
				line[x] = background;
				x++;
			}
		}

		x = next;
	}
}

/* Allocates or resizes the surface pixel buffer, preserving overlapping pixels. */
static int
tab_only_shown(
	struct tab_surface *surface)
{
	uint32_t *pixels;
	unsigned width;
	unsigned height;
	unsigned rows;
	unsigned columns;
	unsigned row;

	width = surface->width;
	if (width == 0U)
		width = 1U;
	if (width > X11_LINK_TAB_SIDE_MAX)
		width = X11_LINK_TAB_SIDE_MAX;
	height = surface->height;
	if (height == 0U)
		height = 1U;
	if (height > X11_LINK_TAB_SIDE_MAX)
		height = X11_LINK_TAB_SIDE_MAX;
	if (height > X11_LINK_TAB_AREA_MAX / width)
		height = X11_LINK_TAB_AREA_MAX / width;

	if (surface->pixels != NULL && width == surface->shown_width && height == surface->shown_height)
		return 0;

	pixels = calloc((size_t)width * (size_t)height, sizeof(*pixels));
	if (pixels == NULL)
		return ENOMEM;

	rows = surface->shown_height;
	if (rows > height)
		rows = height;
	columns = surface->shown_width;
	if (columns > width)
		columns = width;
	for (row = 0U; row < rows; row++)
		memcpy(pixels + (size_t)row * width, surface->pixels + (size_t)row * surface->shown_width, (size_t)columns * sizeof(*pixels));

	free(surface->pixels);
	surface->pixels = pixels;
	surface->shown_width = width;
	surface->shown_height = height;
	surface->damaged = 0;
	if (surface->tab)
		tab_only_damage(surface, 0, 0, (int32_t)width, (int32_t)height);
	return 0;
}

/* Expands the pending damage bounding box on a tab surface. */
static void
tab_only_damage(
	struct tab_surface *surface,
	int32_t x0,
	int32_t y0,
	int32_t x1,
	int32_t y1)
{
	if (!surface->damaged) {
		surface->damage.x0 = x0;
		surface->damage.y0 = y0;
		surface->damage.x1 = x1;
		surface->damage.y1 = y1;
		surface->damaged = 1;
		return;
	}

	if (x0 < surface->damage.x0)
		surface->damage.x0 = x0;
	if (y0 < surface->damage.y0)
		surface->damage.y0 = y0;
	if (x1 > surface->damage.x1)
		surface->damage.x1 = x1;
	if (y1 > surface->damage.y1)
		surface->damage.y1 = y1;
}

/* Sends X11_LINK_TAB_OPEN for a newly opened tab and marks all pixels damaged. */
static void
tab_only_send_open(
	struct x11_tab_only *tabs,
	struct tab_surface *surface)
{
	size_t length;
	int error;

	length = strlen(surface->title);
	x11_link_put32(tabs->body, (surface->kind == X11_SURFACE_ROOT && surface->root_configured) ? surface->view_width : surface->shown_width);
	x11_link_put32(tabs->body + 4, (surface->kind == X11_SURFACE_ROOT && surface->root_configured) ? surface->view_height : surface->shown_height);
	x11_link_put32(tabs->body + 8, (surface->kind == X11_SURFACE_ROOT) ? 100U : tabs->scale);
	memcpy(tabs->body + X11_LINK_TAB_OPEN_FIXED, surface->title, length);
	error = tabs->link.frame(tabs->link.context, X11_LINK_TAB_OPEN, surface->window, tabs->body, (uint32_t)(X11_LINK_TAB_OPEN_FIXED + length));
	if (error != 0)
		return;

	surface->opened = 1;
	surface->entered = 0;
	tab_only_damage(surface, 0, 0, (int32_t)surface->shown_width, (int32_t)surface->shown_height);
}

/* Sends an updated tab title to mikuterm. */
static void
tab_only_send_title(
	struct x11_tab_only *tabs,
	struct tab_surface *surface)
{
	size_t length;

	length = strlen(surface->title);
	(void)tabs->link.frame(tabs->link.context, X11_LINK_TAB_TITLE, surface->window, surface->title, (uint32_t)length);
}

/* Sends one chunk of damaged pixel rows for surface and advances damage.y0. */
static int
tab_only_send_pixels(
	struct x11_tab_only *tabs,
	struct tab_surface *surface)
{
	uint8_t *to;
	uint32_t width;
	uint32_t rows;
	uint32_t row;
	uint32_t column;
	int error;

	width = (uint32_t)(surface->damage.x1 - surface->damage.x0);
	rows = TAB_ONLY_PART_MAX / width;
	if (rows > (uint32_t)(surface->damage.y1 - surface->damage.y0))
		rows = (uint32_t)(surface->damage.y1 - surface->damage.y0);

	x11_link_put32(tabs->body, (uint32_t)surface->damage.x0);
	x11_link_put32(tabs->body + 4, (uint32_t)surface->damage.y0);
	x11_link_put32(tabs->body + 8, width);
	x11_link_put32(tabs->body + 12, rows);
	to = tabs->body + X11_LINK_TAB_PIXELS_FIXED;
	if (surface->kind == X11_SURFACE_ROOT && surface->root_configured) {
		for (row = 0U; row < rows; row++) {
			int32_t sy = surface->damage.y0 + (int32_t)row;
			int32_t ry = (int32_t)floor(surface->root_offset_y + ((double)sy + 0.5) / surface->root_scale);
			if (ry < 0 || ry >= (int32_t)surface->shown_height) {
				for (column = 0U; column < width; column++) {
					x11_link_put32(to, TAB_ONLY_VIEW_BG);
					to += 4;
				}
			} else {
				tab_only_composite_row(tabs, surface, ry, 0, surface->shown_width, tabs->row_buf);
				for (column = 0U; column < width; column++) {
					int32_t sx = surface->damage.x0 + (int32_t)column;
					int32_t rx = (int32_t)floor(surface->root_offset_x + ((double)sx + 0.5) / surface->root_scale);
					uint32_t px = (rx >= 0 && rx < (int32_t)surface->shown_width) ? tabs->row_buf[rx] : TAB_ONLY_VIEW_BG;
					x11_link_put32(to, px);
					to += 4;
				}
			}
		}
	} else {
		for (row = 0U; row < rows; row++) {
			tab_only_composite_row(tabs, surface, surface->damage.y0 + (int32_t)row, surface->damage.x0, width, tabs->row_buf);
			for (column = 0U; column < width; column++) {
				x11_link_put32(to, tabs->row_buf[column]);
				to += 4;
			}
		}
	}

	error = tabs->link.frame(tabs->link.context, X11_LINK_TAB_PIXELS, surface->window, tabs->body, X11_LINK_TAB_PIXELS_FIXED + rows * width * 4U);
	if (error != 0)
		return error;

	surface->damage.y0 += (int32_t)rows;
	if (surface->damage.y0 >= surface->damage.y1)
		surface->damaged = 0;
	return 0;
}

/* Dispatches an X11_LINK_TAB_INPUT frame to the X server core. */
static void
tab_only_input(
	struct x11_tab_only *tabs,
	struct tab_surface *surface,
	const struct x11_link_frame *frame,
	uint32_t now)
{
	unsigned kind;
	uint32_t first;
	int32_t x;
	int32_t y;

	kind = x11_link_get32(frame->body);
	first = 0U;
	x = 0;
	y = 0;
	if (kind != X11_LINK_INPUT_TEXT) {
		first = x11_link_get32(frame->body + 4);
		x = (int32_t)x11_link_get32(frame->body + 8);
		y = (int32_t)x11_link_get32(frame->body + 12);
	}

	switch (kind) {
	case X11_LINK_INPUT_KEY:
		tab_only_key(tabs, first, x != 0, now);
		break;
	case X11_LINK_INPUT_TEXT:
		tab_only_text(tabs, frame->body + 4, frame->size - 4U, now);
		break;
	case X11_LINK_INPUT_POINTER:
		tab_only_button(tabs, surface, first, x, y, now);
		break;
	case X11_LINK_INPUT_FOCUS:
		if (first != 0U) {
			tab_only_set_front(tabs, surface);
			x11server_backend_enter(tabs->server, surface->window, 1);
		} else {
			tab_only_release_keys(tabs, now);
			x11server_backend_leave(tabs->server, surface->window, 1);
		}

		break;
	case X11_LINK_INPUT_LEAVE:
		if (surface->entered) {
			surface->entered = 0;
			x11server_backend_leave(tabs->server, surface->window, 0);
		}

		break;
	case X11_LINK_INPUT_PAN:
		if (surface->kind == X11_SURFACE_ROOT && surface->root_configured)
			tab_only_root_pan(surface, x, y);
		break;
	case X11_LINK_INPUT_PINCH:
		if (surface->kind == X11_SURFACE_ROOT && surface->root_configured && first != 0U)
			tab_only_root_pinch(surface, x, y, (double)first / 65536.0);
		break;
	default:
		break;
	}
}

/* Translates an evdev key event into X11 key press/release with repeat. */
static void
tab_only_key(
	struct x11_tab_only *tabs,
	uint32_t code,
	int pressed,
	uint32_t now)
{
	uint8_t keycode;

	if (code == 0U || code + X11_KEYMAP_EVDEV_OFFSET >= X11_KEYMAP_KEYCODES)
		return;
	keycode = (uint8_t)(code + X11_KEYMAP_EVDEV_OFFSET);

	if (!pressed && !tabs->held[keycode])
		return;

	if (pressed && tabs->held[keycode])
		x11server_backend_key(tabs->server, keycode, 0, now);

	tabs->held[keycode] = (uint8_t)pressed;
	x11server_backend_key(tabs->server, keycode, pressed, now);
}

/* Decodes UTF-8 committed text into code points for x11server_backend_text. */
static void
tab_only_text(
	struct x11_tab_only *tabs,
	const uint8_t *text,
	size_t size,
	uint32_t now)
{
	size_t offset;
	size_t length;
	size_t index;
	uint32_t character;
	uint8_t first;

	offset = 0U;
	while (offset < size) {
		first = text[offset];
		length = 0U;
		character = 0U;
		if (first < 0x80U) {
			length = 1U;
			character = first;
		} else if (first >= 0xC2U && first <= 0xDFU) {
			length = 2U;
			character = first & 0x1FU;
		} else if (first >= 0xE0U && first <= 0xEFU) {
			length = 3U;
			character = first & 0x0FU;
		} else if (first >= 0xF0U && first <= 0xF4U) {
			length = 4U;
			character = first & 0x07U;
		}

		if (length == 0U || length > size - offset) {
			offset++;
			continue;
		}

		for (index = 1U; index < length; index++) {
			if ((text[offset + index] & 0xC0U) != 0x80U)
				break;
			character = (character << 6) | (text[offset + index] & 0x3FU);
		}

		if (index != length) {
			offset++;
			continue;
		}

		offset += length;
		if (character >= 0x20U && character != 0x7FU)
			x11server_backend_text(tabs->server, character, now);
	}
}

/* Sends pointer motion or button events for a tab at (0, 0) on the root window. */
static void
tab_only_button(
	struct x11_tab_only *tabs,
	struct tab_surface *surface,
	uint32_t value,
	int32_t x,
	int32_t y,
	uint32_t now)
{
	struct x11_pointer_frame frame;
	unsigned button;
	int pressed;
	uint16_t bit;

	button = value & 0xFFU;
	pressed = 0;
	if ((value & X11_LINK_INPUT_PRESSED) != 0U)
		pressed = 1;
	if (button > 9U)
		return;
	if (x < -(int32_t)X11_LINK_TAB_SIDE_MAX || x > (int32_t)X11_LINK_TAB_SIDE_MAX)
		return;
	if (y < -(int32_t)X11_LINK_TAB_SIDE_MAX || y > (int32_t)X11_LINK_TAB_SIDE_MAX)
		return;

	if (surface->kind == X11_SURFACE_ROOT && surface->root_configured) {
		x = (int32_t)floor(surface->root_offset_x + (double)x / surface->root_scale);
		y = (int32_t)floor(surface->root_offset_y + (double)y / surface->root_scale);
	}

	if (!surface->entered || (button != 0U && pressed)) {
		tab_only_set_front(tabs, surface);
		surface->entered = 1;
	}

	tabs->pointer_x = (int)x;
	tabs->pointer_y = (int)y;

	bit = 0U;
	if (button >= 1U && button <= TAB_ONLY_BUTTONS)
		bit = (uint16_t)(1U << (button - 1U));

	memset(&frame, 0, sizeof(frame));
	frame.x = (int)x;
	frame.y = (int)y;
	frame.time = now;
	frame.buttons_before = tabs->buttons;
	frame.button = (uint8_t)button;
	frame.pressed = pressed;
	if (button != 0U && pressed) {
		tabs->buttons |= bit;
	} else if (button != 0U) {
		tabs->buttons &= (uint16_t)~bit;
	}

	frame.buttons_after = tabs->buttons;

	x11server_backend_pointer(tabs->server, &frame);
}

/* Releases every key currently marked as held when keyboard focus is lost. */
static void
tab_only_release_keys(
	struct x11_tab_only *tabs,
	uint32_t now)
{
	unsigned keycode;

	for (keycode = 0U; keycode < X11_KEYMAP_KEYCODES; keycode++) {
		if (!tabs->held[keycode])
			continue;
		tabs->held[keycode] = 0;
		x11server_backend_key(tabs->server, (uint8_t)keycode, 0, now);
	}
}

/* Removes a tab from tabs->tabs and updates tabs->front if needed. */
static void
tab_only_forget_tab(
	struct x11_tab_only *tabs,
	struct tab_surface *surface)
{
	unsigned place;

	for (place = 0U; place < X11_LINK_TABS; place++) {
		if (tabs->tabs[place] == surface)
			tabs->tabs[place] = NULL;
	}

	if (tabs->front == surface) {
		tabs->front = NULL;
		for (place = 0U; place < X11_LINK_TABS; place++) {
			if (tabs->tabs[place] != NULL) {
				tabs->front = tabs->tabs[place];
				break;
			}
		}
	}

	/* Reassigns overlays owned by the closed tab to the new front tab. */
	for (place = 0U; place < tabs->overlay_count; place++) {
		if (tabs->overlays[place]->home == surface) {
			tabs->overlays[place]->home = tabs->front;
			tab_only_damage_overlay(tabs->overlays[place]);
		}
	}
}

/* Marks the overlay's bounding box as damaged on its current home tab. */
static void
tab_only_damage_overlay(
	const struct tab_surface *overlay)
{
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;

	if (overlay->home == NULL)
		return;

	x0 = overlay->x;
	y0 = overlay->y;
	x1 = overlay->x + (int32_t)overlay->shown_width;
	y1 = overlay->y + (int32_t)overlay->shown_height;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int32_t)overlay->home->shown_width)
		x1 = (int32_t)overlay->home->shown_width;
	if (y1 > (int32_t)overlay->home->shown_height)
		y1 = (int32_t)overlay->home->shown_height;
	if (x0 < x1 && y0 < y1)
		tab_only_damage(overlay->home, x0, y0, x1, y1);
}

/* Composites one row of tab and any overlays anchored to it into out. */
static void
tab_only_composite_row(
	const struct x11_tab_only *tabs,
	const struct tab_surface *tab,
	int32_t row,
	int32_t x0,
	uint32_t width,
	uint32_t *out)
{
	const struct tab_surface *ov;
	const struct x11_rect *rect;
	unsigned index;
	unsigned shape_i;
	int32_t oy;
	int32_t sx0;
	int32_t sx1;
	int32_t rx0;
	int32_t rx1;

	memcpy(out, tab->pixels + (size_t)row * tab->shown_width + (size_t)x0, (size_t)width * sizeof(*out));

	for (index = 0U; index < tabs->overlay_count; index++) {
		ov = tabs->overlays[index];
		if (ov->home != tab || ov->pixels == NULL)
			continue;

		oy = row - ov->y;
		if (oy < 0 || oy >= (int32_t)ov->shown_height)
			continue;

		sx0 = ov->x - x0;
		sx1 = sx0 + (int32_t)ov->shown_width;
		if (sx0 < 0)
			sx0 = 0;
		if (sx1 > (int32_t)width)
			sx1 = (int32_t)width;
		if (sx0 >= sx1)
			continue;

		if (!ov->shaped) {
			memcpy(out + sx0,
			       ov->pixels + (size_t)oy * ov->shown_width + (size_t)(x0 + sx0 - ov->x),
			       (size_t)(sx1 - sx0) * sizeof(*out));
			continue;
		}

		for (shape_i = 0U; shape_i < ov->shape_count; shape_i++) {
			rect = &ov->shape[shape_i];
			if (oy < rect->y || oy >= rect->y + (int32_t)rect->height)
				continue;

			rx0 = ov->x + rect->x - x0;
			rx1 = rx0 + (int32_t)rect->width;
			if (rx0 < sx0)
				rx0 = sx0;
			if (rx1 > sx1)
				rx1 = sx1;
			if (rx0 < rx1) {
				memcpy(out + rx0,
				       ov->pixels + (size_t)oy * ov->shown_width + (size_t)(x0 + rx0 - ov->x),
				       (size_t)(rx1 - rx0) * sizeof(*out));
			}
		}
	}
}

/* Sets surface as the frontmost tab and raises it along with its overlays. */
static void
tab_only_set_front(
	struct x11_tab_only *tabs,
	struct tab_surface *surface)
{
	unsigned index;

	if (tabs->front == surface && surface->entered)
		return;

	tabs->front = surface;
	x11server_backend_enter(tabs->server, surface->window, 0);
	for (index = 0U; index < tabs->overlay_count; index++) {
		if (tabs->overlays[index]->home == surface)
			x11server_backend_enter(tabs->server, tabs->overlays[index]->window, 0);
	}
}

/* Converts root-space damage into viewport damage on a configured root tab. */
static void
tab_only_damage_root(
	struct tab_surface *surface,
	int32_t rx0,
	int32_t ry0,
	int32_t rx1,
	int32_t ry1)
{
	int32_t sx0;
	int32_t sy0;
	int32_t sx1;
	int32_t sy1;

	if (surface->kind != X11_SURFACE_ROOT || !surface->root_configured) {
		tab_only_damage(surface, rx0, ry0, rx1, ry1);
		return;
	}

	sx0 = (int32_t)floor(((double)rx0 - surface->root_offset_x) * surface->root_scale);
	sy0 = (int32_t)floor(((double)ry0 - surface->root_offset_y) * surface->root_scale);
	sx1 = (int32_t)ceil(((double)rx1 - surface->root_offset_x) * surface->root_scale) + 1;
	sy1 = (int32_t)ceil(((double)ry1 - surface->root_offset_y) * surface->root_scale) + 1;
	if (sx0 < 0)
		sx0 = 0;
	if (sy0 < 0)
		sy0 = 0;
	if (sx1 > (int32_t)surface->view_width)
		sx1 = (int32_t)surface->view_width;
	if (sy1 > (int32_t)surface->view_height)
		sy1 = (int32_t)surface->view_height;
	if (sx0 < sx1 && sy0 < sy1)
		tab_only_damage(surface, sx0, sy0, sx1, sy1);
}

/* Updates the root tab viewport size, keeping fit detent if active. */
static void
tab_only_root_configure(
	struct x11_tab_only *tabs,
	struct tab_surface *surface,
	unsigned vw,
	unsigned vh)
{
	double fit_w;
	double fit_h;
	uint8_t size[X11_LINK_TAB_SIZE_BODY];

	surface->view_width = vw;
	surface->view_height = vh;
	fit_w = (double)vw / (double)surface->shown_width;
	fit_h = (double)vh / (double)surface->shown_height;
	surface->root_fit_scale = (fit_w < fit_h) ? fit_w : fit_h;

	if (!surface->root_configured || surface->root_detent == TAB_ONLY_DETENT_FIT) {
		surface->root_configured = 1;
		surface->root_detent = TAB_ONLY_DETENT_FIT;
		surface->root_scale = surface->root_fit_scale;
		surface->root_raw_scale = surface->root_fit_scale;
		surface->root_offset_x = 0.0;
		surface->root_offset_y = 0.0;
	}

	tab_only_root_clamp(surface);

	if (surface->opened && !surface->closing) {
		x11_link_put32(size, vw);
		x11_link_put32(size + 4, vh);
		(void)tabs->link.frame(tabs->link.context, X11_LINK_TAB_SIZE, surface->window, size, sizeof(size));
	}

	surface->damaged = 0;
	tab_only_damage(surface, 0, 0, (int32_t)vw, (int32_t)vh);
}

/* Pans the root viewport by (dx, dy) screen pixels and damages the viewport. */
static void
tab_only_root_pan(
	struct tab_surface *surface,
	int32_t dx,
	int32_t dy)
{
	surface->root_offset_x -= (double)dx / surface->root_scale;
	surface->root_offset_y -= (double)dy / surface->root_scale;
	tab_only_root_clamp(surface);
	tab_only_damage(surface, 0, 0, (int32_t)surface->view_width, (int32_t)surface->view_height);
}

/* Zooms the root viewport around screen point (cx, cy) with fit and 1.0 detents. */
static void
tab_only_root_pinch(
	struct tab_surface *surface,
	int32_t cx,
	int32_t cy,
	double ratio)
{
	double min_scale;
	double raw;
	double fit;
	double old_scale;
	double new_scale;
	double target;
	double root_cx;
	double root_cy;
	double delta;
	double delta_fit;

	fit = surface->root_fit_scale;
	min_scale = fit * 0.5;
	if (min_scale < TAB_ONLY_ROOT_MIN_SCALE)
		min_scale = TAB_ONLY_ROOT_MIN_SCALE;

	raw = surface->root_raw_scale * ratio;
	if (raw < min_scale)
		raw = min_scale;
	if (raw > TAB_ONLY_ROOT_MAX_SCALE)
		raw = TAB_ONLY_ROOT_MAX_SCALE;

	old_scale = surface->root_scale;
	new_scale = raw;

	if (surface->root_detent != TAB_ONLY_DETENT_NONE) {
		target = (surface->root_detent == TAB_ONLY_DETENT_FIT) ? fit : 1.0;
		delta = fabs(raw - target);
		if (delta / target >= TAB_ONLY_DETENT_ESCAPE) {
			surface->root_detent = TAB_ONLY_DETENT_NONE;
		} else {
			new_scale = target;
		}
	}

	if (surface->root_detent == TAB_ONLY_DETENT_NONE) {
		delta = fabs(raw - 1.0);
		delta_fit = fabs(raw - fit);
		if (delta <= TAB_ONLY_DETENT_SNAP) {
			surface->root_detent = TAB_ONLY_DETENT_ONE;
			new_scale = 1.0;
		} else if (delta_fit / fit <= TAB_ONLY_DETENT_SNAP) {
			surface->root_detent = TAB_ONLY_DETENT_FIT;
			new_scale = fit;
		}
	}

	surface->root_raw_scale = raw;
	root_cx = surface->root_offset_x + (double)cx / old_scale;
	root_cy = surface->root_offset_y + (double)cy / old_scale;
	surface->root_scale = new_scale;
	surface->root_offset_x = root_cx - (double)cx / new_scale;
	surface->root_offset_y = root_cy - (double)cy / new_scale;
	tab_only_root_clamp(surface);
	tab_only_damage(surface, 0, 0, (int32_t)surface->view_width, (int32_t)surface->view_height);
}

/* Clamps root_offset_x/y so the root image stays centered when smaller or within bounds when larger. */
static void
tab_only_root_clamp(
	struct tab_surface *surface)
{
	double scaled_w;
	double scaled_h;
	double max_ox;
	double max_oy;

	scaled_w = (double)surface->shown_width * surface->root_scale;
	if (scaled_w <= (double)surface->view_width) {
		surface->root_offset_x = -((double)surface->view_width / surface->root_scale - (double)surface->shown_width) * 0.5;
	} else {
		max_ox = (double)surface->shown_width - (double)surface->view_width / surface->root_scale;
		if (surface->root_offset_x < 0.0)
			surface->root_offset_x = 0.0;
		if (surface->root_offset_x > max_ox)
			surface->root_offset_x = max_ox;
	}

	scaled_h = (double)surface->shown_height * surface->root_scale;
	if (scaled_h <= (double)surface->view_height) {
		surface->root_offset_y = -((double)surface->view_height / surface->root_scale - (double)surface->shown_height) * 0.5;
	} else {
		max_oy = (double)surface->shown_height - (double)surface->view_height / surface->root_scale;
		if (surface->root_offset_y < 0.0)
			surface->root_offset_y = 0.0;
		if (surface->root_offset_y > max_oy)
			surface->root_offset_y = max_oy;
	}
}

/* Removes an overlay from tabs->overlays while preserving open order. */
static void
tab_only_forget_overlay(
	struct x11_tab_only *tabs,
	struct tab_surface *surface)
{
	unsigned index;

	for (index = 0U; index < tabs->overlay_count; index++) {
		if (tabs->overlays[index] == surface) {
			if (index + 1U < tabs->overlay_count) {
				memmove(&tabs->overlays[index],
				        &tabs->overlays[index + 1U],
				        (size_t)(tabs->overlay_count - index - 1U) * sizeof(tabs->overlays[0]));
			}

			tabs->overlay_count -= 1U;
			break;
		}
	}
}
